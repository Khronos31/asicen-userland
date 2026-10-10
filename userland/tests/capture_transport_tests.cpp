// SPDX-License-Identifier: GPL-2.0-only
#include "asicen/stream_capture.h"
#include "px4/mock_transport.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <vector>

namespace {

using namespace px4::userland;

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            std::fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                                   \
        }                                                                                   \
    } while (false)

// The production raw-capture loop receives the exact pinned transport's FIFO
// oracle through its existing seam. ASICEN DSC start/stop remain local controls;
// no IT930x command packets or reference 0x84 stream endpoint are substituted.
class MockCaptureBackend final : public asicen::CaptureBackend {
public:
    explicit MockCaptureBackend(MockTransport& transport) noexcept : transport_(transport) {}

    bool dsc_start(std::uint8_t local) override
    {
        start_local = local;
        ++starts;
        return start_ok;
    }

    bool dsc_stop(std::uint8_t local) override
    {
        stop_local = local;
        ++stops;
        return stop_ok;
    }

    asicen::CaptureIo bulk_read(std::uint8_t endpoint, unsigned char* data, int length,
                               int* transferred, unsigned timeout_ms) override
    {
        BulkReadObservation observation{};
        const auto result = transport_.bulk_read(
            endpoint, MutableByteView{data, static_cast<std::size_t>(length)},
            Timeout{timeout_ms}, &observation);
        *transferred = static_cast<int>(observation.transferred);
        observations.push_back(observation);
        // A partial timeout has a successful byte-count result, but the
        // transport completion remains TIMEOUT. Preserve both pieces of data.
        if (observation.completion_error == Error::TIMEOUT) {
            return asicen::CaptureIo::Timeout;
        }
        return result ? asicen::CaptureIo::Ok : asicen::CaptureIo::Error;
    }

    bool cancelled() const override { return cancel; }

    std::vector<BulkReadObservation> observations;
    unsigned starts = 0U;
    unsigned stops = 0U;
    std::uint8_t start_local = 255U;
    std::uint8_t stop_local = 255U;
    bool start_ok = true;
    bool stop_ok = true;
    bool cancel = false;

private:
    MockTransport& transport_;
};

class CaptureOutput final : public asicen::CaptureOutput {
public:
    bool write(const unsigned char* data, std::size_t size) override
    {
        if (fail) {
            return false;
        }
        bytes.insert(bytes.end(), data, data + size);
        return true;
    }

    std::vector<std::uint8_t> bytes;
    bool fail = false;
};

asicen::CaptureRequest request(std::uint8_t local, std::uint64_t limit)
{
    asicen::CaptureRequest result{};
    result.local = local;
    result.endpoint = static_cast<std::uint8_t>(0x81U + local);
    result.chunk_size = 4U;
    result.byte_limit = limit;
    result.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    return result;
}

bool test_asicen_short_reads_and_timeout_observations()
{
    constexpr std::array<std::uint8_t, 4> timeout_data{0xEEU, 0xEEU, 0xEEU, 0xEEU};
    constexpr std::array<std::uint8_t, 4> first{0x47U, 0x01U, 0x02U, 0x03U};
    constexpr std::array<std::uint8_t, 4> last{0x04U, 0x05U, 0x06U, 0x07U};
    for (const std::uint8_t local : {0U, 1U}) {
        auto capture_request = request(local, 7U);
        MockTransport transport;
        transport.expect_bulk_read(capture_request.endpoint, ByteView{nullptr, 0U},
                                   MockOutcome::timeout);
        transport.expect_bulk_read(capture_request.endpoint,
                                   ByteView{timeout_data.data(), timeout_data.size()},
                                   MockOutcome::timeout, timeout_data.size());
        transport.expect_bulk_read(capture_request.endpoint, ByteView{first.data(), first.size()});
        transport.expect_bulk_read(capture_request.endpoint, ByteView{last.data(), last.size()},
                                   MockOutcome::short_transfer);
        MockCaptureBackend backend(transport);
        CaptureOutput output;
        asicen::CaptureStats stats{};
        CHECK(asicen::run_raw_capture(&backend, &output, capture_request, &stats) ==
              asicen::CaptureOutcome::Completed);
        CHECK(output.bytes == std::vector<std::uint8_t>({0x47U, 1U, 2U, 3U, 4U, 5U, 6U}));
        CHECK(stats.bytes == 7U && stats.limit_reached);
        CHECK(backend.starts == 1U && backend.stops == 1U);
        CHECK(backend.start_local == local && backend.stop_local == local);
        CHECK(backend.observations.size() == 4U);
        CHECK(backend.observations[0].completion_error == Error::TIMEOUT);
        CHECK(backend.observations[0].transferred == 0U);
        CHECK(backend.observations[1].completion_error == Error::TIMEOUT);
        CHECK(backend.observations[1].transferred == timeout_data.size());
        CHECK(backend.observations[3].completion_error == Error::OK);
        CHECK(backend.observations[3].transferred == 3U);
        CHECK(transport.remaining_expectations() == 0U);
        CHECK(transport.operations().size() == 4U);
        CHECK(transport.timeouts().size() == 4U);
        CHECK(transport.bulk_read_sizes().size() == 4U);
        for (std::size_t index = 0U; index < transport.operations().size(); ++index) {
            CHECK(transport.operations()[index] == MockOperation::bulk_read);
            CHECK(transport.timeouts()[index].milliseconds > 0U);
            CHECK(transport.timeouts()[index].milliseconds <= 1000U);
            CHECK(transport.bulk_read_sizes()[index] == capture_request.chunk_size);
        }
    }
    return true;
}

bool test_asicen_capture_error_cleanup()
{
    constexpr std::array<std::uint8_t, 4> payload{0x47U, 0x01U, 0x02U, 0x03U};
    for (const MockOutcome failure : {MockOutcome::disconnect, MockOutcome::usb_io,
                                      MockOutcome::protocol_error}) {
        MockTransport transport;
        transport.expect_bulk_read(0x82U, ByteView{payload.data(), payload.size()},
                                   failure, payload.size());
        MockCaptureBackend backend(transport);
        CaptureOutput output;
        asicen::CaptureStats stats{};
        CHECK(asicen::run_raw_capture(&backend, &output, request(1U, payload.size()), &stats) ==
              asicen::CaptureOutcome::UsbFailed);
        CHECK(output.bytes.empty() && stats.bytes == 0U && !stats.limit_reached);
        CHECK(backend.stops == 1U && backend.stop_local == 1U);
        CHECK(transport.remaining_expectations() == 0U);
    }
    for (const bool output_failure : {false, true}) {
        MockTransport transport;
        transport.expect_bulk_read(0x81U, ByteView{payload.data(), payload.size()});
        MockCaptureBackend backend(transport);
        backend.stop_ok = output_failure;
        CaptureOutput output;
        output.fail = output_failure;
        const auto expected = output_failure ? asicen::CaptureOutcome::OutputFailed
                                             : asicen::CaptureOutcome::StopFailed;
        CHECK(asicen::run_raw_capture(&backend, &output, request(0U, payload.size()), nullptr) ==
              expected);
        CHECK(backend.stops == 1U && backend.stop_local == 0U);
        CHECK(transport.remaining_expectations() == 0U);
    }
    for (const bool start_failure : {false, true}) {
        MockTransport transport;
        MockCaptureBackend backend(transport);
        backend.start_ok = !start_failure;
        backend.cancel = !start_failure;
        CaptureOutput output;
        CHECK(asicen::run_raw_capture(&backend, &output, request(1U, 4U), nullptr) ==
              (start_failure ? asicen::CaptureOutcome::StartFailed
                             : asicen::CaptureOutcome::Cancelled));
        CHECK(backend.starts == 1U && backend.stops == (start_failure ? 0U : 1U));
        CHECK(transport.operations().empty() && output.bytes.empty());
    }
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
        {"asicen_short_reads_and_timeout_observations", test_asicen_short_reads_and_timeout_observations},
        {"asicen_capture_error_cleanup", test_asicen_capture_error_cleanup},
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
