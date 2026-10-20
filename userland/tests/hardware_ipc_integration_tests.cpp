// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/hardware_stream_session.h"
#include "asicen/px4_mock_backend.h"
#include "asicen/control_context_builder.h"
#include "test_wire_client.h"
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
#include <unistd.h>

namespace {
using namespace px4::userland;
using namespace px4::userland::ipc;
using namespace px4::userland::ipc::posix;
namespace wire = dtv::userland::wire;

class Time final : public TunerServiceTime {
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

class Source final : public asicen::StreamCaptureSource {
public:
    Result<void> prepare(std::uint8_t receiver, System system,
                         const std::atomic<bool>& cancelled) noexcept override
    {
        return receiver == 1U && system == System::ISDB_T && !cancelled.load()
                   ? Result<void>::success()
                   : Result<void>::failure(Error::UNSUPPORTED);
    }
    asicen::CaptureRunResult run(const std::atomic<bool>& cancelled,
                                 bool (*emit)(void*, const std::uint8_t*, std::size_t),
                                 void* context) noexcept override
    {
        std::array<std::uint8_t, 188> packet{};
        packet[0] = 0x47U;
        packet[1] = 0U;
        packet[2] = 0x20U;
        packet[3] = 0x10U;
        if (!cancelled.load()) {
            (void)emit(context, packet.data(), packet.size());
        }
        std::unique_lock<std::mutex> lock(mutex_);
        run_started_.store(true, std::memory_order_release);
        condition_.notify_all();
        condition_.wait(lock, [&] { return cancelled.load() || interrupted_; });
        if (hold_after_interrupt_) {
            condition_.wait(lock, [&] { return release_run_; });
        }
        return asicen::CaptureRunResult::cancelled;
    }
    void interrupt() noexcept override
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            interrupted_ = true;
            interrupt_seen_.store(true, std::memory_order_release);
        }
        condition_.notify_all();
    }
    Result<void> stop() noexcept override { return Result<void>::success(); }
    void hold_after_interrupt() noexcept { hold_after_interrupt_ = true; }
    void release_run() noexcept
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            release_run_ = true;
        }
        condition_.notify_all();
    }
    bool run_started() const noexcept { return run_started_.load(std::memory_order_acquire); }
    bool interrupt_seen() const noexcept { return interrupt_seen_.load(std::memory_order_acquire); }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool interrupted_ = false;
    bool hold_after_interrupt_ = false;
    bool release_run_ = false;
    std::atomic<bool> run_started_{false};
    std::atomic<bool> interrupt_seen_{false};
};

class FrameCounter final : public wire::FrameHandler {
public:
    Result<void> on_frame(const wire::Frame& frame) noexcept override
    {
        if (frame.header.opcode == wire::kOpcodeAttachStream &&
            frame.header.kind == wire::Kind::response) {
            attached = true;
        }
        if (frame.header.opcode == wire::kOpcodeTsData && frame.header.kind == wire::Kind::event) {
            const auto data = wire::decode_ts_data(frame.payload);
            if (!data || data.value().packets.size != 188U || data.value().packets.data[0] != 0x47U) {
                return Result<void>::failure(Error::PROTOCOL_ERROR);
            }
            ts_packets += data.value().packets.size / 188U;
        }
        if (frame.header.opcode == wire::kOpcodeStreamEndEvent &&
            frame.header.kind == wire::Kind::event) {
            const auto end = wire::decode_stream_end_event(frame.payload);
            if (!end) {
                return Result<void>::failure(Error::PROTOCOL_ERROR);
            }
            stream_end = end.value();
            stream_end_seen = true;
        }
        return Result<void>::success();
    }
    bool attached = false;
    std::size_t ts_packets = 0U;
    bool stream_end_seen = false;
    wire::StreamEndEvent stream_end{};
};

bool run(bool hold_detach = false)
{
    std::string pattern = test::temporary_directory_template("asicen-hw-ipc-");
    char* made = ::mkdtemp(pattern.data());
    if (made == nullptr) {
        return false;
    }
    const std::string root(made);
    asicen::MockTunerBackend frontend;
    Source source;
    if (hold_detach) {
        source.hold_after_interrupt();
    }

    asicen::HardwareStreamService stream(frontend, source);
    asicen::UnsupportedCardBackend card_backend;
    asicen::UnsupportedCardSession card_session;
    CardService card(card_backend, card_session);
    px4::userland::ipc::posix::PosixTunerNonceSource nonce;
    Time time;
    TunerService tuner(stream, nonce, time, nullptr, nullptr, &stream);
    if (!tuner.configure_strict_capabilities()) return false;
    const EndpointConfig endpoint{root.c_str(), "receiver1", kControlEndpointName};
    ControlContext context{};
    const auto epoch = nonce.generate();
    const auto* profile = asicen::find_profile(asicen::ModelId::W3u3);
    if (!epoch || profile == nullptr || !asicen::build_asicen_mock_control_context(
            *profile, tuner, endpoint.instance, epoch.value(), context)) return false;
    auto server_result = PosixControlServer::create(endpoint, card, tuner, context, &stream);
    if (!server_result) {
        std::filesystem::remove_all(root);
        return false;
    }
    auto server = std::move(server_result.value());
    std::atomic<bool> polling{true};
    std::thread poller([&] {
        while (polling.load()) {
            (void)server->poll_once(Timeout{10U});
        }
    });
    auto client_result = asicen::test::WireClient::connect(endpoint);
    if (!client_result) {
        polling.store(false);
        poller.join();
        (void)server->shutdown();
        std::filesystem::remove_all(root);
        return false;
    }
    auto client = std::move(client_result);
    const char* stage = "acquire";
    wire::AcquireRequest acquire{};
    acquire.epoch = client->epoch;
    acquire.receiver_id = 1U;
    const auto acquired = client->request(wire::kOpcodeAcquire, acquire, wire::encode_acquire_request);
    if (!acquired) {
        goto fail;
    }
    {
        const auto lease = wire::decode_acquire_response(
            {acquired.value().payload.data(), acquired.value().payload.size()});
        if (!lease) {
            goto fail;
        }
        stage = "tune";
        wire::TuneRequest tune_request{};
        tune_request.epoch = client->epoch;
        tune_request.lease_id = lease.value().lease_id;
        tune_request.system = wire::System::isdb_t;
        tune_request.frequency_hz = 557142857U;
        tune_request.timeout_ms = 500U;
        const auto tune = client->request(wire::kOpcodeTune, tune_request, wire::encode_tune_request);
        if (!tune) {
            goto fail;
        }
        stage = "start-stream";
        const auto tuned = wire::decode_tune_result(
            ByteView{tune.value().payload.data(), tune.value().payload.size()});
        if (!tuned) goto fail;
        wire::StartRequest start_request{};
        start_request.epoch = client->epoch;
        start_request.lease_id = lease.value().lease_id;
        start_request.tune_revision = tuned.value().tune_revision;
        const auto start = client->request(wire::kOpcodeStartStream, start_request, wire::encode_start_request);
        if (!start) {
            goto fail;
        }
        stage = "connect-stream-socket";
        const EndpointConfig stream_endpoint{root.c_str(), "receiver1", kStreamEndpointName};
        auto stream_result = SocketStream::connect(stream_endpoint, Timeout{2000U});
        if (!stream_result) {
            goto fail;
        }
        auto socket = std::move(stream_result.value());
        std::array<std::uint8_t, kMaxControlPayload> payload{};
        const auto started = wire::decode_start_response(
            ByteView{start.value().payload.data(), start.value().payload.size()});
        if (!started) goto fail;
        wire::AttachRequest attach{};
        attach.epoch = client->epoch;
        attach.lease_id = lease.value().lease_id;
        attach.stream_id = started.value().stream_id;
        attach.attach_token = started.value().attach_token;
        const auto payload_size =
            wire::encode_attach_request(attach,
                           {payload.data(), payload.size()});
        if (!payload_size) {
            goto fail;
        }
        stage = "attach-write";
        wire::Header header{};
        header.kind = wire::Kind::request;
        header.opcode = wire::kOpcodeAttachStream;
        header.request_id = 1U;
        header.payload_length = static_cast<std::uint32_t>(payload_size.value());
        std::array<std::uint8_t, wire::kHeaderSize + wire::kMaxControlPayload> frame{};
        const auto frame_size = wire::encode_frame(header, {payload.data(), payload_size.value()},
                                             {frame.data(), frame.size()});
        if (!frame_size ||
            !socket.write_all({frame.data(), frame_size.value()}, Timeout{1000U})) {
            goto fail;
        }
        std::vector<std::uint8_t> storage(wire::kMaxTsDataPayload);
        wire::FrameReceiver framer({storage.data(), storage.size()});
        FrameCounter counter;
        const auto read_frames = [&](Timeout timeout) {
            std::array<std::uint8_t, 4096U> bytes{};
            const auto read = socket.read_some({bytes.data(), bytes.size()}, timeout);
            if (!read) return Result<void>::failure(read.error());
            const auto fed = framer.feed({bytes.data(), read.value()}, counter);
            return fed ? Result<void>::success() : Result<void>::failure(fed.error());
        };
        const auto read_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        bool read_ok = true;
        while (counter.ts_packets == 0U && std::chrono::steady_clock::now() < read_deadline) {
            const auto received = read_frames(Timeout{200U});
            if (!received) {
                read_ok = false;
                break;
            }
        }
        stage = "attach-or-ts-data";
        if (!read_ok || !counter.attached || counter.ts_packets == 0U) {
            goto fail;
        }
        stage = "stop-stream";
        wire::StopRequest stop_request{};
        stop_request.epoch = client->epoch;
        stop_request.lease_id = lease.value().lease_id;
        stop_request.stream_id = started.value().stream_id;
        if (!hold_detach) {
            const auto stop = client->request(wire::kOpcodeStopStream, stop_request, wire::encode_stop_request);
            if (!stop) {
                goto fail;
            }
            socket.close();
            client->close();
            polling.store(false);
            poller.join();
            const auto stopped = server->shutdown();
            server.reset();
            std::filesystem::remove_all(root);
            return static_cast<bool>(stopped);
        }

        Result<asicen::test::Reply> stop = Result<asicen::test::Reply>::failure(Error::INTERNAL);
        std::thread stop_thread([&] {
            stop = client->request(wire::kOpcodeStopStream, stop_request, wire::encode_stop_request);
        });
        const auto interrupt_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!source.interrupt_seen() && std::chrono::steady_clock::now() < interrupt_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!source.interrupt_seen()) {
            source.release_run();
            stop_thread.join();
            goto fail;
        }
        // Keep the producer blocked after detach has invalidated the live
        // attachment, so the server polls read() during the join window.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        source.release_run();
        stop_thread.join();
        if (!stop) {
            goto fail;
        }
        const auto stop_counters =
            wire::decode_stop_response({stop.value().payload.data(), stop.value().payload.size()});
        if (!stop_counters) {
            goto fail;
        }
        stage = "stream-end-after-stop";
        const auto end_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!counter.stream_end_seen && std::chrono::steady_clock::now() < end_deadline) {
            const auto received = read_frames(Timeout{100U});
            if (!received && received.error() != Error::TIMEOUT) {
                goto fail;
            }
        }
        if (!counter.stream_end_seen || counter.stream_end.final.error.present ||
            counter.stream_end.final.statistics.entries.count !=
                stop_counters.value().final.statistics.entries.count) {
            goto fail;
        }
        for (std::uint16_t i = 0U; i < counter.stream_end.final.statistics.entries.count; ++i) {
            const auto& event = counter.stream_end.final.statistics.entries.items[i];
            const auto& response = stop_counters.value().final.statistics.entries.items[i];
            if (event.id != response.id ||
                event.counter.measurement.value != response.counter.measurement.value ||
                event.counter.measurement.state != response.counter.measurement.state ||
                event.counter.overflow != response.counter.overflow) goto fail;
        }
        socket.close();
        client->close();
        polling.store(false);
        poller.join();
        const auto stopped = server->shutdown();
        server.reset();
        std::filesystem::remove_all(root);
        return static_cast<bool>(stopped);
    }
fail:
    std::fprintf(stderr, "hardware IPC integration failed at %s\n", stage);
    source.release_run();
    client->close();
    stream.request_stop();
    polling.store(false);
    poller.join();
    (void)server->shutdown();
    server.reset();
    std::filesystem::remove_all(root);
    return false;
}
}  // namespace

int main()
{
    if (!run()) {
        return 1;
    }
    if (!run(true)) {
        return 1;
    }
    return 0;
}
