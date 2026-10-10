// SPDX-License-Identifier: GPL-2.0-only
// N08 helper coverage from px4-userland 1a1485d0c3e972e0a47be907edb67949564aa9a7.
// The three original test bodies below are an exact extraction from test_main.cpp.
// Their endpoint constants describe the reference MockTransport, not ASICEN USB.
#include "px4/logging.h"
#include "px4/mock_transport.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace {

using namespace px4::userland;

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            std::fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                                   \
        }                                                                                   \
    } while (false)

struct SinkCapture final {
    std::array<LogLevel, 4> levels{};
    std::array<std::string_view, 4> messages{};
    std::size_t count = 0U;
};

void capture_log(const LogRecord& record, void* context) noexcept
{
    auto* capture = static_cast<SinkCapture*>(context);
    if (capture->count < capture->levels.size()) {
        capture->levels[capture->count] = record.level;
        capture->messages[capture->count] = record.message;
    }
    ++capture->count;
}

bool test_logging()
{
    SinkCapture capture;
    Logger logger(LogLevel::warn, capture_log, &capture);
    CHECK(!logger.enabled(LogLevel::info));
    CHECK(logger.enabled(LogLevel::warn));
    logger.log(LogLevel::info, "ignored");
    logger.log(LogLevel::warn, "warning");
    logger.log(LogLevel::error, "failure");
    CHECK(capture.count == 2U);
    CHECK(capture.levels[0] == LogLevel::warn);
    CHECK(capture.messages[0] == "warning");
    CHECK(capture.levels[1] == LogLevel::error);
    CHECK(capture.messages[1] == "failure");
    CHECK(std::strcmp(log_level_string(LogLevel::trace), "TRACE") == 0);
    CHECK(std::strcmp(log_level_string(LogLevel::debug), "DEBUG") == 0);
    CHECK(std::strcmp(log_level_string(LogLevel::info), "INFO") == 0);
    CHECK(std::strcmp(log_level_string(LogLevel::warn), "WARN") == 0);
    CHECK(std::strcmp(log_level_string(LogLevel::error), "ERROR") == 0);
    logger.set_minimum_level(LogLevel::trace);
    logger.log(LogLevel::trace, "no clock text");
    CHECK(capture.count == 3U);
    CHECK(capture.messages[2] == "no clock text");
    return true;
}

bool test_mock_transport()
{
    constexpr std::array<std::uint8_t, 3> read_response{1U, 2U, 3U};
    constexpr std::array<std::uint8_t, 2> write_request{9U, 8U};
    constexpr std::array<std::uint8_t, 4> stream_data{0x47U, 0x01U, 0x02U, 0x03U};
    const StreamConfig stream_config{kTsInEndpoint, 188U, 2U};

    MockTransport transport;
    transport.expect_bulk_read(kCommandInEndpoint,
                               ByteView{read_response.data(), read_response.size()});
    transport.expect_bulk_write(kCommandOutEndpoint,
                                ByteView{write_request.data(), write_request.size()});
    transport.expect_stream_start(stream_config);
    transport.expect_stream_event(ByteView{stream_data.data(), stream_data.size()},
                                  MockOutcome::short_transfer, 3U);
    transport.expect_stream_cancel();

    std::array<std::uint8_t, 8> output{};
    auto read = transport.bulk_read(kCommandInEndpoint,
                                    MutableByteView{output.data(), output.size()}, Timeout{100U});
    CHECK(read);
    CHECK(read.value() == read_response.size());
    CHECK(std::memcmp(output.data(), read_response.data(), read_response.size()) == 0);
    auto write = transport.bulk_write(kCommandOutEndpoint,
                                      ByteView{write_request.data(), write_request.size()}, Timeout{100U});
    CHECK(write);
    CHECK(write.value() == write_request.size());
    CHECK(transport.start_stream(stream_config));
    CHECK(transport.stream_active());
    auto event = transport.wait_stream(Timeout{100U});
    CHECK(event);
    CHECK(event.value().kind == StreamEventKind::short_transfer);
    CHECK(event.value().size == 3U);
    CHECK(event.value().data[0] == 0x47U);
    CHECK(transport.cancel_stream());
    CHECK(!transport.stream_active());
    CHECK(transport.cancel_stream());
    CHECK(transport.stop_stream());
    CHECK(transport.remaining_expectations() == 0U);

    const std::array<MockOperation, 5> expected_operations{
        MockOperation::bulk_read, MockOperation::bulk_write, MockOperation::stream_start,
        MockOperation::stream_wait, MockOperation::stream_cancel};
    CHECK(transport.operations().size() == expected_operations.size());
    for (std::size_t index = 0U; index < expected_operations.size(); ++index) {
        CHECK(transport.operations()[index] == expected_operations[index]);
    }
    return true;
}

bool test_mock_failures_and_bounds()
{
    std::array<std::uint8_t, 1> byte{};
    constexpr std::array<std::uint8_t, 3> short_response{0x21U, 0x22U, 0x23U};
    MockTransport short_transfer;
    short_transfer.expect_bulk_read(kCommandInEndpoint,
                                    ByteView{short_response.data(), short_response.size()},
                                    MockOutcome::short_transfer);
    std::array<std::uint8_t, 3> short_output{};
    BulkReadObservation short_observation{Error::USB_IO, 99U};
    auto short_read = short_transfer.bulk_read(
        kCommandInEndpoint, MutableByteView{short_output.data(), short_output.size()}, Timeout{1U},
        &short_observation);
    CHECK(short_read);
    CHECK(short_read.value() == 2U);
    CHECK(short_observation.completion_error == Error::OK);
    CHECK(short_observation.transferred == 2U);
    CHECK(short_output[0] == 0x21U && short_output[1] == 0x22U);

    MockTransport partial_timeout;
    std::array<std::uint8_t, 512U> timeout_response{};
    partial_timeout.expect_bulk_read(kCommandInEndpoint,
                                     ByteView{timeout_response.data(), timeout_response.size()},
                                     MockOutcome::timeout, timeout_response.size());
    BulkReadObservation partial_timeout_observation{Error::OK, 99U};
    auto partial_timeout_read = partial_timeout.bulk_read(
        kCommandInEndpoint,
        MutableByteView{timeout_response.data(), timeout_response.size()}, Timeout{1U},
        &partial_timeout_observation);
    CHECK(partial_timeout_read && partial_timeout_read.value() == timeout_response.size());
    CHECK(partial_timeout_observation.completion_error == Error::TIMEOUT);
    CHECK(partial_timeout_observation.transferred == timeout_response.size());

    MockTransport zero_timeout;
    zero_timeout.expect_bulk_read(kCommandInEndpoint, ByteView{nullptr, 0U},
                                  MockOutcome::timeout);
    BulkReadObservation zero_timeout_observation{Error::OK, 99U};
    auto zero_timeout_read = zero_timeout.bulk_read(
        kCommandInEndpoint, MutableByteView{byte.data(), byte.size()}, Timeout{1U},
        &zero_timeout_observation);
    CHECK(!zero_timeout_read && zero_timeout_read.error() == Error::TIMEOUT);
    CHECK(zero_timeout_observation.completion_error == Error::TIMEOUT);
    CHECK(zero_timeout_observation.transferred == 0U);

    MockTransport invalid_transfer;
    invalid_transfer.expect_bulk_read(kCommandInEndpoint, ByteView{short_response.data(), 1U},
                                      MockOutcome::success, 2U);
    BulkReadObservation invalid_transfer_observation{Error::OK, 99U};
    auto invalid_transfer_read = invalid_transfer.bulk_read(
        kCommandInEndpoint, MutableByteView{byte.data(), byte.size()}, Timeout{1U},
        &invalid_transfer_observation);
    CHECK(!invalid_transfer_read && invalid_transfer_read.error() == Error::INTERNAL);
    CHECK(invalid_transfer_observation.completion_error == Error::INTERNAL);
    CHECK(invalid_transfer_observation.transferred == 0U);

    MockTransport bounds;
    constexpr std::size_t max_command_size = kMaxCommandTransfer;
    std::array<std::uint8_t, max_command_size> max_command_data{};
    bounds.expect_bulk_read(kCommandInEndpoint,
                            ByteView{max_command_data.data(), max_command_data.size()});
    auto max_read = bounds.bulk_read(kCommandInEndpoint,
                                     MutableByteView{max_command_data.data(), max_command_data.size()},
                                     Timeout{1U});
    CHECK(max_read);
    CHECK(max_read.value() == max_command_size);
    auto too_large_read = bounds.bulk_read(kCommandInEndpoint,
                                           MutableByteView{byte.data(), kMaxCommandTransfer + 1U},
                                           Timeout{1U});
    CHECK(!too_large_read);
    CHECK(too_large_read.error() == Error::BUFFER_TOO_SMALL);
    bounds.expect_bulk_write(kCommandOutEndpoint,
                             ByteView{max_command_data.data(), max_command_data.size()});
    auto max_write = bounds.bulk_write(kCommandOutEndpoint,
                                       ByteView{max_command_data.data(), max_command_data.size()},
                                       Timeout{1U});
    CHECK(max_write);
    CHECK(max_write.value() == max_command_size);
    auto too_large_write = bounds.bulk_write(kCommandOutEndpoint,
                                             ByteView{byte.data(), kMaxCommandTransfer + 1U}, Timeout{1U});
    CHECK(!too_large_write);
    CHECK(too_large_write.error() == Error::BUFFER_TOO_SMALL);

    MockTransport null_pointer;
    auto null_read = null_pointer.bulk_read(kCommandInEndpoint,
                                            MutableByteView{nullptr, 1U}, Timeout{1U});
    CHECK(!null_read);
    CHECK(null_read.error() == Error::INVALID_ARGUMENT);
    auto null_write = null_pointer.bulk_write(kCommandOutEndpoint, ByteView{nullptr, 1U}, Timeout{1U});
    CHECK(!null_write);
    CHECK(null_write.error() == Error::INVALID_ARGUMENT);
    CHECK(null_pointer.operations().empty());

    MockTransport stream_bounds;
    const StreamConfig too_large_stream{kTsInEndpoint, kMaxStreamTransfer + 1U, 1U};
    auto too_large_stream_result = stream_bounds.start_stream(too_large_stream);
    CHECK(!too_large_stream_result);
    CHECK(too_large_stream_result.error() == Error::INVALID_ARGUMENT);
    const StreamConfig wrong_endpoint{ kCommandInEndpoint, 188U, 1U };
    CHECK(stream_bounds.start_stream(wrong_endpoint).error() == Error::INVALID_ARGUMENT);

    MockTransport ordering;
    constexpr std::array<std::uint8_t, 1> expected{0x11U};
    ordering.expect_bulk_read(kCommandInEndpoint, ByteView{expected.data(), expected.size()});
    auto wrong_order = ordering.bulk_write(kCommandOutEndpoint,
                                           ByteView{expected.data(), expected.size()}, Timeout{1U});
    CHECK(!wrong_order);
    CHECK(wrong_order.error() == Error::PROTOCOL_ERROR);
    CHECK(ordering.remaining_expectations() == 1U);

    const MockOutcome outcomes[] = {MockOutcome::timeout, MockOutcome::disconnect,
                                    MockOutcome::protocol_error};
    const Error errors[] = {Error::TIMEOUT, Error::DISCONNECTED, Error::PROTOCOL_ERROR};
    for (std::size_t index = 0U; index < 3U; ++index) {
        MockTransport failing;
        failing.expect_bulk_read(kCommandInEndpoint, ByteView{nullptr, 0U}, outcomes[index]);
        auto result = failing.bulk_read(kCommandInEndpoint,
                                        MutableByteView{byte.data(), byte.size()}, Timeout{1U});
        CHECK(!result);
        CHECK(result.error() == errors[index]);
    }

    MockTransport stream_failure;
    const StreamConfig config{kTsInEndpoint, 188U, 1U};
    stream_failure.expect_stream_start(config);
    stream_failure.expect_stream_event(ByteView{nullptr, 0U}, MockOutcome::timeout);
    stream_failure.expect_stream_stop();
    CHECK(stream_failure.start_stream(config));
    auto timed_out = stream_failure.wait_stream(Timeout{1U});
    CHECK(!timed_out);
    CHECK(timed_out.error() == Error::TIMEOUT);
    CHECK(stream_failure.stream_active());
    CHECK(stream_failure.stop_stream());
    CHECK(stream_failure.stop_stream());
    CHECK(stream_failure.remaining_expectations() == 0U);
    return true;
}


bool test_logging_boundaries_and_sink_replacement()
{
    SinkCapture first;
    SinkCapture second;
    Logger logger;
    CHECK(!logger.enabled(LogLevel::debug));
    CHECK(logger.enabled(LogLevel::info));
    logger.log(LogLevel::error, "null sink");
    logger.set_sink(capture_log, &first);
    const std::array<LogLevel, 5> levels{
        LogLevel::trace, LogLevel::debug, LogLevel::info, LogLevel::warn, LogLevel::error};
    for (std::size_t minimum = 0U; minimum < levels.size(); ++minimum) {
        logger.set_minimum_level(levels[minimum]);
        first.count = 0U;
        for (std::size_t level = 0U; level < levels.size(); ++level) {
            CHECK(logger.enabled(levels[level]) == (level >= minimum));
            logger.log(levels[level], "boundary");
        }
        CHECK(first.count == levels.size() - minimum);
    }
    logger.set_minimum_level(LogLevel::trace);
    logger.set_sink(capture_log, &second);
    logger.log(LogLevel::trace, std::string_view{"a\0b", 3U});
    CHECK(second.count == 1U && second.messages[0].size() == 3U);
    CHECK(first.count == 1U);
    logger.set_sink(nullptr, nullptr);
    logger.log(LogLevel::error, "disabled sink");
    CHECK(second.count == 1U);
    CHECK(std::strcmp(log_level_string(static_cast<LogLevel>(255U)), "UNKNOWN") == 0);
    return true;
}

bool test_mock_validation_and_observations()
{
    constexpr std::array<std::uint8_t, 3> payload{1U, 2U, 3U};
    std::array<std::uint8_t, 3> output{};
    const ByteView input{payload.data(), payload.size()};
    const MutableByteView buffer{output.data(), output.size()};
    MockTransport transport;
    transport.expect_bulk_read(0x82U, input);
    BulkReadObservation observation{Error::USB_IO, 99U};
    for (const std::uint8_t endpoint : {0x00U, 0x02U}) {
        CHECK(transport.bulk_read(endpoint, buffer, Timeout{7U}, &observation).error() ==
              Error::INVALID_ARGUMENT);
        CHECK(observation.completion_error == Error::INVALID_ARGUMENT);
        CHECK(observation.transferred == 0U);
    }
    for (const std::uint8_t endpoint : {0x00U, 0x81U}) {
        CHECK(transport.bulk_write(endpoint, input, Timeout{8U}).error() ==
              Error::INVALID_ARGUMENT);
    }
    CHECK(transport.operations().empty());
    CHECK(transport.timeouts().empty());
    CHECK(transport.bulk_read_sizes().empty());
    CHECK(transport.remaining_expectations() == 1U);

    // Wrong operation records the attempted call but does not consume the FIFO.
    CHECK(transport.bulk_write(0x02U, input, Timeout{11U}).error() == Error::PROTOCOL_ERROR);
    CHECK(transport.remaining_expectations() == 1U);
    CHECK(transport.bulk_read(0x82U, buffer, Timeout{13U}, &observation));
    CHECK(observation.completion_error == Error::OK && observation.transferred == 3U);
    CHECK(transport.remaining_expectations() == 0U);
    CHECK(transport.operations().size() == 2U);
    CHECK(transport.operations()[0] == MockOperation::bulk_write);
    CHECK(transport.operations()[1] == MockOperation::bulk_read);
    CHECK(transport.timeouts().size() == 2U);
    CHECK(transport.timeouts()[0].milliseconds == 11U);
    CHECK(transport.timeouts()[1].milliseconds == 13U);
    CHECK(transport.bulk_read_sizes().size() == 1U);
    CHECK(transport.bulk_read_sizes()[0] == output.size());

    MockTransport wrong_endpoint;
    wrong_endpoint.expect_bulk_read(0x81U, input);
    CHECK(wrong_endpoint.bulk_read(0x82U, buffer, Timeout{1U}).error() ==
          Error::PROTOCOL_ERROR);
    CHECK(wrong_endpoint.remaining_expectations() == 0U);

    MockTransport writes;
    writes.expect_bulk_write(0x02U, input, MockOutcome::short_transfer);
    const auto short_write = writes.bulk_write(0x02U, input, Timeout{17U});
    CHECK(short_write && short_write.value() == 2U);
    writes.expect_bulk_write(0x02U, input);
    CHECK(writes.bulk_write(0x02U, ByteView{output.data(), 1U}, Timeout{1U}).error() ==
          Error::PROTOCOL_ERROR);
    writes.expect_bulk_write(0x02U, input, MockOutcome::usb_io);
    CHECK(writes.bulk_write(0x02U, input, Timeout{1U}).error() == Error::USB_IO);

    MockTransport zero_length;
    zero_length.expect_bulk_read(0x81U, ByteView{nullptr, 0U});
    const auto empty_read = zero_length.bulk_read(0x81U, MutableByteView{nullptr, 0U}, Timeout{0U});
    CHECK(empty_read && empty_read.value() == 0U);
    zero_length.expect_bulk_write(0x02U, ByteView{nullptr, 0U});
    const auto empty_write = zero_length.bulk_write(0x02U, ByteView{nullptr, 0U}, Timeout{0U});
    CHECK(empty_write && empty_write.value() == 0U);

    MockTransport failure;
    failure.expect_bulk_read(0x82U, input, MockOutcome::usb_io, 2U);
    output.fill(0U);
    CHECK(failure.bulk_read(0x82U, buffer, Timeout{1U}, &observation).error() == Error::USB_IO);
    CHECK(observation.completion_error == Error::USB_IO && observation.transferred == 2U);
    CHECK(output[0] == 0U && output[1] == 0U);

    MockTransport payload_overflow;
    payload_overflow.expect_bulk_read(0x82U, input);
    CHECK(payload_overflow.bulk_read(
              0x82U, MutableByteView{output.data(), 2U}, Timeout{1U}).error() ==
          Error::BUFFER_TOO_SMALL);
    MockTransport transferred_overflow;
    transferred_overflow.expect_bulk_read(0x82U, ByteView{payload.data(), 1U},
                                          MockOutcome::success, 2U);
    CHECK(transferred_overflow.bulk_read(0x82U, buffer, Timeout{1U}).error() ==
          Error::PROTOCOL_ERROR);
    return true;
}

bool test_mock_reference_stream_lifecycle()
{
    // This is the pinned helper's reference stream policy. ASICEN stream
    // endpoints remain 0x81/0x82 and are tested through CaptureBackend separately.
    const StreamConfig config{kTsInEndpoint, 188U, 2U};
    MockTransport transport;
    for (const StreamConfig invalid : {
             StreamConfig{0x82U, 188U, 2U}, StreamConfig{kTsInEndpoint, 0U, 2U},
             StreamConfig{kTsInEndpoint, 188U, 0U},
             StreamConfig{kTsInEndpoint, kMaxStreamTransfer + 1U, 2U}}) {
        CHECK(transport.start_stream(invalid).error() == Error::INVALID_ARGUMENT);
    }
    CHECK(transport.operations().empty());
    CHECK(transport.wait_stream(Timeout{1U}).error() == Error::NOT_READY);
    transport.expect_stream_start(config);
    transport.expect_stream_event(ByteView{nullptr, 0U}, MockOutcome::disconnect);
    CHECK(transport.start_stream(config));
    CHECK(transport.start_stream(config).error() == Error::BUSY);
    CHECK(transport.wait_stream(Timeout{19U}).error() == Error::DISCONNECTED);
    CHECK(!transport.stream_active());
    CHECK(transport.stop_stream());
    CHECK(transport.cancel_stream());
    CHECK(transport.operations().size() == 2U);
    // Pinned API records bulk timeouts only; wait_stream's timeout is ignored.
    CHECK(transport.timeouts().empty());

    for (const bool cancel : {false, true}) {
        MockTransport cleanup;
        cleanup.expect_stream_start(config);
        if (cancel) {
            cleanup.expect_stream_cancel(MockOutcome::usb_io);
        } else {
            cleanup.expect_stream_stop(MockOutcome::usb_io);
        }
        CHECK(cleanup.start_stream(config));
        CHECK((cancel ? cleanup.cancel_stream() : cleanup.stop_stream()).error() ==
              Error::USB_IO);
        CHECK(!cleanup.stream_active());
        CHECK(cleanup.cancel_stream());
        CHECK(cleanup.stop_stream());
        CHECK(cleanup.remaining_expectations() == 0U);
        CHECK(cleanup.operations().size() == 2U);
    }

    MockTransport failed_start;
    failed_start.expect_stream_start(config, MockOutcome::timeout);
    CHECK(failed_start.start_stream(config).error() == Error::TIMEOUT);
    CHECK(!failed_start.stream_active());

    MockTransport mismatched_config;
    mismatched_config.expect_stream_start(config);
    CHECK(mismatched_config.start_stream(
              StreamConfig{kTsInEndpoint, 188U, 3U}).error() == Error::PROTOCOL_ERROR);
    CHECK(!mismatched_config.stream_active());

    constexpr std::array<std::uint8_t, 3> payload{0x47U, 0x01U, 0x02U};
    MockTransport oversized_event;
    const StreamConfig small{kTsInEndpoint, 2U, 1U};
    oversized_event.expect_stream_start(small);
    oversized_event.expect_stream_event(ByteView{payload.data(), payload.size()});
    oversized_event.expect_stream_stop();
    CHECK(oversized_event.start_stream(small));
    CHECK(oversized_event.wait_stream(Timeout{1U}).error() == Error::PROTOCOL_ERROR);
    CHECK(oversized_event.stop_stream());
    CHECK(oversized_event.remaining_expectations() == 0U);
    return true;
}

}  // namespace

int main()
{
    struct Test final {
        const char* name;
        bool (*run)();
    };
    const Test tests[] = {
        {"logging", test_logging},
        {"mock_transport", test_mock_transport},
        {"mock_failures_and_bounds", test_mock_failures_and_bounds},
        {"logging_boundaries_and_sink_replacement", test_logging_boundaries_and_sink_replacement},
        {"mock_validation_and_observations", test_mock_validation_and_observations},
        {"mock_reference_stream_lifecycle", test_mock_reference_stream_lifecycle},
    };
    for (const Test& test : tests) {
        if (!test.run()) {
            std::fprintf(stderr, "FAIL %s\n", test.name);
            return 1;
        }
        std::printf("PASS %s\n", test.name);
    }
    return 0;
}
