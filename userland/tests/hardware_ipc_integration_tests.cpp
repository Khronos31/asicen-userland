// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/hardware_stream_session.h"
#include "asicen/px4_mock_backend.h"
#include "px4/card_service.h"
#include "px4/control_client.h"
#include "px4/control_server.h"
#include "px4/posix_tuner_nonce.h"
#include "test_temp_directory.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>

namespace {
using namespace px4::userland;
using namespace px4::userland::ipc;
using namespace px4::userland::ipc::posix;

class Time final : public TunerServiceTime {
public:
    std::uint64_t monotonic_ms() noexcept override {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    void sleep_ms(std::uint32_t ms) noexcept override {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
};

class Source final : public asicen::StreamCaptureSource {
public:
    Result<void> prepare(std::uint8_t receiver, System system,
                         const std::atomic<bool>& cancelled) noexcept override {
        return receiver == 1U && system == System::ISDB_T && !cancelled.load()
            ? Result<void>::success() : Result<void>::failure(Error::UNSUPPORTED);
    }
    asicen::CaptureRunResult run(const std::atomic<bool>& cancelled,
                                 bool (*emit)(void*, const std::uint8_t*, std::size_t),
                                 void* context) noexcept override {
        std::array<std::uint8_t, 188> packet{};
        packet[0] = 0x47U;
        packet[1] = 0U;
        packet[2] = 0x20U;
        packet[3] = 0x10U;
        if (!cancelled.load()) (void)emit(context, packet.data(), packet.size());
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [&] { return cancelled.load() || interrupted_; });
        return asicen::CaptureRunResult::cancelled;
    }
    void interrupt() noexcept override {
        { std::lock_guard<std::mutex> lock(mutex_); interrupted_ = true; }
        condition_.notify_all();
    }
    Result<void> stop() noexcept override { return Result<void>::success(); }
private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool interrupted_ = false;
};

class Fatal final : public asicen::StreamProcessFatal {
public:
    void terminate_nonzero(int) noexcept override { std::abort(); }
};

class FrameCounter final : public FrameConsumer {
public:
    Result<void> on_frame(const FrameView& frame) noexcept override {
        if (frame.header.type == MessageType::ATTACH_STREAM &&
            frame.header.kind == MessageKind::response) attached = true;
        if (frame.header.type == MessageType::TS_DATA &&
            frame.header.kind == MessageKind::event) {
            const auto data = decode_ts_data_event_payload(frame.payload);
            if (!data || data.value().bytes.size != 188U || data.value().bytes.data[0] != 0x47U)
                return Result<void>::failure(Error::PROTOCOL_ERROR);
            ts_packets += data.value().bytes.size / 188U;
        }
        return Result<void>::success();
    }
    bool attached = false;
    std::size_t ts_packets = 0U;
};

template <typename T>
Result<ControlResponse> typed_request(PosixControlClient& client, MessageType type,
                                      const T& payload) {
    std::array<std::uint8_t, kMaxControlPayload> encoded{};
    const auto size = encode_payload(payload, {encoded.data(), encoded.size()});
    if (!size) return Result<ControlResponse>::failure(size.error());
    return client.request(type, {encoded.data(), size.value()}, Timeout{2000U});
}

bool run() {
    std::string pattern = test::temporary_directory_template("asicen-hw-ipc-");
    char* made = ::mkdtemp(pattern.data());
    if (made == nullptr) return false;
    const std::string root(made);
    asicen::MockTunerBackend frontend;
    Source source;
    Fatal fatal;
    asicen::HardwareStreamService stream(frontend, source, fatal);
    asicen::UnsupportedCardBackend card_backend;
    asicen::UnsupportedCardSession card_session;
    CardService card(card_backend, card_session);
    px4::userland::ipc::posix::PosixTunerNonceSource nonce;
    Time time;
    TunerService tuner(stream, nonce, time, nullptr, nullptr, &stream);
    const EndpointConfig endpoint{root.c_str(), "receiver1", kControlEndpointName};
    auto server_result = PosixControlServer::create(endpoint, card, tuner, {}, true,
                                                    0x03U, &stream, 4U, false);
    if (!server_result) { std::filesystem::remove_all(root); return false; }
    auto server = std::move(server_result.value());
    std::atomic<bool> polling{true};
    std::thread poller([&] {
        while (polling.load()) (void)server->poll_once(Timeout{10U});
    });
    auto client_result = PosixControlClient::connect(endpoint, 0U, Timeout{2000U});
    if (!client_result) {
        polling.store(false); poller.join(); (void)server->shutdown();
        std::filesystem::remove_all(root); return false;
    }
    auto client = std::move(client_result.value());
    const char* stage = "acquire";
    const auto acquired = typed_request(*client, MessageType::ACQUIRE,
                                        AcquireRequestPayload{1U});
    if (!acquired) goto fail;
    {
        const auto lease = decode_acquire_response_payload(
            {acquired.value().payload.data(), acquired.value().payload.size()});
        if (!lease) goto fail;
        stage = "tune";
        const auto tune = typed_request(*client, MessageType::TUNE,
            TuneRequestPayload{lease.value().lease_id, System::ISDB_T,
                               557142U, 0xffffU, 0xffffU, 6000000U, 0U, 500U});
        if (!tune) goto fail;
        stage = "start-stream";
        const auto start = typed_request(*client, MessageType::START_STREAM,
                                         LeaseRequestPayload{lease.value().lease_id});
        if (!start) goto fail;
        stage = "connect-stream-socket";
        const EndpointConfig stream_endpoint{root.c_str(), "receiver1", kStreamEndpointName};
        auto stream_result = SocketStream::connect(stream_endpoint, Timeout{2000U});
        if (!stream_result) goto fail;
        auto socket = std::move(stream_result.value());
        std::array<std::uint8_t, kMaxControlPayload> payload{};
        const auto payload_size = encode_payload(
            AttachStreamRequestPayload{lease.value().lease_id, lease.value().nonce},
            {payload.data(), payload.size()});
        if (!payload_size) goto fail;
        stage = "attach-write";
        const FrameHeader header{kProtocolMajor, kProtocolMinor, MessageType::ATTACH_STREAM,
                                 MessageKind::request, 1U,
                                 static_cast<std::uint32_t>(payload_size.value())};
        std::array<std::uint8_t, kFrameHeaderSize + kMaxControlPayload> frame{};
        const auto frame_size = encode_frame(header,
            {payload.data(), payload_size.value()}, {frame.data(), frame.size()});
        if (!frame_size || !socket.write_frame({frame.data(), frame_size.value()}, Timeout{1000U}))
            goto fail;
        std::array<std::uint8_t, kFrameHeaderSize + kMaxTsDataPayload> storage{};
        StreamFramer framer({storage.data(), storage.size()});
        FrameCounter counter;
        const auto read_deadline = std::chrono::steady_clock::now() +
                                   std::chrono::seconds(2);
        bool read_ok = true;
        while (counter.ts_packets == 0U &&
               std::chrono::steady_clock::now() < read_deadline) {
            const auto received = socket.read_frames(
                {storage.data(), storage.size()}, framer, counter, Timeout{200U});
            if (!received) { read_ok = false; break; }
        }
        stage = "attach-or-ts-data";
        if (!read_ok || !counter.attached || counter.ts_packets == 0U) goto fail;
        stage = "stop-stream";
        const auto stop = typed_request(*client, MessageType::STOP_STREAM,
                                        LeaseRequestPayload{lease.value().lease_id});
        if (!stop) goto fail;
        socket.close();
        client->close();
        polling.store(false); poller.join();
        const auto stopped = server->shutdown();
        server.reset();
        std::filesystem::remove_all(root);
        return static_cast<bool>(stopped);
    }
fail:
    std::fprintf(stderr, "hardware IPC integration failed at %s\n", stage);
    client->close();
    stream.request_stop();
    polling.store(false); poller.join();
    (void)server->shutdown();
    server.reset();
    std::filesystem::remove_all(root);
    return false;
}
}  // namespace

int main() {
    if (!run()) return 1;
    return 0;
}
