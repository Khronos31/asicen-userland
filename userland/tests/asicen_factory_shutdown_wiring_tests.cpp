// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/control_context_builder.h"
#include "asicen/device_profile.h"
#include "dtv/userland/control_connection.h"
#include "dtv/userland/wire.h"
#include "px4/card_service.h"
#include "px4/control_server.h"
#include "px4/posix_tuner_nonce.h"
#include "px4/tuner_service.h"
#include "control_server_test_access.h"
#include "control_workers.h"
#include "test_temp_directory.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {
using namespace px4::userland;
using namespace px4::userland::ipc;
using namespace px4::userland::ipc::posix;
namespace wire = dtv::userland::wire;

struct WireReply final {
    wire::Header header{};
    std::vector<std::uint8_t> payload;
};

class ReplyCollector final : public wire::FrameHandler {
public:
    Result<void> on_frame(const wire::Frame& frame) noexcept override
    {
        WireReply reply;
        reply.header = frame.header;
        if (frame.payload.size != 0U)
            reply.payload.assign(frame.payload.data, frame.payload.data + frame.payload.size);
        replies.push_back(std::move(reply));
        return Result<void>::success();
    }
    std::vector<WireReply> replies;
};

class ShutdownWireClient final {
public:
    explicit ShutdownWireClient(SocketStream stream)
        : stream_(std::move(stream)), storage_(new std::array<std::uint8_t, wire::kMaxControlPayload>()),
          receiver_(MutableByteView{storage_->data(), storage_->size()})
    {
    }

    static std::unique_ptr<ShutdownWireClient> connect(const EndpointConfig& endpoint)
    {
        auto stream = SocketStream::connect(endpoint, Timeout{1000U});
        if (!stream) return nullptr;
        return std::unique_ptr<ShutdownWireClient>(
            new ShutdownWireClient(std::move(stream.value())));
    }

    bool hello()
    {
        wire::HelloRequest request;
        request.min = wire::Version{1U, 0U};
        request.max = wire::Version{1U, 0U};
        WireReply reply;
        if (!transact(wire::kOpcodeHello, encode(request, wire::encode_hello_request), reply)) {
            std::fprintf(stderr, "shutdown wire HELLO transaction failed\n");
            return false;
        }
        if (reply.header.kind != wire::Kind::response) {
            std::fprintf(stderr, "shutdown wire HELLO unexpected response kind=%u\n",
                         static_cast<unsigned>(reply.header.kind));
            return false;
        }
        const auto decoded = wire::decode_hello_response(view(reply));
        if (!decoded) {
            std::fprintf(stderr, "shutdown wire HELLO decode failed: %s\n",
                         error_string(decoded.error()));
            return false;
        }
        epoch_ = decoded.value().epoch;
        return true;
    }

    bool acquire(std::uint16_t receiver_id, std::uint64_t& lease_id)
    {
        wire::AcquireRequest request;
        request.epoch = epoch_;
        request.receiver_id = receiver_id;
        WireReply reply;
        if (!transact(wire::kOpcodeAcquire, encode(request, wire::encode_acquire_request), reply) ||
            reply.header.kind != wire::Kind::response) return false;
        const auto decoded = wire::decode_acquire_response(view(reply));
        if (!decoded) return false;
        lease_id = decoded.value().lease_id;
        return true;
    }

    bool send_tune(std::uint64_t lease_id)
    {
        wire::TuneRequest request;
        request.epoch = epoch_;
        request.lease_id = lease_id;
        request.system = wire::System::isdb_t;
        request.frequency_hz = 473142857U;
        request.timeout_ms = 30000U;
        const auto payload = encode(request, wire::encode_tune_request);
        const std::uint32_t request_id = ++request_id_;
        wire::Header header;
        header.kind = wire::Kind::request;
        header.opcode = wire::kOpcodeTune;
        header.request_id = request_id;
        header.payload_length = static_cast<std::uint32_t>(payload.size());
        std::vector<std::uint8_t> frame(wire::kHeaderSize + payload.size());
        const auto size = wire::encode_frame(
            header, ByteView{payload.data(), payload.size()},
            MutableByteView{frame.data(), frame.size()});
        if (!size) return false;
        return send_frame(ByteView{frame.data(), size.value()});
    }

private:
    template <typename Record>
    static std::vector<std::uint8_t> encode(
        const Record& value,
        Result<std::size_t> (*encoder)(const Record&, MutableByteView) noexcept)
    {
        std::vector<std::uint8_t> bytes(wire::kMaxControlPayload);
        const auto size = encoder(value, MutableByteView{bytes.data(), bytes.size()});
        if (!size) return {};
        bytes.resize(size.value());
        return bytes;
    }

    static ByteView view(const WireReply& reply) noexcept
    {
        return ByteView{reply.payload.data(), reply.payload.size()};
    }

    bool send_frame(ByteView frame)
    {
        std::size_t sent = 0U;
        while (sent < frame.size) {
            const auto count = ::send(stream_.native_handle(), frame.data + sent,
                                      frame.size - sent, MSG_NOSIGNAL);
            if (count <= 0) return false;
            sent += static_cast<std::size_t>(count);
        }
        return true;
    }

    bool transact(std::uint16_t opcode, const std::vector<std::uint8_t>& payload,
                  WireReply& reply)
    {
        const std::uint32_t request_id = ++request_id_;
        wire::Header header;
        header.kind = wire::Kind::request;
        header.opcode = opcode;
        header.request_id = request_id;
        header.payload_length = static_cast<std::uint32_t>(payload.size());
        std::vector<std::uint8_t> frame(wire::kHeaderSize + payload.size());
        const auto size = wire::encode_frame(
            header, ByteView{payload.data(), payload.size()},
            MutableByteView{frame.data(), frame.size()});
        if (!size) {
            std::fprintf(stderr, "shutdown wire encode failed opcode=%u\n", opcode);
            return false;
        }
        if (!send_frame(ByteView{frame.data(), size.value()})) {
            std::fprintf(stderr, "shutdown wire write failed opcode=%u\n", opcode);
            return false;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (collector_.replies.empty() && std::chrono::steady_clock::now() < deadline) {
            std::array<std::uint8_t, 4096U> buffer{};
            const auto read = stream_.read_some(
                MutableByteView{buffer.data(), buffer.size()}, Timeout{20U});
            if (!read) {
                if (read.error() != Error::TIMEOUT) {
                    std::fprintf(stderr, "shutdown wire read failed opcode=%u error=%s\n",
                                 opcode, error_string(read.error()));
                    return false;
                }
                continue;
            }
            if (!receiver_.feed(ByteView{buffer.data(), read.value()}, collector_)) {
                std::fprintf(stderr, "shutdown wire frame decode failed opcode=%u\n", opcode);
                return false;
            }
        }
        if (collector_.replies.empty()) {
            std::fprintf(stderr, "shutdown wire response timeout opcode=%u\n", opcode);
            return false;
        }
        reply = std::move(collector_.replies.front());
        collector_.replies.erase(collector_.replies.begin());
        return reply.header.opcode == opcode && reply.header.request_id == request_id;
    }

    SocketStream stream_;
    std::unique_ptr<std::array<std::uint8_t, wire::kMaxControlPayload>> storage_;
    wire::FrameReceiver receiver_;
    ReplyCollector collector_;
    wire::Uuid epoch_{};
    std::uint32_t request_id_ = 0U;
};

struct StopTrace final {
    void add(const char* event)
    {
        std::lock_guard<std::mutex> lock(mutex);
        events.emplace_back(event);
    }
    std::mutex mutex;
    std::vector<std::string> events;
};

class Tuner final : public TunerServiceBackend {
public:
    explicit Tuner(StopTrace& trace) : trace_(trace) {}
    std::uint8_t receiver_count() const noexcept override { return 1U; }
    bool receiver_supports(std::uint8_t receiver, System system) const noexcept override
    { return receiver == 0U && (system == System::ISDB_T || system == System::ISDB_S); }
    Result<void> open_receiver(std::uint8_t) noexcept override { return Result<void>::success(); }
    Result<void> tune_terrestrial(std::uint8_t, std::uint32_t, std::uint32_t) noexcept override
    {
        std::unique_lock<std::mutex> lock(mutex_);
        entered.store(true, std::memory_order_release);
        condition_.notify_all();
        condition_.wait(lock, [&] { return stopped.load(std::memory_order_acquire); });
        return Result<void>::failure(Error::NOT_READY);
    }
    Result<void> tune_satellite(std::uint8_t, std::uint32_t, std::uint32_t) noexcept override
    { return Result<void>::success(); }
    Result<bool> is_locked(std::uint8_t, System) noexcept override
    { return Result<bool>::success(true); }
    Result<void> select_satellite_slot(std::uint8_t, std::uint8_t, std::uint32_t) noexcept override
    { return Result<void>::success(); }
    Result<void> select_satellite_tsid(std::uint8_t, std::uint16_t, std::uint32_t) noexcept override
    { return Result<void>::success(); }
    Result<void> close_receiver(std::uint8_t) noexcept override { return Result<void>::success(); }
    void request_stop() noexcept override
    {
        trace_.add("tuner");
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped.store(true, std::memory_order_release);
        }
        condition_.notify_all();
    }
    std::atomic<bool> entered{false};
    std::atomic<bool> stopped{false};
private:
    StopTrace& trace_;
    std::mutex mutex_;
    std::condition_variable condition_;
};

class CardBackend final : public CardServiceBackend {
public:
    explicit CardBackend(StopTrace& trace) : trace_(trace) {}
    Result<void> set_power(bool) noexcept override { return Result<void>::success(); }
    Result<void> initialize_uart() noexcept override { return Result<void>::success(); }
    Result<bool> detect_card() noexcept override
    {
        std::unique_lock<std::mutex> lock(mutex_);
        entered.store(true, std::memory_order_release);
        condition_.notify_all();
        condition_.wait(lock, [&] { return stopped.load(std::memory_order_acquire); });
        return Result<bool>::failure(Error::NOT_READY);
    }
    void request_stop() noexcept override
    {
        trace_.add("card-backend");
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped.store(true, std::memory_order_release);
        }
        condition_.notify_all();
    }
    std::atomic<bool> entered{false};
    std::atomic<bool> stopped{false};
private:
    StopTrace& trace_;
    std::mutex mutex_;
    std::condition_variable condition_;
};

class CardSession final : public CardProtocolSession {
public:
    explicit CardSession(StopTrace& trace) : trace_(trace) {}
    Result<void> initialize() noexcept override
    {
        std::unique_lock<std::mutex> lock(mutex_);
        entered.store(true, std::memory_order_release);
        condition_.notify_all();
        condition_.wait(lock, [&] { return stopped.load(std::memory_order_acquire); });
        return Result<void>::failure(Error::NOT_READY);
    }
    Result<std::size_t> transmit(ByteView, MutableByteView) noexcept override
    { return Result<std::size_t>::failure(Error::UNSUPPORTED); }
    bool initialized() const noexcept override { return false; }
    const CardAtr& atr() const noexcept override { return atr_; }
    void invalidate() noexcept override {}
    void request_stop() noexcept override
    {
        trace_.add("card-session");
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped.store(true, std::memory_order_release);
        }
        condition_.notify_all();
    }
    std::atomic<bool> entered{false};
    std::atomic<bool> stopped{false};
private:
    StopTrace& trace_;
    CardAtr atr_{};
    std::mutex mutex_;
    std::condition_variable condition_;
};

class Stream final : public TunerStreamControl {
public:
    explicit Stream(StopTrace& trace) : trace_(trace) {}
    Result<void> attach(const TunerAttachment&) noexcept override { return Result<void>::success(); }
    Result<void> detach(const TunerAttachment&) noexcept override { return Result<void>::success(); }
    Result<TunerStreamReadResult> read(const TunerAttachment&, MutableByteView,
                                       Timeout) noexcept override
    {
        std::unique_lock<std::mutex> lock(mutex_);
        entered.store(true, std::memory_order_release);
        condition_.notify_all();
        condition_.wait(lock, [&] { return stopped.load(std::memory_order_acquire); });
        return Result<TunerStreamReadResult>::failure(Error::NOT_READY);
    }
    void request_stop() noexcept override
    {
        trace_.add("stream");
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped.store(true, std::memory_order_release);
        }
        condition_.notify_all();
    }
    std::atomic<bool> entered{false};
    std::atomic<bool> stopped{false};
private:
    StopTrace& trace_;
    std::mutex mutex_;
    std::condition_variable condition_;
};

class Time final : public TunerServiceTime {
public:
    std::uint64_t monotonic_ms() noexcept override { return 1U; }
    void sleep_ms(std::uint32_t) noexcept override {}
};

class QueuedBackend final : public TunerServiceBackend {
public:
    std::uint8_t receiver_count() const noexcept override { return 2U; }
    bool receiver_supports(std::uint8_t receiver, System system) const noexcept override
    { return receiver < 2U && system == System::ISDB_T; }
    TunerStrictProfile strict_profile(std::uint8_t receiver,
                                      System system) const noexcept override
    {
        if (!receiver_supports(receiver, system)) return {};
        TunerStrictProfile profile{};
        profile.native_hz = true;
        profile.min_hz = 40000000U;
        profile.max_hz = 900000000U;
        return profile;
    }
    Result<void> open_receiver(std::uint8_t receiver) noexcept override
    { return receiver < 2U ? Result<void>::success() : Result<void>::failure(Error::NOT_FOUND); }
    Result<void> tune_terrestrial(std::uint8_t, std::uint32_t, std::uint32_t) noexcept override
    { return Result<void>::failure(Error::UNSUPPORTED); }
    Result<void> tune_satellite(std::uint8_t, std::uint32_t, std::uint32_t) noexcept override
    { return Result<void>::failure(Error::UNSUPPORTED); }
    Result<TunerHzProgramming> tune_terrestrial_hz(std::uint8_t receiver, std::uint64_t,
                                                   std::uint32_t) noexcept override
    {
        if (receiver >= 2U) return Result<TunerHzProgramming>::failure(Error::NOT_FOUND);
        if (receiver == 0U) {
            std::unique_lock<std::mutex> lock(mutex);
            active_tune_entered.store(true, std::memory_order_release);
            condition.notify_all();
            condition.wait(lock, [&] { return stopped.load(std::memory_order_acquire); });
        } else {
            receiver_one_tune_entered.store(true, std::memory_order_release);
            receiver_one_tune_entered_after_stop.store(
                stopped.load(std::memory_order_acquire), std::memory_order_release);
        }
        return Result<TunerHzProgramming>::failure(Error::NOT_READY);
    }
    Result<TunerHzProgramming> tune_satellite_hz(std::uint8_t, std::uint64_t,
                                                 std::uint32_t) noexcept override
    { return Result<TunerHzProgramming>::failure(Error::UNSUPPORTED); }
    Result<bool> is_locked(std::uint8_t receiver, System system) noexcept override
    { return receiver_supports(receiver, system) ? Result<bool>::success(true)
                                                  : Result<bool>::failure(Error::NOT_FOUND); }
    Result<void> select_satellite_slot(std::uint8_t, std::uint8_t, std::uint32_t) noexcept override
    { return Result<void>::failure(Error::UNSUPPORTED); }
    Result<void> select_satellite_tsid(std::uint8_t, std::uint16_t, std::uint32_t) noexcept override
    { return Result<void>::failure(Error::UNSUPPORTED); }
    Result<void> close_receiver(std::uint8_t receiver) noexcept override
    { return receiver < 2U ? Result<void>::success() : Result<void>::failure(Error::NOT_FOUND); }
    Result<void> begin_tune_power(std::uint8_t receiver, System system,
                                  std::uint8_t) noexcept override
    { return receiver_supports(receiver, system) ? Result<void>::success()
                                                  : Result<void>::failure(Error::NOT_FOUND); }
    Result<void> commit_tune_power(std::uint8_t) noexcept override
    { return Result<void>::success(); }
    Result<void> rollback_tune_power(std::uint8_t) noexcept override
    { return Result<void>::success(); }
    void request_stop() noexcept override
    {
        stopped.store(true, std::memory_order_release);
        condition.notify_all();
    }

    std::atomic<bool> active_tune_entered{false};
    std::atomic<bool> receiver_one_tune_entered{false};
    std::atomic<bool> receiver_one_tune_entered_after_stop{false};
    std::atomic<bool> stopped{false};
private:
    std::mutex mutex;
    std::condition_variable condition;
};

struct WorkerEvent final {
    std::uint8_t lane = 0U;
    std::uint8_t operation = 0U;
    std::uint64_t client_id = 0U;
    std::uint32_t request_id = 0U;
    std::uint8_t receiver = 0U;
    std::uint32_t error = 0U;
};

struct ServerWorkerTrace final {
    std::mutex mutex;
    std::vector<WorkerEvent> submitted;
    std::vector<WorkerEvent> completed;
    std::size_t live_threads_after_join = std::numeric_limits<std::size_t>::max();
};

void record_task_submitted(void* context, std::uint8_t lane, std::uint8_t operation,
                           std::uint64_t client_id, std::uint32_t request_id,
                           std::uint8_t receiver) noexcept
{
    auto& trace = *static_cast<ServerWorkerTrace*>(context);
    std::lock_guard<std::mutex> lock(trace.mutex);
    trace.submitted.push_back(WorkerEvent{lane, operation, client_id, request_id, receiver, 0U});
}

void record_task_completed(void* context, std::uint8_t operation,
                           std::uint64_t client_id, std::uint32_t request_id,
                           std::uint8_t receiver, std::uint32_t error) noexcept
{
    auto& trace = *static_cast<ServerWorkerTrace*>(context);
    std::lock_guard<std::mutex> lock(trace.mutex);
    trace.completed.push_back(
        WorkerEvent{0U, operation, client_id, request_id, receiver, error});
}

void record_workers_joined(void* context, std::size_t live_threads) noexcept
{
    auto& trace = *static_cast<ServerWorkerTrace*>(context);
    std::lock_guard<std::mutex> lock(trace.mutex);
    trace.live_threads_after_join = live_threads;
}

template <typename Predicate>
bool wait_for(std::chrono::milliseconds timeout, Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return predicate();
}

bool test_real_server_shutdown_wakes_blocked_services()
{
    std::string pattern = test::temporary_directory_template("asicen-shutdown-wiring-");
    char* made = ::mkdtemp(pattern.data());
    if (made == nullptr) return false;
    const std::string root(made);
    const std::string instance("shutdown-proof");
    StopTrace trace;
    Tuner backend(trace);
    CardBackend card_backend(trace);
    CardSession card_session(trace);
    Stream stream(trace);
    PosixTunerNonceSource nonce;
    Time time;
    CardService card(card_backend, card_session);
    TunerService tuner(backend, nonce, time, nullptr, nullptr, &stream);
    const auto* profile = asicen::find_profile(asicen::ModelId::S3u);
    if (profile == nullptr) return false;
    wire::Epoch epoch{};
    epoch[0] = 1U;
    ControlContext context;
    if (!asicen::build_asicen_mock_control_context(*profile, tuner, instance.c_str(), epoch,
                                                  context)) return false;
    const EndpointConfig endpoint{root.c_str(), instance.c_str(), kControlEndpointName,
                                  EndpointAccess::private_user};
    auto server = PosixControlServer::create(endpoint, card, tuner, context, &stream);
    if (!server) {
        std::filesystem::remove_all(root);
        return false;
    }
    const auto acquired = tuner.acquire(77U, 0U);
    if (!acquired) return false;
    const TuneRequestPayload request{acquired.value().lease_id, System::ISDB_T,
                                     557142U, 0xffffU, 0xffffU, 6000000U, 0U, 30000U};
    std::atomic<bool> active_done{false};
    std::atomic<bool> queued_started{false};
    std::atomic<bool> queued_done{false};
    std::atomic<bool> detect_done{false};
    std::atomic<bool> initialize_done{false};
    std::atomic<bool> stream_done{false};
    std::thread active([&] {
        (void)tuner.tune(77U, request);
        active_done.store(true, std::memory_order_release);
    });
    const bool active_entered = wait_for(std::chrono::seconds(1), [&] {
        return backend.entered.load(std::memory_order_acquire);
    });
    if (!active_entered) {
        backend.request_stop();
        active.join();
        server.value()->shutdown();
        server.value().reset();
        std::filesystem::remove_all(root);
        return false;
    }
    std::thread queued([&] {
        queued_started.store(true, std::memory_order_release);
        (void)tuner.tune(77U, request);
        queued_done.store(true, std::memory_order_release);
    });
    std::thread detect([&] {
        (void)card_backend.detect_card();
        detect_done.store(true, std::memory_order_release);
    });
    std::thread initialize([&] {
        (void)card_session.initialize();
        initialize_done.store(true, std::memory_order_release);
    });
    const TunerAttachment attachment{};
    std::thread stream_read([&] {
        (void)stream.read(attachment, MutableByteView{nullptr, 0U}, Timeout{30000U});
        stream_done.store(true, std::memory_order_release);
    });
    const bool entered = wait_for(std::chrono::seconds(1), [&] {
        return queued_started.load(std::memory_order_acquire) &&
               card_backend.entered.load(std::memory_order_acquire) &&
               card_session.entered.load(std::memory_order_acquire) &&
               stream.entered.load(std::memory_order_acquire);
    });
    if (!entered) {
        backend.request_stop();
        card_backend.request_stop();
        card_session.request_stop();
        stream.request_stop();
        active.join();
        queued.join();
        detect.join();
        initialize.join();
        stream_read.join();
        server.value()->shutdown();
        server.value().reset();
        std::filesystem::remove_all(root);
        return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const bool queued_blocked_before_shutdown =
        !queued_done.load(std::memory_order_acquire);

    std::atomic<bool> shutdown_returned{false};
    std::atomic<bool> watchdog_released_gates{false};
    std::thread watchdog([&] {
        if (!wait_for(std::chrono::seconds(3), [&] {
                return shutdown_returned.load(std::memory_order_acquire);
            })) {
            watchdog_released_gates.store(true, std::memory_order_release);
            backend.request_stop();
            card_backend.request_stop();
            card_session.request_stop();
            stream.request_stop();
        }
    });
    const auto started = std::chrono::steady_clock::now();
    const auto stopped = server.value()->shutdown();
    shutdown_returned.store(true, std::memory_order_release);
    watchdog.join();
    const bool joined = wait_for(std::chrono::seconds(2), [&] {
        return active_done.load(std::memory_order_acquire) &&
               queued_done.load(std::memory_order_acquire) &&
               detect_done.load(std::memory_order_acquire) &&
               initialize_done.load(std::memory_order_acquire) &&
               stream_done.load(std::memory_order_acquire);
    });
    if (!joined) {
        backend.request_stop();
        card_backend.request_stop();
        card_session.request_stop();
        stream.request_stop();
    }
    active.join();
    queued.join();
    detect.join();
    initialize.join();
    stream_read.join();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const std::vector<std::string> expected{"tuner", "card-backend", "card-session", "stream"};
    const bool ok = stopped && joined && queued_blocked_before_shutdown &&
        !watchdog_released_gates.load(std::memory_order_acquire) &&
        elapsed < std::chrono::seconds(2) &&
        backend.stopped.load() && card_backend.stopped.load() && card_session.stopped.load() &&
        stream.stopped.load() && active_done.load() && queued_done.load() && detect_done.load() &&
        initialize_done.load() && stream_done.load() && trace.events == expected;
    server.value().reset();
    std::filesystem::remove_all(root);
    if (!ok) std::fprintf(stderr, "real server shutdown wiring/order proof failed\n");
    return ok;
}

bool test_shutdown_drains_server_owned_same_lane_tasks()
{
    std::string pattern = test::temporary_directory_template("asicen-shutdown-queue-");
    char* made = ::mkdtemp(pattern.data());
    if (made == nullptr) return false;
    const std::string root(made);
    const std::string instance("shutdown-queue-proof");
    StopTrace stop_trace;
    QueuedBackend backend;
    CardBackend card_backend(stop_trace);
    CardSession card_session(stop_trace);
    PosixTunerNonceSource nonce;
    Time time;
    CardService card(card_backend, card_session);
    TunerService tuner(backend, nonce, time);
    const auto* profile = asicen::find_profile(asicen::ModelId::W3u3);
    wire::Epoch epoch{};
    epoch[0] = 0x71U;
    ControlContext context;
    if (profile == nullptr || !tuner.configure_strict_capabilities() ||
        !asicen::build_asicen_mock_control_context(*profile, tuner, instance.c_str(), epoch,
                                                   context)) {
        std::filesystem::remove_all(root);
        return false;
    }
    const EndpointConfig endpoint{root.c_str(), instance.c_str(), kControlEndpointName,
                                  EndpointAccess::private_user};
    auto created = PosixControlServer::create(endpoint, card, tuner, context);
    if (!created) {
        std::filesystem::remove_all(root);
        return false;
    }
    auto server = std::move(created.value());
    ServerWorkerTrace worker_trace;
    ControlServerTestHooks hooks;
    hooks.worker_task_submitted = &record_task_submitted;
    hooks.worker_task_completed = &record_task_completed;
    hooks.workers_joined = &record_workers_joined;
    hooks.context = &worker_trace;
    set_control_server_test_hooks(&hooks);

    std::atomic<bool> poller_running{true};
    std::atomic<bool> poller_ok{true};
    std::thread poller([&] {
        while (poller_running.load(std::memory_order_acquire)) {
            if (!server->poll_once(Timeout{10U})) {
                poller_ok.store(false, std::memory_order_release);
                return;
            }
        }
    });

    bool admitted_pair = false;
    bool clients_distinct = false;
    bool lane_active_and_queued = false;
    auto first = ShutdownWireClient::connect(endpoint);
    auto second = ShutdownWireClient::connect(endpoint);
    const bool clients_connected = first != nullptr && second != nullptr;
    const bool hellos_ok = clients_connected && first->hello() && second->hello();
    std::uint64_t first_lease = 0U;
    std::uint64_t second_lease = 0U;
    const bool acquires_ok = hellos_ok && first->acquire(0U, first_lease) &&
        second->acquire(1U, second_lease);
    const bool first_tune_sent = acquires_ok && first->send_tune(first_lease);
    if (first_tune_sent) {
        const bool active_entered = wait_for(std::chrono::seconds(2), [&] {
            return backend.active_tune_entered.load(std::memory_order_acquire);
        });
        const bool second_tune_sent = active_entered && second->send_tune(second_lease);
        if (second_tune_sent) {
            admitted_pair = wait_for(std::chrono::seconds(2), [&] {
                std::lock_guard<std::mutex> lock(worker_trace.mutex);
                std::size_t tunes = 0U;
                for (const WorkerEvent& event : worker_trace.submitted) {
                    if (event.operation == static_cast<std::uint8_t>(
                            ControlWorkerOperation::tuner_tune)) ++tunes;
                }
                return tunes == 2U;
            });
            std::lock_guard<std::mutex> lock(worker_trace.mutex);
            std::vector<WorkerEvent> tunes;
            for (const WorkerEvent& event : worker_trace.submitted) {
                if (event.operation == static_cast<std::uint8_t>(
                        ControlWorkerOperation::tuner_tune)) tunes.push_back(event);
            }
            if (tunes.size() == 2U) {
                clients_distinct = tunes[0].client_id != tunes[1].client_id &&
                    tunes[0].receiver != tunes[1].receiver &&
                    tunes[0].lane == static_cast<std::uint8_t>(ControlWorkerLane::tuner_dev1) &&
                    tunes[1].lane == tunes[0].lane;
                lane_active_and_queued = backend.active_tune_entered.load() &&
                    !backend.receiver_one_tune_entered.load();
            }
        }
    }

    poller_running.store(false, std::memory_order_release);
    if (poller.joinable()) poller.join();
    std::atomic<bool> shutdown_returned{false};
    std::atomic<bool> watchdog_released_gate{false};
    std::thread watchdog([&] {
        if (!wait_for(std::chrono::seconds(3), [&] {
                return shutdown_returned.load(std::memory_order_acquire);
            })) {
            watchdog_released_gate.store(true, std::memory_order_release);
            backend.request_stop();
        }
    });
    const auto shutdown = server->shutdown();
    shutdown_returned.store(true, std::memory_order_release);
    watchdog.join();

    std::vector<WorkerEvent> tune_submitted;
    std::vector<WorkerEvent> tune_completed;
    std::size_t live_threads_after_join = std::numeric_limits<std::size_t>::max();
    {
        std::lock_guard<std::mutex> lock(worker_trace.mutex);
        for (const WorkerEvent& event : worker_trace.submitted) {
            if (event.operation == static_cast<std::uint8_t>(
                    ControlWorkerOperation::tuner_tune)) tune_submitted.push_back(event);
        }
        for (const WorkerEvent& event : worker_trace.completed) {
            if (event.operation == static_cast<std::uint8_t>(
                    ControlWorkerOperation::tuner_tune)) tune_completed.push_back(event);
        }
        live_threads_after_join = worker_trace.live_threads_after_join;
    }
    const auto cancelled_result = [](const WorkerEvent& event) {
        return event.error == static_cast<std::uint32_t>(Error::NOT_READY) ||
               event.error == static_cast<std::uint32_t>(Error::NOT_FOUND);
    };
    const bool both_results_cancelled = tune_completed.size() == 2U &&
        cancelled_result(tune_completed[0]) && cancelled_result(tune_completed[1]);
    const bool completion_owners_match = tune_submitted.size() == 2U &&
        tune_completed.size() == 2U &&
        ((tune_completed[0].client_id == tune_submitted[0].client_id &&
          tune_completed[1].client_id == tune_submitted[1].client_id) ||
         (tune_completed[0].client_id == tune_submitted[1].client_id &&
          tune_completed[1].client_id == tune_submitted[0].client_id));
    const bool ok = admitted_pair && clients_distinct && lane_active_and_queued &&
        poller_ok.load(std::memory_order_acquire) && shutdown &&
        !watchdog_released_gate.load(std::memory_order_acquire) &&
        both_results_cancelled && completion_owners_match &&
        live_threads_after_join == 0U &&
        (!backend.receiver_one_tune_entered.load(std::memory_order_acquire) ||
         backend.receiver_one_tune_entered_after_stop.load(std::memory_order_acquire));
    set_control_server_test_hooks(nullptr);
    server.reset();
    first.reset();
    second.reset();
    std::filesystem::remove_all(root);
    if (!ok) {
        std::fprintf(stderr,
            "server-owned same-lane shutdown proof failed connected=%d hellos=%d acquires=%d first_tune=%d admitted=%d owners=%d active_queued=%d poller=%d shutdown=%d watchdog=%d submitted=%zu completed=%zu errors=%u,%u cancelled=%d match=%d joined=%zu receiver1=%d after_stop=%d\n",
            clients_connected, hellos_ok, acquires_ok, first_tune_sent,
            admitted_pair, clients_distinct, lane_active_and_queued,
            poller_ok.load(std::memory_order_acquire), static_cast<bool>(shutdown),
            watchdog_released_gate.load(std::memory_order_acquire), tune_submitted.size(),
            tune_completed.size(), tune_completed.size() > 0U ? tune_completed[0].error : 999U,
            tune_completed.size() > 1U ? tune_completed[1].error : 999U,
            both_results_cancelled, completion_owners_match, live_threads_after_join,
            backend.receiver_one_tune_entered.load(),
            backend.receiver_one_tune_entered_after_stop.load());
    }
    return ok;
}
}  // namespace

int main()
{
    return test_real_server_shutdown_wakes_blocked_services() &&
           test_shutdown_drains_server_owned_same_lane_tasks() ? 0 : 1;
}
