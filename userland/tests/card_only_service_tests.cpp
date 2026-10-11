// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/card_mailbox_hardware.h"
#include "asicen/card_only_service.h"
#include "test_temp_directory.h"
#include "px4/control_client.h"
#include "asicen/product_profile.h"
#include "px4/card_service.h"
#include "px4/posix_ipc.h"
#include "px4/control_server.h"
#include "px4/pcsc_ifd_adapter.h"
#include "px4/posix_tuner_nonce.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

namespace {
using namespace px4::userland;
using namespace px4::userland::ipc;
using namespace px4::userland::ipc::posix;
using namespace px4::userland::pcsc;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::fprintf(stderr, "check failed %s:%d: %s\n", __FILE__, __LINE__, #condition);      \
            return false;                                                                          \
        }                                                                                          \
    } while (false)

class SingleFrame final : public FrameConsumer {
public:
    Result<void> on_frame(const FrameView& frame) noexcept override
    {
        header = frame.header;
        if (frame.payload.size > payload.size()) {
            return Result<void>::failure(Error::BUFFER_TOO_SMALL);
        }
        std::copy(frame.payload.data, frame.payload.data + frame.payload.size, payload.begin());
        payload_size = frame.payload.size;
        received = true;
        return Result<void>::success();
    }
    FrameHeader header{};
    std::array<std::uint8_t, kMaxControlPayload> payload{};
    std::size_t payload_size = 0U;
    bool received = false;
};

Result<void> send_raw_request(SocketStream& stream, MessageType type, std::uint32_t request_id,
                              ByteView payload)
{
    std::array<std::uint8_t, kFrameHeaderSize + kMaxControlPayload> frame{};
    const auto encoded =
        encode_frame(FrameHeader{kProtocolMajor, kProtocolMinor, type, MessageKind::request,
                                 request_id, static_cast<std::uint32_t>(payload.size)},
                     payload, MutableByteView{frame.data(), frame.size()});
    if (!encoded) {
        return Result<void>::failure(encoded.error());
    }
    return stream.write_frame(ByteView{frame.data(), encoded.value()}, Timeout{2000U});
}

Result<void> read_one_raw_response(SocketStream& stream, StreamFramer& framer,
                                   SingleFrame& response)
{
    std::array<std::uint8_t, 8192U> read_buffer{};
    for (unsigned attempt = 0U; attempt < 20U && !response.received; ++attempt) {
        const auto read =
            stream.read_frames(MutableByteView{read_buffer.data(), read_buffer.size()}, framer,
                               response, Timeout{100U});
        if (!read) {
            return Result<void>::failure(read.error());
        }
    }
    return response.received ? Result<void>::success() : Result<void>::failure(Error::TIMEOUT);
}

std::uint8_t reverse_bits(std::uint8_t value)
{
    std::uint8_t output = 0U;
    for (unsigned i = 0; i < 8U; ++i) {
        output = static_cast<std::uint8_t>((output << 1U) | ((value >> i) & 1U));
    }
    return output;
}

std::vector<std::uint8_t> t1_frame(std::uint8_t pcb, const std::vector<std::uint8_t>& data)
{
    std::vector<std::uint8_t> frame{0U, pcb, static_cast<std::uint8_t>(data.size())};
    frame.insert(frame.end(), data.begin(), data.end());
    std::uint8_t lrc = 0U;
    for (const auto byte : frame) {
        lrc = static_cast<std::uint8_t>(lrc ^ byte);
    }
    frame.push_back(lrc);
    return frame;
}

class MailboxFixture final : public asicen::FrontendTransport {
public:
    int control(const asicen::ControlTransfer& transfer, unsigned char* output) override
    {
        if (output == nullptr || transfer.length == 0U) {
            return -1;
        }
        ++calls;
        std::fill_n(output, transfer.length, 0U);
        if (transfer.request == asicen::Request::I2cRead) {
            const auto reg = static_cast<std::uint8_t>(transfer.value >> 8U);
            if (static_cast<std::uint8_t>(transfer.value & 0xffU) != 0x4aU) {
                return -1;
            }
            output[0] = 1U;
            for (std::size_t i = 0U; i + 1U < transfer.length; ++i) {
                const auto address = static_cast<std::uint8_t>(reg + i);
                if (address >= 0x40U && address < 0x80U) {
                    const std::size_t offset =
                        static_cast<std::size_t>(page) * 64U + address - 0x40U;
                    output[i + 1U] = window[offset];
                } else {
                    output[i + 1U] = registers[address];
                }
            }
            return transfer.length;
        }
        if (transfer.request == asicen::Request::I2cWrite) {
            const auto reg = static_cast<std::uint8_t>(transfer.value >> 8U);
            const auto value = static_cast<std::uint8_t>(transfer.index & 0xffU);
            output[0] = (fail_cleanup && (reg == 0x04U || reg == 0x00U)) ? 0U : 1U;
            if (output[0] == 0U) {
                return transfer.length;
            }
            registers[reg] = value;
            if (reg == 0x3aU) {
                page = value;
            }
            if (reg == 0x00U && value == 0x80U) {
                ++reset_prepares;
            }
            if (reg == 0x00U && value == 0U && reset_prepares != 0U) {
                ++controller_inits;
                registers[0x04U] = card_present ? 0x81U : 0x80U;
            }
            if (reg == 0x00U && value == 0x09U) {
                load_atr();
            }
            if (reg == 0x00U && value == 0x0aU) {
                load_next();
            }
            if (reg == 0x00U && value == 0x0cU) {
                registers[0x03U] = 0U;
            }
            return transfer.length;
        }
        if (transfer.request == asicen::Request::I2cBufferFill) {
            output[0] = 1U;
            const std::size_t offset = transfer.value & 0xffU;
            const std::array<std::uint8_t, 3U> bytes{
                {static_cast<std::uint8_t>(transfer.value >> 8U),
                 static_cast<std::uint8_t>(transfer.index & 0xffU),
                 static_cast<std::uint8_t>(transfer.index >> 8U)}};
            for (std::size_t i = 0U; i + 1U < transfer.length; ++i) {
                staging[offset + i] = bytes[i];
            }
            return transfer.length;
        }
        if (transfer.request == asicen::Request::I2cBufferSend) {
            output[0] = 1U;
            const auto reg = staging[0];
            for (std::size_t i = 1U; i + 1U < transfer.length; ++i) {
                const std::size_t offset =
                    static_cast<std::size_t>(page) * 64U + reg - 0x40U + i - 1U;
                window[offset] = staging[i];
            }
            return transfer.length;
        }
        return -1;
    }
    void delay_ms(unsigned) override {}
    bool cancelled() const override { return false; }
    bool expired() const override { return false; }

    void queue(std::vector<std::uint8_t> frame) { pending.push_back(std::move(frame)); }
    void load_frame(const std::vector<std::uint8_t>& frame)
    {
        registers[0x04U] = card_present ? 0x81U : 0x80U;
        registers[0x03U] = static_cast<std::uint8_t>(frame.size());
        registers[0x39U] = static_cast<std::uint8_t>((frame.size() >> 8U) & 1U);
        std::fill(window.begin(), window.end(), 0U);
        for (std::size_t i = 0; i < frame.size(); ++i) {
            window[i] = reverse_bits(frame[i]);
        }
    }
    void load_atr()
    {
        load_frame({0x3bU, 0xf0U, 0x12U, 0x00U, 0xffU, 0x91U, 0x81U, 0xb1U, 0x7cU, 0x45U, 0x1fU,
                    0x01U, 0x9bU});
    }
    void load_next()
    {
        if (pending.empty()) {
            return;
        }
        load_frame(pending.front());
        pending.pop_front();
    }

    std::array<std::uint8_t, 256U> registers{};
    std::array<std::uint8_t, 512U> window{};
    std::array<std::uint8_t, 64U> staging{};
    std::deque<std::vector<std::uint8_t>> pending;
    std::atomic<unsigned> calls{0U};
    unsigned reset_prepares = 0U;
    std::atomic<unsigned> controller_inits{0U};
    std::uint8_t page = 0U;
    bool card_present = true;
    bool fail_cleanup = false;
};

class FixtureOperationGuard final : public asicen::CardOperationGuard {
public:
    px4::userland::Result<void>
    begin_card_operation(std::uint32_t timeout_ms,
                         const volatile std::sig_atomic_t* stop_flag) noexcept override
    {
        if (entry_error != px4::userland::Error::OK) {
            return px4::userland::Result<void>::failure(entry_error);
        }
        if (quarantined || stopped || timeout_ms == 0U ||
            (stop_flag != nullptr && *stop_flag != 0)) {
            return px4::userland::Result<void>::failure(px4::userland::Error::NOT_READY);
        }
        ++operations;
        last_operation_timeout = timeout_ms;
        return px4::userland::Result<void>::success();
    }
    void end_card_operation() noexcept override {}
    px4::userland::Result<void> begin_card_cleanup(std::uint32_t timeout_ms) noexcept override
    {
        if (entry_error != px4::userland::Error::OK) {
            return px4::userland::Result<void>::failure(entry_error);
        }
        if (quarantined || timeout_ms == 0U) {
            return px4::userland::Result<void>::failure(px4::userland::Error::USB_IO);
        }
        ++cleanups;
        last_cleanup_timeout = timeout_ms;
        return px4::userland::Result<void>::success();
    }
    void end_card_cleanup(bool success) noexcept override
    {
        if (!success) {
            quarantined = true;
        }
    }
    void request_card_stop() noexcept override { stopped = true; }

    std::atomic<unsigned> operations{0U};
    std::atomic<unsigned> cleanups{0U};
    std::atomic<std::uint32_t> last_operation_timeout{0U};
    std::atomic<std::uint32_t> last_cleanup_timeout{0U};
    std::atomic<bool> quarantined{false};
    std::atomic<bool> stopped{false};
    px4::userland::Error entry_error = px4::userland::Error::OK;

private:
};

class ObservedProtocolSession final : public CardProtocolSession {
public:
    ObservedProtocolSession(CardProtocolSession& session, FixtureOperationGuard& guard) noexcept
        : session_(session), guard_(guard)
    {
    }

    Result<void> initialize() noexcept override
    {
        const auto result = session_.initialize();
        // The server's card worker serializes protocol operations and presence
        // polling. Record this operation before a later detect_card() replaces
        // the guard's latest timeout with its independent 5000 ms budget.
        initialize_timeout.store(guard_.last_operation_timeout.load());
        return result;
    }
    Result<std::size_t> transmit(ByteView apdu, MutableByteView response) noexcept override
    {
        const auto result = session_.transmit(apdu, response);
        transmit_timeout.store(guard_.last_operation_timeout.load());
        return result;
    }
    bool initialized() const noexcept override { return session_.initialized(); }
    const CardAtr& atr() const noexcept override { return session_.atr(); }
    void invalidate() noexcept override { session_.invalidate(); }
    void request_stop() noexcept override { session_.request_stop(); }

    std::atomic<std::uint32_t> initialize_timeout{0U};
    std::atomic<std::uint32_t> transmit_timeout{0U};

private:
    CardProtocolSession& session_;
    FixtureOperationGuard& guard_;
};

bool test_protocol_timeouts_survive_presence_poll()
{
    MailboxFixture transport;
    transport.queue(t1_frame(0xe0U, {}));
    transport.queue(t1_frame(0xe1U, {251U}));
    transport.queue(t1_frame(0x00U, {0x90U, 0x00U}));
    asicen::W3u3CardMailboxHardware mailbox(transport);
    CardSession raw_session(mailbox, mailbox);
    FixtureOperationGuard guard;
    asicen::W3u3CardServiceBackend backend(mailbox, guard, nullptr);
    asicen::W3u3CardProtocolSession session(raw_session, guard, nullptr);
    ObservedProtocolSession observed_session(session, guard);

    CHECK(observed_session.initialize());
    CHECK(guard.last_operation_timeout == 15000U);
    const auto present = backend.detect_card();
    CHECK(present && present.value());
    CHECK(guard.last_operation_timeout == 5000U);
    CHECK(observed_session.initialize_timeout == 15000U);

    const std::array<std::uint8_t, 5U> apdu{{0x90U, 0x30U, 0U, 0U, 0U}};
    std::array<std::uint8_t, 2U> response{};
    const auto transmitted = observed_session.transmit(
        ByteView{apdu.data(), apdu.size()}, MutableByteView{response.data(), response.size()});
    CHECK(transmitted && transmitted.value() == 2U);
    CHECK(response[0] == 0x90U && response[1] == 0U);
    CHECK(backend.detect_card());
    CHECK(guard.last_operation_timeout == 5000U);
    CHECK(observed_session.transmit_timeout == 15000U);
    return true;
}

class TestTime final : public TunerServiceTime {
public:
    std::uint64_t monotonic_ms() noexcept override
    {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::steady_clock::now().time_since_epoch())
                                              .count());
    }
    void sleep_ms(std::uint32_t ms) noexcept override
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
};

class JoinOnExit final {
public:
    JoinOnExit(std::atomic<bool>& stop, std::thread& thread) : stop_(stop), thread_(thread) {}
    ~JoinOnExit()
    {
        stop_.store(true);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    std::atomic<bool>& stop_;
    std::thread& thread_;
};

bool test_card_service_through_product_ifd()
{
    std::string directory_template =
        px4::userland::test::temporary_directory_template("asicen-card-only-");
    char* directory = ::mkdtemp(directory_template.data());
    CHECK(directory != nullptr);
    CHECK(::chmod(directory, 0700) == 0);

    MailboxFixture transport;
    transport.queue(t1_frame(0xe0U, {}));
    transport.queue(t1_frame(0xe1U, {251U}));
    std::vector<std::uint8_t> reply(61U, 0U);
    reply[59] = 0x90U;
    reply[60] = 0x00U;
    transport.queue(t1_frame(0x00U, reply));

    asicen::W3u3CardMailboxHardware mailbox(transport);
    CardSession raw_session(mailbox, mailbox);
    FixtureOperationGuard guard;
    volatile std::sig_atomic_t stop = 0;
    asicen::W3u3CardServiceBackend backend(mailbox, guard, &stop);
    asicen::W3u3CardProtocolSession session(raw_session, guard, &stop);
    ObservedProtocolSession observed_session(session, guard);
    CardService card(backend, observed_session);
    asicen::CardOnlyTunerBackend tuner_backend;
    PosixTunerNonceSource nonce;
    TestTime time;
    TunerService tuner(tuner_backend, nonce, time);
    const EndpointConfig endpoint{directory, "card-test", kControlEndpointName};
    auto created = PosixControlServer::create(endpoint, card, tuner, {}, true,
                                              asicen::profile::kUsbPresentMask, nullptr,
                                              asicen::profile::kReceiverCount, false);
    CHECK(created);
    auto server = std::move(created.value());
    std::atomic<bool> done{false};
    std::thread pump([&] {
        while (!done.load()) {
            const auto result = server->poll_once(Timeout{5U});
            if (!result) {
                break;
            }
        }
    });
    JoinOnExit join_on_exit(done, pump);

    PosixIfdCardClientFactory factory;
    IfdAdapter ifd(factory);
    const std::string device =
        std::string("asicen-userland:runtime=") + directory + ":instance=card-test:access=user";
    CHECK(ifd.create_channel_by_name(0U, device.c_str()) == IfdResult::success);
    CHECK(ifd.presence(0U) == IfdResult::icc_present);
    std::array<std::uint8_t, 33U> atr{};
    std::size_t atr_length = atr.size();
    const IfdResult power_result = ifd.power(0U, IfdPowerAction::power_up,
                                             MutableByteView{atr.data(), atr.size()}, atr_length);
    if (power_result != IfdResult::success) {
        std::fprintf(stderr, "IFD power-up result=%u atr_length=%zu calls=%u\n",
                     static_cast<unsigned>(power_result), atr_length, transport.calls.load());
    }
    CHECK(power_result == IfdResult::success);
    CHECK(atr_length == 13U);
    CHECK(observed_session.initialize_timeout == 15000U);
    const unsigned resets_after_connect = transport.controller_inits;
    CHECK(ifd.presence(0U) == IfdResult::icc_present);
    CHECK(ifd.presence(0U) == IfdResult::icc_present);
    // Force the presence/protocol interleaving that used to make the timeout
    // assertion scheduler-dependent, without suppressing background polling.
    CHECK(guard.last_operation_timeout == 5000U);
    CHECK(observed_session.initialize_timeout == 15000U);
    CHECK(transport.controller_inits == resets_after_connect);

    std::array<std::uint8_t, 5U> apdu{{0x90U, 0x30U, 0U, 0U, 0U}};
    std::array<std::uint8_t, 80U> response{};
    std::size_t response_length = 0U;
    CHECK(ifd.transmit(0U, kIfdTransmitProtocolT1, ByteView{apdu.data(), apdu.size()},
                       MutableByteView{response.data(), response.size()},
                       response_length) == IfdResult::success);
    CHECK(response_length == 61U && response[59] == 0x90U && response[60] == 0x00U);
    CHECK(observed_session.transmit_timeout == 15000U);

    // Exercise the actual IPC route: card-only mode rejects tuner access
    // before any operation reaches receiver hardware.
    auto raw_result = SocketStream::connect(endpoint, Timeout{2000U});
    CHECK(raw_result);
    auto raw = std::move(raw_result.value());
    std::array<std::uint8_t, kMaxControlPayload> hello_bytes{};
    const auto hello_size =
        encode_payload(HelloRequestPayload{kProtocolMajor, kProtocolMinor, kProtocolMajor,
                                           kProtocolMinor, kCapabilityCard},
                       MutableByteView{hello_bytes.data(), hello_bytes.size()});
    CHECK(hello_size && send_raw_request(raw, MessageType::HELLO, 1U,
                                         ByteView{hello_bytes.data(), hello_size.value()}));
    std::array<std::uint8_t, kFrameHeaderSize + kMaxControlPayload> frame_storage{};
    StreamFramer framer(MutableByteView{frame_storage.data(), frame_storage.size()});
    SingleFrame hello_response;
    CHECK(read_one_raw_response(raw, framer, hello_response));
    CHECK(hello_response.header.kind == MessageKind::response &&
          hello_response.header.type == MessageType::HELLO);
    std::array<std::uint8_t, kMaxControlPayload> acquire_bytes{};
    const auto acquire_size = encode_payload(
        AcquireRequestPayload{1U}, MutableByteView{acquire_bytes.data(), acquire_bytes.size()});
    CHECK(acquire_size);
    CHECK(send_raw_request(raw, MessageType::ACQUIRE, 2U,
                           ByteView{acquire_bytes.data(), acquire_size.value()}));
    SingleFrame unsupported;
    CHECK(read_one_raw_response(raw, framer, unsupported));
    CHECK(unsupported.header.kind == MessageKind::error_response);
    const auto unsupported_error = decode_error_response_payload(
        ByteView{unsupported.payload.data(), unsupported.payload_size});
    CHECK(unsupported_error && unsupported_error.value().error_code == ErrorCode::UNSUPPORTED);
    raw.close();

    CHECK(ifd.close_channel(0U) == IfdResult::success);
    done.store(true);
    pump.join();
    CHECK(server->shutdown());
    const auto shutdown = card.shutdown();
    CHECK(shutdown);
    CHECK(guard.cleanups != 0U && guard.last_cleanup_timeout == 2000U);
    server.reset();
    std::filesystem::remove_all(directory);
    return true;
}

bool test_card_cleanup_failure_is_reported()
{
    MailboxFixture transport;
    transport.fail_cleanup = true;
    asicen::W3u3CardMailboxHardware mailbox(transport);
    CardSession raw_session(mailbox, mailbox);
    FixtureOperationGuard guard;
    asicen::W3u3CardServiceBackend backend(mailbox, guard, nullptr);
    asicen::W3u3CardProtocolSession session(raw_session, guard, nullptr);
    CardService card(backend, session);
    const auto connected = card.connect(77U, ShareMode::shared);
    CHECK(!connected && connected.error() == Error::PROTOCOL_ERROR);
    CHECK(guard.quarantined);
    const auto retry = backend.detect_card();
    CHECK(!retry && retry.error() == Error::NOT_READY);
    return true;
}

bool test_card_operation_entry_preserves_error_class()
{
    MailboxFixture transport;
    asicen::W3u3CardMailboxHardware mailbox(transport);
    CardSession raw_session(mailbox, mailbox);
    FixtureOperationGuard guard;
    asicen::W3u3CardServiceBackend backend(mailbox, guard, nullptr);
    asicen::W3u3CardProtocolSession session(raw_session, guard, nullptr);
    std::array<std::uint8_t, 5U> command{};
    std::array<std::uint8_t, 16U> response{};
    for (const auto error : {Error::TIMEOUT, Error::BUSY, Error::DISCONNECTED,
                             Error::INVALID_ARGUMENT, Error::USB_IO}) {
        guard.entry_error = error;
        CHECK(backend.detect_card().error() == error);
        CHECK(backend.set_power(false).error() == error);
        CHECK(session.initialize().error() == error);
        CHECK(session.transmit({command.data(), command.size()}, {response.data(), response.size()})
                  .error() == error);
        CHECK(transport.calls == 0U && guard.operations == 0U && guard.cleanups == 0U);
    }
    return true;
}

}  // namespace

int main()
{
    if (!test_protocol_timeouts_survive_presence_poll() ||
        !test_card_operation_entry_preserves_error_class() ||
        !test_card_service_through_product_ifd() || !test_card_cleanup_failure_is_reported()) {
        return 1;
    }
    std::puts("PASS asicen card-only service");
    return 0;
}
