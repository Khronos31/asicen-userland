// SPDX-License-Identifier: GPL-2.0-only
#include "px4/card_service.h"
#include "px4/control_client.h"
#include "px4/control_server.h"
#include "px4/tuner_service.h"
#include "control_server_test_access.h"
#include "control_workers.h"
#include "test_temp_directory.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <condition_variable>
#include <mutex>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <utility>
#include <unistd.h>
#include <vector>

namespace {

using namespace px4::userland;
using namespace px4::userland::ipc;
using namespace px4::userland::ipc::posix;

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            std::fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__,       \
                         #condition);                                                       \
            return false;                                                                   \
        }                                                                                   \
    } while (false)

class TempRuntime final {
public:
    TempRuntime()
    {
        std::string pattern = test::temporary_directory_template("px4-control-");
        char* made = ::mkdtemp(pattern.data());
        if (made != nullptr) {
            path_ = made;
            valid_ = ::chmod(path_.c_str(), 0700) == 0;
        }
    }
    ~TempRuntime() noexcept
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    bool valid() const noexcept { return valid_; }
    const std::string& path() const noexcept { return path_; }
private:
    std::string path_;
    bool valid_ = false;
};

class Backend final : public CardServiceBackend {
public:
    Result<void> set_power(bool on) noexcept override
    {
        power_calls.push_back(on);
        powered = on;
        return Result<void>::success();
    }
    Result<void> initialize_uart() noexcept override
    {
        ++uart_calls;
        return Result<void>::success();
    }
    Result<bool> detect_card() noexcept override
    {
        return Result<bool>::success(present.load());
    }
    std::vector<bool> power_calls;
    std::size_t uart_calls = 0U;
    bool powered = false;
    std::atomic<bool> present{true};
};

class TunerBackend final : public TunerServiceBackend {
public:
    Result<void> open_receiver(std::uint8_t receiver) noexcept override
    {
        ++open_calls[receiver];
        if (receiver == 0U && block_receiver_zero.load()) {
            {
                std::lock_guard<std::mutex> lock(receiver_zero_mutex);
                receiver_zero_started.store(true);
            }
            receiver_zero_condition.notify_all();
            while (block_receiver_zero.load()) std::this_thread::yield();
        }
        if (open_error != Error::OK) return Result<void>::failure(open_error);
        return Result<void>::success();
    }
    Result<void> tune_terrestrial(std::uint8_t receiver, std::uint32_t,
                                  std::uint32_t) noexcept override
    {
        if (receiver == 0U && block_tune_receiver_zero.load()) {
            {
                std::lock_guard<std::mutex> lock(tune_receiver_zero_mutex);
                tune_receiver_zero_started.store(true);
            }
            tune_receiver_zero_condition.notify_all();
            while (block_tune_receiver_zero.load()) std::this_thread::yield();
        }
        last_system[receiver] = System::ISDB_T;
        ++tune_calls[receiver];
        return Result<void>::success();
    }
    Result<void> tune_satellite(std::uint8_t receiver, std::uint32_t,
                                std::uint32_t) noexcept override
    {
        if (receiver == 0U && block_tune_receiver_zero.load()) {
            {
                std::lock_guard<std::mutex> lock(tune_receiver_zero_mutex);
                tune_receiver_zero_started.store(true);
            }
            tune_receiver_zero_condition.notify_all();
            while (block_tune_receiver_zero.load()) std::this_thread::yield();
        }
        last_system[receiver] = System::ISDB_S;
        ++tune_calls[receiver];
        return Result<void>::success();
    }
    Result<bool> is_locked(std::uint8_t, System) noexcept override
    { return Result<bool>::success(true); }
    Result<void> select_satellite_slot(std::uint8_t, std::uint8_t,
                                       std::uint32_t) noexcept override
    { return Result<void>::success(); }
    Result<void> select_satellite_tsid(std::uint8_t, std::uint16_t,
                                       std::uint32_t) noexcept override
    { return Result<void>::success(); }
    Result<void> close_receiver(std::uint8_t receiver) noexcept override
    {
        ++close_calls[receiver];
        return Result<void>::success();
    }

    Result<void> begin_tune_power(std::uint8_t, System system,
                                  std::uint8_t voltage) noexcept override
    {
        if (system == System::ISDB_S && voltage == 15U) {
            if (!allow_lnb_15v.load())
                return Result<void>::failure(Error::UNSUPPORTED);
            ++lnb_gpio_calls;
        }
        return Result<void>::success();
    }

    Result<void> start_capture(std::uint8_t receiver, System system) noexcept override
    {
        ++start_capture_calls[receiver];
        last_capture_system[receiver] = system;
        if (receiver == 2U && block_start_capture.load()) {
            std::unique_lock<std::mutex> lock(start_capture_mutex);
            start_capture_started.store(true);
            start_capture_condition.notify_all();
            start_capture_condition.wait(lock, [&]() noexcept {
                return !block_start_capture.load();
            });
        }
        return Result<void>::success();
    }

    Result<void> stop_capture(std::uint8_t receiver, System system) noexcept override
    {
        ++stop_capture_calls[receiver];
        last_capture_system[receiver] = system;
        return Result<void>::success();
    }

    Error open_error = Error::OK;
    std::atomic<bool> allow_lnb_15v{false};
    std::atomic<std::size_t> lnb_gpio_calls{0U};
    std::array<std::size_t, kReceiverCount> open_calls{};
    std::array<std::size_t, kReceiverCount> tune_calls{};
    std::array<std::size_t, kReceiverCount> close_calls{};
    std::array<std::size_t, kReceiverCount> start_capture_calls{};
    std::array<std::size_t, kReceiverCount> stop_capture_calls{};
    std::array<System, kReceiverCount> last_system{};
    std::array<System, kReceiverCount> last_capture_system{};
    std::atomic<bool> block_receiver_zero{false};
    std::atomic<bool> receiver_zero_started{false};
    std::mutex receiver_zero_mutex;
    std::condition_variable receiver_zero_condition;
    std::atomic<bool> block_tune_receiver_zero{false};
    std::atomic<bool> tune_receiver_zero_started{false};
    std::mutex tune_receiver_zero_mutex;
    std::condition_variable tune_receiver_zero_condition;
    std::atomic<bool> block_start_capture{false};
    std::atomic<bool> start_capture_started{false};
    std::mutex start_capture_mutex;
    std::condition_variable start_capture_condition;
};

class ControlStream final : public TunerStreamControl {
public:
    Result<void> attach(const TunerAttachment& value) noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (std::size_t index = 0U; index < active.size(); ++index) {
            if (active[index] && active_identities[index].attachment_id ==
                    value.attachment_id) {
                return Result<void>::failure(Error::BUSY);
            }
        }
        if (value.receiver >= active.size())
            return Result<void>::failure(Error::INVALID_ARGUMENT);
        active[value.receiver] = true;
        active_identities[value.receiver] = value;
        identity = value;
        attached = true;
        final_available_by_receiver[value.receiver] = false;
        final_terminal = TunerStreamTerminal::stopped;
        return Result<void>::success();
    }

    Result<void> detach(const TunerAttachment& value) noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex);
        std::size_t receiver = active.size();
        for (std::size_t index = 0U; index < active.size(); ++index) {
            if (active[index] &&
                active_identities[index].attachment_id == value.attachment_id) {
                receiver = index;
                break;
            }
        }
        if (receiver == active.size())
            return Result<void>::failure(Error::NOT_FOUND);
        active[receiver] = false;
        attached = false;
        for (const bool value_active : active) attached = attached || value_active;
        ++detach_calls;
        final_identity = value;
        final_available = true;
        final_identities[receiver] = value;
        final_available_by_receiver[receiver] = true;
        final_terminals[receiver] = final_terminal;
        changed.notify_all();
        return Result<void>::success();
    }

    Result<TunerStreamCounters> stats(
        const TunerAttachment& value) const noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (std::size_t index = 0U; index < active.size(); ++index) {
            if (active[index] && active_identities[index].attachment_id ==
                    value.attachment_id)
                return Result<TunerStreamCounters>::success(counters);
        }
        return Result<TunerStreamCounters>::failure(Error::NOT_FOUND);
    }

    Result<TunerStreamFinalSnapshot> final_snapshot(
        const TunerAttachment& value) const noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (std::size_t index = 0U; index < final_available_by_receiver.size();
             ++index) {
            if (final_available_by_receiver[index] &&
                final_identities[index].attachment_id == value.attachment_id)
                return Result<TunerStreamFinalSnapshot>::success(
                    TunerStreamFinalSnapshot{
                        counters, static_cast<std::uint8_t>(final_terminals[index])});
        }
        return Result<TunerStreamFinalSnapshot>::failure(Error::NOT_FOUND);
    }

    Result<TunerStreamReadResult> read(
        const TunerAttachment& value, MutableByteView output, Timeout) noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex);
        bool active_match = false;
        for (std::size_t index = 0U; index < active.size(); ++index) {
            if (active[index] && active_identities[index].attachment_id ==
                    value.attachment_id) {
                active_match = true;
                break;
            }
        }
        if (active_match && (emit_data || continuous_data)) {
            if (output.size < data_packet.size())
                return Result<TunerStreamReadResult>::failure(Error::BUFFER_TOO_SMALL);
            std::memcpy(output.data, data_packet.data(), data_packet.size());
            if (!continuous_data) emit_data = false;
            return Result<TunerStreamReadResult>::success(
                TunerStreamReadResult{data_packet.size(), false, false,
                                      TunerStreamTerminal::none});
        }
        if (active_match)
            return Result<TunerStreamReadResult>::success(
                TunerStreamReadResult{0U, false, true, TunerStreamTerminal::none});
        for (std::size_t index = 0U; index < final_available_by_receiver.size();
             ++index) {
            if (final_available_by_receiver[index] &&
                final_identities[index].attachment_id == value.attachment_id)
                return Result<TunerStreamReadResult>::success(
                    TunerStreamReadResult{0U, true, false, final_terminals[index]});
        }
        return Result<TunerStreamReadResult>::failure(Error::NOT_FOUND);
    }

    bool wait_for_detaches(std::size_t expected) noexcept
    {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(5), [&]() noexcept {
            return detach_calls >= expected;
        });
    }

    TunerStreamCounters counters{17U, 3196U, 1U, 2U, 3U, 4U, 5U, 6U};
    mutable std::mutex mutex;
    TunerAttachment identity{};
    TunerAttachment final_identity{};
    bool attached = false;
    bool final_available = false;
    bool emit_data = false;
    bool continuous_data = false;
    TunerStreamTerminal final_terminal = TunerStreamTerminal::stopped;
    std::size_t detach_calls = 0U;
    std::condition_variable changed;
    std::array<std::uint8_t, 188U> data_packet{};
    std::array<bool, kReceiverCount> active{};
    std::array<TunerAttachment, kReceiverCount> active_identities{};
    std::array<bool, kReceiverCount> final_available_by_receiver{};
    std::array<TunerAttachment, kReceiverCount> final_identities{};
    std::array<TunerStreamTerminal, kReceiverCount> final_terminals{};
};

class TunerNonce final : public TunerNonceSource {
public:
    Result<std::array<std::uint8_t, kNonceLength>> generate() noexcept override
    {
        std::array<std::uint8_t, kNonceLength> value{};
        value[0] = next++;
        return Result<std::array<std::uint8_t, kNonceLength>>::success(value);
    }
private:
    std::uint8_t next = 1U;
};

class TunerTime final : public TunerServiceTime {
public:
    std::uint64_t monotonic_ms() noexcept override { return now; }
    void sleep_ms(std::uint32_t milliseconds) noexcept override { now += milliseconds; }
private:
    std::uint64_t now = 0U;
};

class BlockedOperationGuard final {
public:
    BlockedOperationGuard(std::atomic<bool>& blocked,
                          std::condition_variable& condition,
                          std::thread& request,
                          std::atomic<bool>& stop, std::thread& server_thread,
                          PosixControlServer& server) noexcept
        : blocked_(blocked), condition_(condition), request_(request), stop_(stop),
          server_thread_(server_thread), server_(server)
    {
    }

    ~BlockedOperationGuard() noexcept
    {
        if (!active_) return;
        unblock_and_join();
        stop_.store(true);
        if (server_thread_.joinable()) server_thread_.join();
        (void)server_.shutdown();
    }

    void unblock_and_join() noexcept
    {
        blocked_.store(false);
        condition_.notify_all();
        if (request_.joinable()) request_.join();
    }

    void dismiss() noexcept { active_ = false; }

private:
    std::atomic<bool>& blocked_;
    std::condition_variable& condition_;
    std::thread& request_;
    std::atomic<bool>& stop_;
    std::thread& server_thread_;
    PosixControlServer& server_;
    bool active_ = true;
};

class ServerThreadGuard final {
public:
    ServerThreadGuard(std::atomic<bool>& stop, std::thread& thread,
                      PosixControlServer& server) noexcept
        : stop_(stop), thread_(thread), server_(server)
    {
    }

    ~ServerThreadGuard() noexcept
    {
        if (!active_) return;
        stop_.store(true);
        if (thread_.joinable()) thread_.join();
        (void)server_.shutdown();
    }

    void dismiss() noexcept { active_ = false; }

private:
    std::atomic<bool>& stop_;
    std::thread& thread_;
    PosixControlServer& server_;
    bool active_ = true;
};

class AttachCompletionBarrier final {
public:
    static void before_completion(void* context) noexcept
    {
        auto& barrier = *static_cast<AttachCompletionBarrier*>(context);
        std::unique_lock<std::mutex> lock(barrier.mutex);
        barrier.entered = true;
        barrier.changed.notify_all();
        barrier.changed.wait(lock, [&]() noexcept { return barrier.released; });
    }

    bool wait_until_entered() noexcept
    {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(5), [&]() noexcept {
            return entered;
        });
    }

    void release() noexcept
    {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
        changed.notify_all();
    }

private:
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool released = false;
};

class AttachCompletionReleaseGuard final {
public:
    explicit AttachCompletionReleaseGuard(AttachCompletionBarrier& barrier) noexcept
        : barrier_(barrier)
    {
    }

    ~AttachCompletionReleaseGuard() noexcept
    {
        if (active_) barrier_.release();
    }

    void dismiss() noexcept { active_ = false; }

private:
    AttachCompletionBarrier& barrier_;
    bool active_ = true;
};

class StreamServerObservation final {
public:
    static void count_changed(void* context, std::size_t live,
                              std::size_t retired) noexcept
    {
        auto& observation = *static_cast<StreamServerObservation*>(context);
        std::lock_guard<std::mutex> lock(observation.mutex);
        observation.live = live;
        observation.retired = retired;
        if (retired != 0U) observation.saw_retired = true;
        observation.changed.notify_all();
    }

    static void partial_frame(void* context, std::uint64_t) noexcept
    {
        auto& observation = *static_cast<StreamServerObservation*>(context);
        std::lock_guard<std::mutex> lock(observation.mutex);
        ++observation.partial_frames;
        observation.changed.notify_all();
    }

    static void write_blocked(void* context, std::uint64_t) noexcept
    {
        auto& observation = *static_cast<StreamServerObservation*>(context);
        std::lock_guard<std::mutex> lock(observation.mutex);
        ++observation.write_blocks;
        observation.changed.notify_all();
    }

    bool wait_for_capacity(std::size_t expected_live,
                           std::size_t expected_retired) noexcept
    {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(5), [&]() noexcept {
            return live == expected_live && retired == expected_retired;
        });
    }

    bool wait_for_retired_owner(std::size_t expected_live) noexcept
    {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(5), [&]() noexcept {
            return live == expected_live && saw_retired;
        });
    }

    bool wait_for_partial() noexcept
    {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(5), [&]() noexcept {
            return partial_frames != 0U;
        });
    }

    bool wait_for_write_blocked() noexcept
    {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(5), [&]() noexcept {
            return write_blocks != 0U;
        });
    }

    std::mutex mutex;
    std::condition_variable changed;
    std::size_t live = 0U;
    std::size_t retired = 0U;
    bool saw_retired = false;
    std::size_t partial_frames = 0U;
    std::size_t write_blocks = 0U;
};

class StreamServerHookGuard final {
public:
    explicit StreamServerHookGuard(StreamServerObservation& observation) noexcept
    {
        hooks_.stream_count_changed = &StreamServerObservation::count_changed;
        hooks_.stream_partial_frame = &StreamServerObservation::partial_frame;
        hooks_.stream_write_blocked = &StreamServerObservation::write_blocked;
        hooks_.context = &observation;
        set_control_server_test_hooks(&hooks_);
    }

    ~StreamServerHookGuard() noexcept
    {
        set_control_server_test_hooks(nullptr);
    }

    StreamServerHookGuard(const StreamServerHookGuard&) = delete;
    StreamServerHookGuard& operator=(const StreamServerHookGuard&) = delete;

private:
    ControlServerTestHooks hooks_{};
};

class FrameCollector final : public FrameConsumer {
public:
    FrameCollector() : payloads(64U) {}

    Result<void> on_frame(const FrameView& frame) noexcept override
    {
        if (count >= headers.size() || frame.payload.size > payloads[0U].size()) {
            return Result<void>::failure(Error::BUFFER_TOO_SMALL);
        }
        headers[count] = frame.header;
        if (frame.payload.size != 0U) {
            std::memcpy(payloads[count].data(), frame.payload.data, frame.payload.size);
        }
        payload_sizes[count] = frame.payload.size;
        ++count;
        return Result<void>::success();
    }

    void clear() noexcept
    {
        count = 0U;
        payload_sizes.fill(0U);
    }

    // A single nonblocking read can legally contain many small stream frames.
    // Keep enough bounded storage for one maximum transport read.  The payload
    // backing is on the heap because collectors also live on worker threads.
    std::array<FrameHeader, 64U> headers{};
    std::vector<std::array<std::uint8_t, 1024U>> payloads;
    std::array<std::size_t, 64U> payload_sizes{};
    std::size_t count = 0U;
};

Result<void> send_raw_request(SocketStream& stream, MessageType type,
                              std::uint32_t request_id, ByteView payload) noexcept
{
    std::vector<std::uint8_t> frame(kFrameHeaderSize + kMaxControlPayload, 0U);
    const auto encoded = encode_frame(
        FrameHeader{kProtocolMajor, kProtocolMinor, type, MessageKind::request,
                    request_id, static_cast<std::uint32_t>(payload.size)},
        payload, MutableByteView{frame.data(), frame.size()});
    if (!encoded) return Result<void>::failure(encoded.error());
    return stream.write_frame(ByteView{frame.data(), encoded.value()}, Timeout{2000U});
}

bool send_socket_bytes(SocketStream& stream, const std::uint8_t* data,
                       std::size_t size) noexcept
{
    std::size_t offset = 0U;
    while (offset < size) {
#if defined(MSG_NOSIGNAL)
        constexpr int flags = MSG_NOSIGNAL;
#else
        constexpr int flags = 0;
#endif
        const ssize_t sent = ::send(stream.native_handle(), data + offset,
                                    size - offset, flags);
        if (sent > 0) {
            offset += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent < 0 && errno == EINTR) continue;
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd descriptor{stream.native_handle(), POLLOUT, 0};
            if (::poll(&descriptor, 1U, 2000) > 0) continue;
        }
        return false;
    }
    return true;
}

bool read_raw_frames(SocketStream& stream, StreamFramer& framer,
                     FrameCollector& collector, std::size_t count) noexcept
{
    std::array<std::uint8_t, 8192U> input{};
    for (std::size_t attempt = 0U; attempt < 200U && collector.count < count; ++attempt) {
        const auto read = stream.read_frames(
            MutableByteView{input.data(), input.size()}, framer, collector,
            Timeout{20U});
        if (!read) return false;
    }
    return collector.count >= count;
}

bool read_until_stream_end(SocketStream& stream, StreamFramer& framer,
                           StreamEndEventPayload& end) noexcept
{
    FrameCollector frames;
    for (std::size_t attempt = 0U; attempt < 500U; ++attempt) {
        frames.clear();
        if (!read_raw_frames(stream, framer, frames, 1U)) return false;
        for (std::size_t index = 0U; index < frames.count; ++index) {
            if (frames.headers[index].type != MessageType::STREAM_END ||
                frames.headers[index].kind != MessageKind::event)
                continue;
            const auto decoded = decode_stream_end_event_payload(
                ByteView{frames.payloads[index].data(), frames.payload_sizes[index]});
            if (!decoded) return false;
            end = decoded.value();
            return true;
        }
    }
    return false;
}

bool raw_hello(SocketStream& stream, StreamFramer& framer) noexcept
{
    std::array<std::uint8_t, 12U> payload{};
    const auto payload_size = encode_payload(
        HelloRequestPayload{kProtocolMajor, kProtocolMinor, kProtocolMajor,
                            kProtocolMinor, 0U},
        MutableByteView{payload.data(), payload.size()});
    if (!payload_size || !send_raw_request(
            stream, MessageType::HELLO, 1U,
            ByteView{payload.data(), payload_size.value()})) {
        return false;
    }
    FrameCollector frames;
    return read_raw_frames(stream, framer, frames, 1U) &&
           frames.headers[0U].type == MessageType::HELLO &&
           frames.headers[0U].kind == MessageKind::response;
}

class Session final : public CardProtocolSession {
public:
    Session()
    {
        atr_.bytes[0] = 0x3bU;
        atr_.bytes[1] = 0x00U;
        atr_.length = 2U;
    }
    Result<void> initialize() noexcept override
    {
        initialized_ = true;
        ++initialize_calls;
        return Result<void>::success();
    }
    Result<std::size_t> transmit(ByteView apdu, MutableByteView response) noexcept override
    {
        ++transmit_calls;
        if (!initialized_ || apdu.size == 0U || response.size < 2U) {
            return Result<std::size_t>::failure(Error::INVALID_ARGUMENT);
        }
        response.data[0] = 0x61U;
        response.data[1] = 0x00U;
        return Result<std::size_t>::success(2U);
    }
    bool initialized() const noexcept override { return initialized_; }
    const CardAtr& atr() const noexcept override { return atr_; }
    void invalidate() noexcept override { initialized_ = false; }
    std::size_t initialize_calls = 0U;
    std::size_t transmit_calls = 0U;
private:
    CardAtr atr_{};
    bool initialized_ = false;
};

class EventSink final : public ControlEventSink {
public:
    void on_device_event(const DeviceEventPayload& event) noexcept override
    {
        last = event;
        ++count;
    }
    DeviceEventPayload last{};
    std::size_t count = 0U;
};

template <typename Payload>
Result<ControlResponse> request(PosixControlClient& client, MessageType type,
                                const Payload& payload,
                                ControlEventSink* sink = nullptr) noexcept
{
    std::array<std::uint8_t, kMaxControlPayload> encoded{};
    const auto size = encode_payload(payload,
        MutableByteView{encoded.data(), encoded.size()});
    if (!size) return Result<ControlResponse>::failure(size.error());
    return client.request(type, ByteView{encoded.data(), size.value()}, Timeout{2000U}, sink);
}

Result<ControlResponse> empty_request(PosixControlClient& client,
                                      MessageType type,
                                      ControlEventSink* sink = nullptr) noexcept
{
    return client.request(type, ByteView{nullptr, 0U}, Timeout{2000U}, sink);
}

bool test_control_server_card_flow_and_shutdown()
{
    TempRuntime runtime;
    CHECK(runtime.valid());
    constexpr const char* serial = "00001205000960";
    const EndpointConfig endpoint{runtime.path().c_str(), serial, kControlEndpointName,
                                  EndpointAccess::private_user};
    Backend backend;
    Session session;
    CardService service(backend, session);
    TunerBackend tuner_backend;
    TunerNonce tuner_nonce;
    TunerTime tuner_time;
    TunerService tuner_service(tuner_backend, tuner_nonce, tuner_time);
    auto server_result = PosixControlServer::create(
        endpoint, service, tuner_service, serial);
    CHECK(server_result);
    std::unique_ptr<PosixControlServer> server = std::move(server_result.value());
    const std::string socket_path = server->endpoint_path();
    const std::string stream_socket_path = server->stream_endpoint_path();
    const std::string product_path = runtime.path() + "/px4-userland";
    const std::string instance_path = product_path + "/" + serial;

    std::atomic<bool> stop{false};
    std::atomic<bool> server_ok{true};
    std::thread server_thread([&]() {
        while (!stop.load()) {
            const auto polled = server->poll_once(Timeout{20U});
            if (!polled) {
                server_ok.store(false);
                return;
            }
        }
    });

    auto first_result = PosixControlClient::connect(
        endpoint, kCapabilityCard | kCapabilityEvents, Timeout{2000U});
    auto second_result = PosixControlClient::connect(
        endpoint, kCapabilityCard, Timeout{2000U});
    CHECK(first_result && second_result);
    std::unique_ptr<PosixControlClient> first = std::move(first_result.value());
    std::unique_ptr<PosixControlClient> second = std::move(second_result.value());
    CHECK((first->negotiated_capabilities() &
           (kCapabilityCard | kCapabilityEvents)) ==
          (kCapabilityCard | kCapabilityEvents));

    const auto listed = empty_request(*first, MessageType::LIST);
    CHECK(listed);
    const auto list = decode_list_response_payload(
        ByteView{listed.value().payload.data(), listed.value().payload.size()});
    CHECK(list && list.value().receivers.size() == 8U && list.value().ready == 1U);
    CHECK(list.value().receivers[0].system == System::ISDB_S);
    CHECK(list.value().receivers[7].global_id == 7U);

    const auto daemon_status = empty_request(*first, MessageType::STATUS);
    CHECK(daemon_status);
    const auto status = decode_status_response_payload(
        ByteView{daemon_status.value().payload.data(),
                 daemon_status.value().payload.size()});
    CHECK(status && status.value().receiver_states[0] == ReceiverState::free &&
          status.value().receiver_states[7] == ReceiverState::free);
    const auto initial_card_status = empty_request(*first, MessageType::CARD_STATUS);
    CHECK(initial_card_status);
    const auto card_status_payload = decode_card_status_response_payload(
        ByteView{initial_card_status.value().payload.data(),
                 initial_card_status.value().payload.size()});
    CHECK(card_status_payload && card_status_payload.value().present == 1U &&
          card_status_payload.value().initialized == 0U);

    const auto opened1 = request(*first, MessageType::CARD_CONNECT,
                                 CardConnectRequestPayload{ShareMode::shared});
    const auto opened2 = request(*second, MessageType::CARD_CONNECT,
                                 CardConnectRequestPayload{ShareMode::shared});
    CHECK(opened1 && opened2);
    const auto card1 = decode_card_connect_response_payload(
        ByteView{opened1.value().payload.data(), opened1.value().payload.size()});
    const auto card2 = decode_card_connect_response_payload(
        ByteView{opened2.value().payload.data(), opened2.value().payload.size()});
    CHECK(card1 && card2 && card1.value().card_handle != card2.value().card_handle);

    const auto reconnected = request(
        *second, MessageType::CARD_RECONNECT,
        CardReconnectRequestPayload{card2.value().card_handle,
                                    ShareMode::shared, Disposition::leave});
    CHECK(reconnected);
    const auto reconnect_atr = decode_atr_payload(
        ByteView{reconnected.value().payload.data(), reconnected.value().payload.size()});
    CHECK(reconnect_atr && reconnect_atr.value().atr.size == 2U);

    const auto reset = request(*first, MessageType::CARD_RESET,
                               CardHandleRequestPayload{card1.value().card_handle});
    CHECK(reset);
    const auto reset_atr = decode_atr_payload(
        ByteView{reset.value().payload.data(), reset.value().payload.size()});
    CHECK(reset_atr && reset_atr.value().atr.size == 2U);

    CHECK(request(*first, MessageType::BEGIN_TRANSACTION,
                  CardHandleRequestPayload{card1.value().card_handle}));
    const std::array<std::uint8_t, 2U> apdu{0x00U, 0x84U};
    const auto busy = request(
        *second, MessageType::CARD_TRANSMIT,
        CardTransmitRequestPayload{card2.value().card_handle,
                                   ByteView{apdu.data(), apdu.size()}});
    CHECK(!busy && busy.error() == Error::BUSY);
    CHECK(request(*first, MessageType::END_TRANSACTION,
                  CardDispositionRequestPayload{card1.value().card_handle,
                                                Disposition::leave}));

    const auto transmitted = request(
        *first, MessageType::CARD_TRANSMIT,
        CardTransmitRequestPayload{card1.value().card_handle,
                                   ByteView{apdu.data(), apdu.size()}});
    CHECK(transmitted);
    const auto response = decode_card_transmit_response_payload(
        ByteView{transmitted.value().payload.data(), transmitted.value().payload.size()});
    CHECK(response && response.value().response.size == 2U);

    CHECK(request(*second, MessageType::CARD_DISCONNECT,
                  CardDispositionRequestPayload{card2.value().card_handle,
                                                Disposition::leave}));
    CHECK(service.handle_count() == 1U && service.powered());

    backend.present = false;
    std::this_thread::sleep_for(std::chrono::milliseconds(300U));
    EventSink sink;
    const auto card_status = empty_request(*first, MessageType::CARD_STATUS, &sink);
    CHECK(card_status);
    CHECK(sink.count == 1U && sink.last.kind == DeviceEventKind::card_removed &&
          sink.last.target_type == EventTargetType::card);

    first->close();
    second->close();
    std::this_thread::sleep_for(std::chrono::milliseconds(100U));
    stop.store(true);
    server_thread.join();
    CHECK(server_ok.load());
    CHECK(service.handle_count() == 0U && !service.powered());
    CHECK(server->shutdown());
    CHECK(!std::filesystem::exists(socket_path));
    CHECK(!std::filesystem::exists(stream_socket_path));
    CHECK(!std::filesystem::exists(instance_path));
    CHECK(!std::filesystem::exists(product_path));
    return true;
}

bool test_no_hello_timeout_and_malformed_disconnect()
{
    TempRuntime runtime;
    CHECK(runtime.valid());
    constexpr const char* serial = "00001205000961";
    const EndpointConfig endpoint{runtime.path().c_str(), serial, kControlEndpointName,
                                  EndpointAccess::private_user};
    Backend backend;
    Session session;
    CardService service(backend, session);
    TunerBackend tuner_backend;
    TunerNonce tuner_nonce;
    TunerTime tuner_time;
    ControlStream stream_control;
    TunerService tuner_service(tuner_backend, tuner_nonce, tuner_time,
                               nullptr, nullptr, &stream_control);
    auto server_result = PosixControlServer::create(
        endpoint, service, tuner_service, serial);
    CHECK(server_result);
    std::unique_ptr<PosixControlServer> server = std::move(server_result.value());

    std::atomic<bool> stop{false};
    std::thread server_thread([&]() {
        while (!stop.load()) (void)server->poll_once(Timeout{10U});
    });
    auto raw = SocketStream::connect(endpoint, Timeout{1000U});
    CHECK(raw);
    std::array<std::uint8_t, 8U> buffer{};
    const auto timeout = raw.value().read_some(
        MutableByteView{buffer.data(), buffer.size()}, Timeout{20U});
    CHECK(!timeout && timeout.error() == Error::TIMEOUT);

    const std::array<std::uint8_t, kFrameHeaderSize> malformed{};
    const ssize_t sent = ::send(raw.value().native_handle(), malformed.data(),
                                malformed.size(), 0);
    CHECK(sent == static_cast<ssize_t>(malformed.size()));
    Result<std::size_t> disconnected = Result<std::size_t>::failure(Error::TIMEOUT);
    for (std::size_t attempt = 0U; attempt < 100U; ++attempt) {
        disconnected = raw.value().read_some(
            MutableByteView{buffer.data(), buffer.size()}, Timeout{20U});
        if (!disconnected && disconnected.error() == Error::DISCONNECTED) break;
    }
    CHECK(!disconnected && disconnected.error() == Error::DISCONNECTED);
    stop.store(true);
    server_thread.join();
    CHECK(server->shutdown());
    return true;
}

bool test_control_server_tuner_flow()
{
    TempRuntime runtime;
    CHECK(runtime.valid());
    constexpr const char* serial = "00001205000962";
    const EndpointConfig endpoint{runtime.path().c_str(), serial, kControlEndpointName,
                                  EndpointAccess::private_user};
    Backend card_backend;
    Session card_session;
    CardService card_service(card_backend, card_session);
    TunerBackend tuner_backend;
    TunerNonce tuner_nonce;
    TunerTime tuner_time;
    ControlStream stream_control;
    TunerService tuner_service(tuner_backend, tuner_nonce, tuner_time,
                               nullptr, nullptr, &stream_control);
    auto server_result = PosixControlServer::create(
        endpoint, card_service, tuner_service, serial);
    CHECK(server_result);
    std::unique_ptr<PosixControlServer> server = std::move(server_result.value());

    std::atomic<bool> stop{false};
    std::atomic<bool> server_ok{true};
    std::thread server_thread([&]() {
        while (!stop.load()) {
            const auto polled = server->poll_once(Timeout{20U});
            if (!polled) {
                server_ok.store(false);
                return;
            }
        }
    });

    auto first_result = PosixControlClient::connect(
        endpoint, kCapabilityEvents | kCapabilityStreamStats, Timeout{2000U});
    auto second_result = PosixControlClient::connect(
        endpoint, kCapabilityEvents | kCapabilityStreamStats, Timeout{2000U});
    CHECK(first_result && second_result);
    std::unique_ptr<PosixControlClient> first = std::move(first_result.value());
    std::unique_ptr<PosixControlClient> second = std::move(second_result.value());

    const auto terrestrial = request(*first, MessageType::ACQUIRE,
                                      AcquireRequestPayload{2U});
    CHECK(terrestrial);
    const auto terrestrial_lease = decode_acquire_response_payload(
        ByteView{terrestrial.value().payload.data(), terrestrial.value().payload.size()});
    CHECK(terrestrial_lease && terrestrial_lease.value().lease_id != 0U);
    const auto busy = request(*second, MessageType::ACQUIRE,
                               AcquireRequestPayload{2U});
    CHECK(!busy && busy.error() == Error::BUSY);
    const auto wrong_release = request(
        *second, MessageType::RELEASE,
        LeaseRequestPayload{terrestrial_lease.value().lease_id});
    CHECK(!wrong_release && wrong_release.error() == Error::NOT_FOUND);

    const auto satellite = request(*first, MessageType::ACQUIRE,
                                   AcquireRequestPayload{0U});
    CHECK(satellite);
    const auto satellite_lease = decode_acquire_response_payload(
        ByteView{satellite.value().payload.data(), satellite.value().payload.size()});
    CHECK(satellite_lease);

    const auto denied_lnb = request(
        *first, MessageType::TUNE,
        TuneRequestPayload{satellite_lease.value().lease_id, System::ISDB_S,
                           200000U, 0xffffU, 1U, 0U, 15U, 100U});
    CHECK(!denied_lnb && denied_lnb.error() == Error::UNSUPPORTED);
    CHECK(tuner_backend.lnb_gpio_calls.load() == 0U);

    const auto terrestrial_tune = request(
        *first, MessageType::TUNE,
        TuneRequestPayload{terrestrial_lease.value().lease_id, System::ISDB_T,
                           500000U, 0xffffU, 0xffffU, 6000000U, 0U, 100U});
    CHECK(terrestrial_tune);
    const auto terrestrial_response = decode_tune_response_payload(
        ByteView{terrestrial_tune.value().payload.data(),
                 terrestrial_tune.value().payload.size()});
    CHECK(terrestrial_response && terrestrial_response.value().locked == 1U);

    const auto satellite_tune = request(
        *first, MessageType::TUNE,
        TuneRequestPayload{satellite_lease.value().lease_id, System::ISDB_S,
                           200000U, 0xffffU, 1U, 0U, 0U, 100U});
    CHECK(satellite_tune);
    CHECK(tuner_backend.last_system[2U] == System::ISDB_T &&
          tuner_backend.last_system[0U] == System::ISDB_S);

    CHECK(request(*first, MessageType::START_STREAM,
                  LeaseRequestPayload{terrestrial_lease.value().lease_id}));
    const auto attachment = tuner_service.attach_stream(
        terrestrial_lease.value().lease_id, terrestrial_lease.value().nonce);
    CHECK(attachment);
    const auto live_stats = request(
        *first, MessageType::STATS,
        LeaseRequestPayload{terrestrial_lease.value().lease_id});
    CHECK(live_stats);
    const auto live_counters = decode_counters_payload(
        ByteView{live_stats.value().payload.data(), live_stats.value().payload.size()});
    CHECK(live_counters && live_counters.value().packets == 17U &&
          live_counters.value().queue_drops == 4U);
    const auto stopped_stream = request(
        *first, MessageType::STOP_STREAM,
        LeaseRequestPayload{terrestrial_lease.value().lease_id});
    CHECK(stopped_stream);
    const auto final_counters = decode_counters_payload(
        ByteView{stopped_stream.value().payload.data(),
                 stopped_stream.value().payload.size()});
    CHECK(final_counters && final_counters.value().bytes == 3196U &&
          final_counters.value().usb_errors == 5U);

    const auto status_response = empty_request(*first, MessageType::STATUS);
    CHECK(status_response);
    const auto status = decode_status_response_payload(
        ByteView{status_response.value().payload.data(), status_response.value().payload.size()});
    CHECK(status && status.value().receiver_states[0U] == ReceiverState::tuned &&
          status.value().receiver_states[2U] == ReceiverState::tuned &&
          status.value().generation >= 5U);
    const std::uint64_t generation_before_retune = status.value().generation;
    const auto retuned = request(
        *first, MessageType::TUNE,
        TuneRequestPayload{terrestrial_lease.value().lease_id, System::ISDB_T,
                           500000U, 0xffffU, 0xffffU, 6000000U, 0U, 100U});
    CHECK(retuned);
    const auto retune_status_response = empty_request(*first, MessageType::STATUS);
    CHECK(retune_status_response);
    const auto retune_status = decode_status_response_payload(
        ByteView{retune_status_response.value().payload.data(),
                 retune_status_response.value().payload.size()});
    // TunerService may pass through leased while a retune is in progress, but
    // that internal transition is not externally observable in this
    // synchronous server and must not create a generation-only jump.
    CHECK(retune_status && retune_status.value().generation == generation_before_retune);

    auto raw_result = SocketStream::connect(endpoint, Timeout{2000U});
    CHECK(raw_result);
    SocketStream raw = std::move(raw_result.value());
    std::array<std::uint8_t, kFrameHeaderSize + kMaxControlPayload> raw_storage{};
    StreamFramer raw_framer(MutableByteView{raw_storage.data(), raw_storage.size()});
    FrameCollector raw_frames;
    std::array<std::uint8_t, 12U> hello_payload{};
    const auto hello_size = encode_payload(
        HelloRequestPayload{kProtocolMajor, kProtocolMinor, kProtocolMajor,
                            kProtocolMinor, kCapabilityEvents},
        MutableByteView{hello_payload.data(), hello_payload.size()});
    CHECK(hello_size && send_raw_request(
        raw, MessageType::HELLO, 1U,
        ByteView{hello_payload.data(), hello_size.value()}));
    CHECK(read_raw_frames(raw, raw_framer, raw_frames, 1U) &&
          raw_frames.headers[0U].type == MessageType::HELLO &&
          raw_frames.headers[0U].kind == MessageKind::response);
    raw_frames.clear();
    std::array<std::uint8_t, 1U> acquire_payload{1U};
    // request_id zero is valid for an ordinary live request. It must not be
    // confused with the server's private cleanup tasks.
    CHECK(send_raw_request(raw, MessageType::ACQUIRE, 0U,
                           ByteView{acquire_payload.data(), acquire_payload.size()}));
    CHECK(read_raw_frames(raw, raw_framer, raw_frames, 2U) &&
          raw_frames.headers[0U].type == MessageType::ACQUIRE &&
          raw_frames.headers[0U].request_id == 0U &&
          raw_frames.headers[0U].kind == MessageKind::response &&
          raw_frames.headers[1U].kind == MessageKind::event);
    const auto raw_lease = decode_acquire_response_payload(
        ByteView{raw_frames.payloads[0U].data(), raw_frames.payload_sizes[0U]});
    CHECK(raw_lease);
    raw_frames.clear();
    std::array<std::uint8_t, 8U> release_payload{};
    const auto release_size = encode_payload(
        LeaseRequestPayload{raw_lease.value().lease_id},
        MutableByteView{release_payload.data(), release_payload.size()});
    CHECK(release_size && send_raw_request(
        raw, MessageType::RELEASE, 3U,
        ByteView{release_payload.data(), release_size.value()}));
    CHECK(read_raw_frames(raw, raw_framer, raw_frames, 2U) &&
          raw_frames.headers[0U].type == MessageType::RELEASE &&
          raw_frames.headers[0U].kind == MessageKind::response &&
          raw_frames.headers[0U].request_id == 3U &&
          raw_frames.headers[1U].type == MessageType::DEVICE_EVENT &&
          raw_frames.headers[1U].kind == MessageKind::event);
    const auto raw_event = decode_device_event_payload(
        ByteView{raw_frames.payloads[1U].data(), raw_frames.payload_sizes[1U]});
    CHECK(raw_event && raw_event.value().kind == DeviceEventKind::state_changed &&
          raw_event.value().target_type == EventTargetType::receiver &&
          raw_event.value().target_id == 1U);
    raw.close();

    std::array<std::uint8_t, kMaxControlPayload> invalid_tune{};
    const auto valid_tune_size = encode_payload(
        TuneRequestPayload{terrestrial_lease.value().lease_id, System::ISDB_T,
                           500000U, 0xffffU, 0xffffU, 6000000U, 0U, 100U},
        MutableByteView{invalid_tune.data(), invalid_tune.size()});
    CHECK(valid_tune_size && valid_tune_size.value() == 30U);
    invalid_tune[26U] = 99U;
    invalid_tune[27U] = 0U;
    invalid_tune[28U] = 0U;
    invalid_tune[29U] = 0U;
    const auto invalid_semantics = first->request(
        MessageType::TUNE, ByteView{invalid_tune.data(), valid_tune_size.value()},
        Timeout{2000U});
    CHECK(!invalid_semantics && invalid_semantics.error() == Error::INVALID_ARGUMENT);
    CHECK(empty_request(*first, MessageType::STATUS));

    const auto release_result = request(
        *first, MessageType::RELEASE,
        LeaseRequestPayload{terrestrial_lease.value().lease_id});
    CHECK(release_result);
    const auto release_satellite = request(
        *first, MessageType::RELEASE,
        LeaseRequestPayload{satellite_lease.value().lease_id});
    CHECK(release_satellite);

    first->close();
    std::this_thread::sleep_for(std::chrono::milliseconds(100U));
    const auto reacquired = request(*second, MessageType::ACQUIRE,
                                    AcquireRequestPayload{2U});
    CHECK(reacquired);
    const auto reacquired_lease = decode_acquire_response_payload(
        ByteView{reacquired.value().payload.data(), reacquired.value().payload.size()});
    CHECK(reacquired_lease);
    const auto reacquired_tune = request(
        *second, MessageType::TUNE,
        TuneRequestPayload{reacquired_lease.value().lease_id, System::ISDB_T,
                           500000U, 0xffffU, 0xffffU, 6000000U, 0U, 100U});
    CHECK(reacquired_tune);
    const auto start_stream = request(
        *second, MessageType::START_STREAM,
        LeaseRequestPayload{reacquired_lease.value().lease_id});
    CHECK(start_stream);
    const auto duplicate_start = request(
        *second, MessageType::START_STREAM,
        LeaseRequestPayload{reacquired_lease.value().lease_id});
    const auto stop_stream = request(
        *second, MessageType::STOP_STREAM,
        LeaseRequestPayload{reacquired_lease.value().lease_id});
    const auto stats = request(
        *second, MessageType::STATS,
        LeaseRequestPayload{reacquired_lease.value().lease_id});
    // START is a real worker-dispatched arm operation.  No ATTACH data
    // endpoint exists in 3E3a, so the still-armed lease cannot be stopped or
    // queried as an active stream yet.
    CHECK(!duplicate_start && duplicate_start.error() == Error::BUSY &&
          !stop_stream && stop_stream.error() == Error::NOT_READY &&
          !stats && stats.error() == Error::NOT_READY);

    stop.store(true);
    server_thread.join();
    CHECK(server_ok.load());
    const auto shutdown = server->shutdown();
    CHECK(shutdown);
    CHECK(tuner_backend.close_calls[2U] >= 2U && tuner_backend.close_calls[0U] >= 1U);
    return true;
}

bool test_control_server_lanes_are_independent()
{
    TempRuntime runtime;
    CHECK(runtime.valid());
    constexpr const char* serial = "00001205000963";
    const EndpointConfig endpoint{runtime.path().c_str(), serial, kControlEndpointName,
                                  EndpointAccess::private_user};
    Backend card_backend;
    Session card_session;
    CardService card_service(card_backend, card_session);
    TunerBackend tuner_backend;
    TunerNonce tuner_nonce;
    TunerTime tuner_time;
    TunerService tuner_service(tuner_backend, tuner_nonce, tuner_time);
    auto server_result = PosixControlServer::create(
        endpoint, card_service, tuner_service, serial);
    CHECK(server_result);
    std::unique_ptr<PosixControlServer> server = std::move(server_result.value());
    std::atomic<bool> stop{false};
    std::atomic<bool> server_ok{true};
    std::thread server_thread([&]() {
        while (!stop.load()) {
            const auto polled = server->poll_once(Timeout{20U});
            if (!polled) {
                server_ok.store(false);
                return;
            }
        }
    });

    auto slow_result = PosixControlClient::connect(endpoint, 0U, Timeout{2000U});
    CHECK(slow_result);
    std::unique_ptr<PosixControlClient> slow = std::move(slow_result.value());
    tuner_backend.block_receiver_zero.store(true);
    std::atomic<bool> slow_done{false};
    bool slow_ok = false;
    std::uint64_t slow_lease = 0U;
    std::thread slow_request([&]() {
        const auto acquired = request(*slow, MessageType::ACQUIRE,
                                      AcquireRequestPayload{0U});
        slow_ok = static_cast<bool>(acquired);
        if (acquired) {
            const auto payload = decode_acquire_response_payload(
                ByteView{acquired.value().payload.data(), acquired.value().payload.size()});
            if (payload) slow_lease = payload.value().lease_id;
        }
        slow_done.store(true);
    });
    BlockedOperationGuard blocked_acquire_guard(
        tuner_backend.block_receiver_zero, tuner_backend.receiver_zero_condition,
        slow_request, stop, server_thread, *server);
    bool started = false;
    {
        std::unique_lock<std::mutex> lock(tuner_backend.receiver_zero_mutex);
        started = tuner_backend.receiver_zero_condition.wait_for(
            lock, std::chrono::seconds(10),
            [&]() { return tuner_backend.receiver_zero_started.load(); });
    }
    if (!started) {
        return false;
    }

    auto fast_result = PosixControlClient::connect(endpoint, kCapabilityCard, Timeout{2000U});
    CHECK(fast_result);
    std::unique_ptr<PosixControlClient> fast = std::move(fast_result.value());
    CHECK(empty_request(*fast, MessageType::LIST));
    const auto dev2 = request(*fast, MessageType::ACQUIRE,
                              AcquireRequestPayload{4U});
    CHECK(dev2);
    const auto dev2_payload = decode_acquire_response_payload(
        ByteView{dev2.value().payload.data(), dev2.value().payload.size()});
    CHECK(dev2_payload && request(*fast, MessageType::RELEASE,
                                  LeaseRequestPayload{dev2_payload.value().lease_id}));
    const auto card = request(*fast, MessageType::CARD_CONNECT,
                              CardConnectRequestPayload{ShareMode::shared});
    CHECK(card);
    const auto card_payload = decode_card_connect_response_payload(
        ByteView{card.value().payload.data(), card.value().payload.size()});
    CHECK(card_payload && request(
        *fast, MessageType::CARD_DISCONNECT,
        CardDispositionRequestPayload{card_payload.value().card_handle,
                                      Disposition::leave}));

    tuner_backend.block_receiver_zero.store(false);
    slow_request.join();
    CHECK(slow_done.load() && slow_ok && slow_lease != 0U);
    CHECK(request(*slow, MessageType::RELEASE, LeaseRequestPayload{slow_lease}));
    slow->close();
    fast->close();
    stop.store(true);
    server_thread.join();
    CHECK(server_ok.load());
    CHECK(server->shutdown());
    blocked_acquire_guard.dismiss();
    return true;
}

bool test_control_server_disconnect_during_blocked_acquire()
{
    TempRuntime runtime;
    CHECK(runtime.valid());
    constexpr const char* serial = "00001205000964";
    const EndpointConfig endpoint{runtime.path().c_str(), serial, kControlEndpointName,
                                  EndpointAccess::private_user};
    Backend card_backend;
    Session card_session;
    CardService card_service(card_backend, card_session);
    TunerBackend tuner_backend;
    TunerNonce tuner_nonce;
    TunerTime tuner_time;
    TunerService tuner_service(tuner_backend, tuner_nonce, tuner_time);
    auto server_result = PosixControlServer::create(
        endpoint, card_service, tuner_service, serial);
    CHECK(server_result);
    std::unique_ptr<PosixControlServer> server = std::move(server_result.value());
    std::atomic<bool> stop{false};
    std::atomic<bool> server_ok{true};
    std::thread server_thread([&]() {
        while (!stop.load()) {
            const auto polled = server->poll_once(Timeout{20U});
            if (!polled) {
                server_ok.store(false);
                return;
            }
        }
    });
    ServerThreadGuard server_guard(stop, server_thread, *server);

    auto raw_result = SocketStream::connect(endpoint, Timeout{2000U});
    CHECK(raw_result);
    SocketStream raw = std::move(raw_result.value());
    std::array<std::uint8_t, kFrameHeaderSize + kMaxControlPayload> raw_storage{};
    StreamFramer raw_framer(MutableByteView{raw_storage.data(), raw_storage.size()});
    CHECK(raw_hello(raw, raw_framer));

    tuner_backend.block_receiver_zero.store(true);
    std::atomic<bool> request_done{false};
    std::atomic<bool> response_seen{false};
    std::thread request_thread([&]() {
        const std::array<std::uint8_t, 1U> payload{0U};
        const auto sent = send_raw_request(
            raw, MessageType::ACQUIRE, 2U, ByteView{payload.data(), payload.size()});
        if (sent) {
            FrameCollector frames;
            if (read_raw_frames(raw, raw_framer, frames, 1U)) response_seen.store(true);
        }
        request_done.store(true);
    });
    BlockedOperationGuard blocked_guard(
        tuner_backend.block_receiver_zero, tuner_backend.receiver_zero_condition,
        request_thread, stop, server_thread, *server);
    {
        std::unique_lock<std::mutex> lock(tuner_backend.receiver_zero_mutex);
        CHECK(tuner_backend.receiver_zero_condition.wait_for(
            lock, std::chrono::seconds(10),
            [&]() { return tuner_backend.receiver_zero_started.load(); }));
    }

    // shutdown() is an OS-level operation on the borrowed descriptor; the
    // SocketStream object remains owned by request_thread, avoiding a C++
    // data race while making the disconnect happen before unblocking open.
    CHECK(::shutdown(raw.native_handle(), SHUT_RDWR) == 0);
    blocked_guard.unblock_and_join();
    CHECK(request_done.load() && !response_seen.load());

    auto replacement_result = PosixControlClient::connect(
        endpoint, 0U, Timeout{2000U});
    CHECK(replacement_result);
    std::unique_ptr<PosixControlClient> replacement =
        std::move(replacement_result.value());
    Result<ControlResponse> acquired = Result<ControlResponse>::failure(Error::BUSY);
    for (std::size_t attempt = 0U; attempt < 500U; ++attempt) {
        acquired = request(*replacement, MessageType::ACQUIRE,
                           AcquireRequestPayload{0U});
        if (acquired) break;
        CHECK(acquired.error() == Error::BUSY);
        std::this_thread::sleep_for(std::chrono::milliseconds(10U));
    }
    CHECK(acquired);
    const auto lease = decode_acquire_response_payload(
        ByteView{acquired.value().payload.data(), acquired.value().payload.size()});
    CHECK(lease && request(*replacement, MessageType::RELEASE,
                           LeaseRequestPayload{lease.value().lease_id}));
    replacement->close();
    raw.close();
    stop.store(true);
    server_thread.join();
    CHECK(server_ok.load());
    CHECK(server->shutdown());
    blocked_guard.dismiss();
    server_guard.dismiss();
    return true;
}

bool test_control_server_disconnect_during_blocked_tune()
{
    TempRuntime runtime;
    CHECK(runtime.valid());
    constexpr const char* serial = "00001205000965";
    const EndpointConfig endpoint{runtime.path().c_str(), serial, kControlEndpointName,
                                  EndpointAccess::private_user};
    Backend card_backend;
    Session card_session;
    CardService card_service(card_backend, card_session);
    TunerBackend tuner_backend;
    TunerNonce tuner_nonce;
    TunerTime tuner_time;
    TunerService tuner_service(tuner_backend, tuner_nonce, tuner_time);
    auto server_result = PosixControlServer::create(
        endpoint, card_service, tuner_service, serial);
    CHECK(server_result);
    std::unique_ptr<PosixControlServer> server = std::move(server_result.value());
    std::atomic<bool> stop{false};
    std::atomic<bool> server_ok{true};
    std::thread server_thread([&]() {
        while (!stop.load()) {
            const auto polled = server->poll_once(Timeout{20U});
            if (!polled) {
                server_ok.store(false);
                return;
            }
        }
    });
    ServerThreadGuard server_guard(stop, server_thread, *server);

    auto raw_result = SocketStream::connect(endpoint, Timeout{2000U});
    CHECK(raw_result);
    SocketStream raw = std::move(raw_result.value());
    std::array<std::uint8_t, kFrameHeaderSize + kMaxControlPayload> raw_storage{};
    StreamFramer raw_framer(MutableByteView{raw_storage.data(), raw_storage.size()});
    CHECK(raw_hello(raw, raw_framer));
    const std::array<std::uint8_t, 1U> acquire_payload{0U};
    CHECK(send_raw_request(raw, MessageType::ACQUIRE, 2U,
                           ByteView{acquire_payload.data(), acquire_payload.size()}));
    FrameCollector acquire_frames;
    CHECK(read_raw_frames(raw, raw_framer, acquire_frames, 1U));
    const auto lease = decode_acquire_response_payload(
        ByteView{acquire_frames.payloads[0U].data(), acquire_frames.payload_sizes[0U]});
    CHECK(lease);

    tuner_backend.block_tune_receiver_zero.store(true);
    std::atomic<bool> request_done{false};
    std::atomic<bool> response_seen{false};
    std::thread request_thread([&]() {
        std::array<std::uint8_t, kMaxControlPayload> payload{};
        const auto payload_size = encode_payload(
            TuneRequestPayload{lease.value().lease_id, System::ISDB_S, 200000U,
                               0xffffU, 1U, 0U, 0U, 100U},
            MutableByteView{payload.data(), payload.size()});
        const auto sent = payload_size && send_raw_request(
            raw, MessageType::TUNE, 3U,
            ByteView{payload.data(), payload_size.value()});
        if (sent) {
            FrameCollector frames;
            if (read_raw_frames(raw, raw_framer, frames, 1U)) response_seen.store(true);
        }
        request_done.store(true);
    });
    BlockedOperationGuard blocked_guard(
        tuner_backend.block_tune_receiver_zero,
        tuner_backend.tune_receiver_zero_condition, request_thread, stop,
        server_thread, *server);
    {
        std::unique_lock<std::mutex> lock(tuner_backend.tune_receiver_zero_mutex);
        CHECK(tuner_backend.tune_receiver_zero_condition.wait_for(
            lock, std::chrono::seconds(10),
            [&]() { return tuner_backend.tune_receiver_zero_started.load(); }));
    }
    CHECK(::shutdown(raw.native_handle(), SHUT_RDWR) == 0);
    blocked_guard.unblock_and_join();
    CHECK(request_done.load() && !response_seen.load());

    auto replacement_result = PosixControlClient::connect(
        endpoint, 0U, Timeout{2000U});
    CHECK(replacement_result);
    std::unique_ptr<PosixControlClient> replacement =
        std::move(replacement_result.value());
    Result<ControlResponse> acquired = Result<ControlResponse>::failure(Error::BUSY);
    for (std::size_t attempt = 0U; attempt < 500U; ++attempt) {
        acquired = request(*replacement, MessageType::ACQUIRE,
                           AcquireRequestPayload{0U});
        if (acquired) break;
        CHECK(acquired.error() == Error::BUSY);
        std::this_thread::sleep_for(std::chrono::milliseconds(10U));
    }
    CHECK(acquired);
    const auto replacement_lease = decode_acquire_response_payload(
        ByteView{acquired.value().payload.data(), acquired.value().payload.size()});
    CHECK(replacement_lease &&
          replacement_lease.value().lease_id != lease.value().lease_id);
    CHECK(request(*replacement, MessageType::RELEASE,
                  LeaseRequestPayload{replacement_lease.value().lease_id}));
    replacement->close();
    raw.close();
    stop.store(true);
    server_thread.join();
    CHECK(server_ok.load());
    CHECK(server->shutdown());
    blocked_guard.dismiss();
    server_guard.dismiss();
    return true;
}

bool test_stream_endpoint_attach_and_stream_end()
{
    TempRuntime runtime;
    CHECK(runtime.valid());
    constexpr const char* serial = "00001205000964";
    const EndpointConfig endpoint{runtime.path().c_str(), serial, kControlEndpointName,
                                  EndpointAccess::private_user};
    Backend card_backend;
    Session card_session;
    CardService card_service(card_backend, card_session);
    TunerBackend tuner_backend;
    TunerNonce tuner_nonce;
    TunerTime tuner_time;
    ControlStream stream_control;
    TunerService tuner_service(tuner_backend, tuner_nonce, tuner_time,
                               nullptr, nullptr, &stream_control);
    auto server_result = PosixControlServer::create(
        endpoint, card_service, tuner_service, serial, true, 0x03U,
        &stream_control);
    CHECK(server_result);
    std::unique_ptr<PosixControlServer> server = std::move(server_result.value());
    const std::string stream_path = server->stream_endpoint_path();
    struct stat status {};
    CHECK(::stat(stream_path.c_str(), &status) == 0 &&
          (status.st_mode & 0777U) == 0600U);

    std::atomic<bool> stop{false};
    std::atomic<bool> server_ok{true};
    std::thread server_thread([&]() {
        while (!stop.load()) {
            const auto polled = server->poll_once(Timeout{10U});
            if (!polled) {
                server_ok.store(false);
                return;
            }
        }
    });
    ServerThreadGuard server_guard(stop, server_thread, *server);

    auto control_result = PosixControlClient::connect(
        endpoint, kCapabilityStreamStats, Timeout{2000U});
    CHECK(control_result);
    std::unique_ptr<PosixControlClient> control = std::move(control_result.value());
    const auto acquired = request(*control, MessageType::ACQUIRE,
                                  AcquireRequestPayload{2U});
    CHECK(acquired);
    const auto lease = decode_acquire_response_payload(
        ByteView{acquired.value().payload.data(), acquired.value().payload.size()});
    CHECK(lease);
    CHECK(request(*control, MessageType::TUNE,
                  TuneRequestPayload{lease.value().lease_id, System::ISDB_T,
                                     500000U, 0xffffU, 0xffffU, 6000000U, 0U,
                                     100U}));
    CHECK(request(*control, MessageType::START_STREAM,
                  LeaseRequestPayload{lease.value().lease_id}));

    const EndpointConfig stream_endpoint{runtime.path().c_str(), serial,
                                         kStreamEndpointName,
                                         EndpointAccess::private_user};
    auto stream_result = SocketStream::connect(stream_endpoint, Timeout{2000U});
    CHECK(stream_result);
    SocketStream stream = std::move(stream_result.value());
    std::array<std::uint8_t, kFrameHeaderSize + kMaxControlPayload> storage{};
    StreamFramer framer(MutableByteView{storage.data(), storage.size()});
    std::array<std::uint8_t, kMaxControlPayload> attach_payload{};
    {
        std::lock_guard<std::mutex> lock(stream_control.mutex);
        stream_control.data_packet.fill(0xa5U);
        stream_control.data_packet[0U] = 0x47U;
        stream_control.emit_data = true;
    }
    const auto attach_size = encode_payload(
        AttachStreamRequestPayload{lease.value().lease_id, lease.value().nonce},
        MutableByteView{attach_payload.data(), attach_payload.size()});
    CHECK(attach_size && send_raw_request(
        stream, MessageType::ATTACH_STREAM, 0U,
        ByteView{attach_payload.data(), attach_size.value()}));
    FrameCollector frames;
    CHECK(read_raw_frames(stream, framer, frames, 1U));
    std::size_t attach_index = frames.count;
    std::size_t data_index = frames.count;
    for (std::size_t index = 0U; index < frames.count; ++index) {
        if (frames.headers[index].type == MessageType::ATTACH_STREAM &&
            frames.headers[index].kind == MessageKind::response &&
            frames.headers[index].request_id == 0U) {
            attach_index = index;
        }
        if (frames.headers[index].type == MessageType::TS_DATA &&
            frames.headers[index].kind == MessageKind::event) {
            data_index = index;
        }
    }
    CHECK(attach_index < frames.count);

    auto wrong_result = SocketStream::connect(stream_endpoint, Timeout{2000U});
    CHECK(wrong_result);
    SocketStream wrong = std::move(wrong_result.value());
    std::array<std::uint8_t, kFrameHeaderSize + kMaxControlPayload> wrong_storage{};
    StreamFramer wrong_framer(MutableByteView{wrong_storage.data(), wrong_storage.size()});
    auto wrong_payload = AttachStreamRequestPayload{
        lease.value().lease_id, lease.value().nonce};
    wrong_payload.nonce[0U] ^= 0xffU;
    std::array<std::uint8_t, kMaxControlPayload> wrong_encoded{};
    const auto wrong_size = encode_payload(
        wrong_payload, MutableByteView{wrong_encoded.data(), wrong_encoded.size()});
    CHECK(wrong_size && send_raw_request(
        wrong, MessageType::ATTACH_STREAM, 7U,
        ByteView{wrong_encoded.data(), wrong_size.value()}));
    FrameCollector wrong_frames;
    CHECK(read_raw_frames(wrong, wrong_framer, wrong_frames, 1U));
    const auto wrong_error = decode_error_response_payload(
        ByteView{wrong_frames.payloads[0U].data(), wrong_frames.payload_sizes[0U]});
    CHECK(wrong_frames.headers[0U].kind == MessageKind::error_response &&
          wrong_error && wrong_error.value().error_code == ErrorCode::NOT_FOUND);
    wrong.close();

    // The acquire nonce is one-use: a second data connection presenting the
    // same lease/nonce must not attach the already-active session.
    auto reused_result = SocketStream::connect(stream_endpoint, Timeout{2000U});
    CHECK(reused_result);
    SocketStream reused = std::move(reused_result.value());
    std::array<std::uint8_t, kFrameHeaderSize + kMaxControlPayload> reused_storage{};
    StreamFramer reused_framer(
        MutableByteView{reused_storage.data(), reused_storage.size()});
    CHECK(send_raw_request(reused, MessageType::ATTACH_STREAM, 8U,
                           ByteView{attach_payload.data(), attach_size.value()}));
    FrameCollector reused_frames;
    CHECK(read_raw_frames(reused, reused_framer, reused_frames, 1U));
    const auto reused_error = decode_error_response_payload(
        ByteView{reused_frames.payloads[0U].data(),
                 reused_frames.payload_sizes[0U]});
    CHECK(reused_frames.headers[0U].kind == MessageKind::error_response &&
          reused_error && reused_error.value().error_code == ErrorCode::NOT_FOUND);
    reused.close();

    if (data_index == frames.count) {
        frames.clear();
        CHECK(read_raw_frames(stream, framer, frames, 1U));
        data_index = 0U;
    }
    CHECK(data_index < frames.count &&
          frames.headers[data_index].type == MessageType::TS_DATA &&
          frames.headers[data_index].kind == MessageKind::event);
    const auto data = decode_ts_data_event_payload(
        ByteView{frames.payloads[data_index].data(), frames.payload_sizes[data_index]});
    CHECK(data && data.value().sequence == 0U &&
          data.value().cumulative_drop_count == 0U &&
          data.value().bytes.size == stream_control.data_packet.size() &&
          data.value().bytes.data[0U] == 0x47U &&
          data.value().bytes.data[187U] == 0xa5U);

    const auto stopped = request(*control, MessageType::STOP_STREAM,
                                 LeaseRequestPayload{lease.value().lease_id});
    CHECK(stopped);
    frames.clear();
    CHECK(read_raw_frames(stream, framer, frames, 1U) &&
          frames.headers[0U].type == MessageType::STREAM_END &&
          frames.headers[0U].kind == MessageKind::event);
    const auto end = decode_stream_end_event_payload(
        ByteView{frames.payloads[0U].data(), frames.payload_sizes[0U]});
    CHECK(end && end.value().error_code == ErrorCode::OK);
    CHECK(request(*control, MessageType::RELEASE,
                  LeaseRequestPayload{lease.value().lease_id}));

    stream.close();
    control->close();
    stop.store(true);
    server_thread.join();
    CHECK(server_ok.load());
    CHECK(server->shutdown());
    server_guard.dismiss();
    return true;
}

bool test_stream_endpoint_terminal_mapping()
{
    const std::array<std::pair<TunerStreamTerminal, ErrorCode>, 6U> cases{{
        {TunerStreamTerminal::stopped, ErrorCode::OK},
        {TunerStreamTerminal::slow_consumer, ErrorCode::SLOW_CONSUMER},
        {TunerStreamTerminal::usb_error, ErrorCode::USB_IO},
        {TunerStreamTerminal::disconnected, ErrorCode::DISCONNECTED},
        {TunerStreamTerminal::sync_error, ErrorCode::PROTOCOL_ERROR},
        {TunerStreamTerminal::bridge_fatal, ErrorCode::INTERNAL},
    }};

    for (const auto& terminal_case : cases) {
        TempRuntime runtime;
        CHECK(runtime.valid());
        constexpr const char* serial = "00001205000965";
        const EndpointConfig endpoint{runtime.path().c_str(), serial,
                                      kControlEndpointName,
                                      EndpointAccess::private_user};
        Backend card_backend;
        Session card_session;
        CardService card_service(card_backend, card_session);
        TunerBackend tuner_backend;
        TunerNonce tuner_nonce;
        TunerTime tuner_time;
        ControlStream stream_control;
        TunerService tuner_service(tuner_backend, tuner_nonce, tuner_time,
                                   nullptr, nullptr, &stream_control);
        auto server_result = PosixControlServer::create(
            endpoint, card_service, tuner_service, serial, true, 0x03U,
            &stream_control);
        CHECK(server_result);
        std::unique_ptr<PosixControlServer> server = std::move(server_result.value());
        std::atomic<bool> stop{false};
        std::atomic<bool> server_ok{true};
        std::thread server_thread([&]() {
            while (!stop.load()) {
                const auto polled = server->poll_once(Timeout{10U});
                if (!polled) {
                    server_ok.store(false);
                    return;
                }
            }
        });
        ServerThreadGuard server_guard(stop, server_thread, *server);

        auto control_result = PosixControlClient::connect(
            endpoint, kCapabilityStreamStats, Timeout{2000U});
        CHECK(control_result);
        std::unique_ptr<PosixControlClient> control = std::move(control_result.value());
        const auto acquired = request(*control, MessageType::ACQUIRE,
                                      AcquireRequestPayload{2U});
        CHECK(acquired);
        const auto lease = decode_acquire_response_payload(
            ByteView{acquired.value().payload.data(), acquired.value().payload.size()});
        CHECK(lease);
        CHECK(request(*control, MessageType::TUNE,
                      TuneRequestPayload{lease.value().lease_id, System::ISDB_T,
                                         500000U, 0xffffU, 0xffffU, 6000000U, 0U,
                                         100U}));
        CHECK(request(*control, MessageType::START_STREAM,
                      LeaseRequestPayload{lease.value().lease_id}));

        const EndpointConfig stream_endpoint{runtime.path().c_str(), serial,
                                             kStreamEndpointName,
                                             EndpointAccess::private_user};
        auto stream_result = SocketStream::connect(stream_endpoint, Timeout{2000U});
        CHECK(stream_result);
        SocketStream stream = std::move(stream_result.value());
        std::array<std::uint8_t, kFrameHeaderSize + kMaxControlPayload> storage{};
        StreamFramer framer(MutableByteView{storage.data(), storage.size()});
        std::array<std::uint8_t, kMaxControlPayload> payload{};
        const auto payload_size = encode_payload(
            AttachStreamRequestPayload{lease.value().lease_id, lease.value().nonce},
            MutableByteView{payload.data(), payload.size()});
        CHECK(payload_size && send_raw_request(
            stream, MessageType::ATTACH_STREAM, 0U,
            ByteView{payload.data(), payload_size.value()}));
        FrameCollector frames;
        CHECK(read_raw_frames(stream, framer, frames, 1U));
        CHECK(frames.headers[0U].type == MessageType::ATTACH_STREAM &&
              frames.headers[0U].kind == MessageKind::response);
        {
            std::lock_guard<std::mutex> lock(stream_control.mutex);
            stream_control.counters = TunerStreamCounters{31U, 5828U, 7U, 11U,
                                                            13U, 17U, 19U, 23U};
            stream_control.final_terminal = terminal_case.first;
        }
        const auto stopped = request(*control, MessageType::STOP_STREAM,
                                     LeaseRequestPayload{lease.value().lease_id});
        CHECK(stopped);
        frames.clear();
        CHECK(read_raw_frames(stream, framer, frames, 1U));
        CHECK(frames.headers[0U].type == MessageType::STREAM_END &&
              frames.headers[0U].kind == MessageKind::event);
        const auto end = decode_stream_end_event_payload(
            ByteView{frames.payloads[0U].data(), frames.payload_sizes[0U]});
        CHECK(end && end.value().error_code == terminal_case.second &&
              end.value().counters.packets == 31U &&
              end.value().counters.bytes == 5828U &&
              end.value().counters.sync_errors == 7U &&
              end.value().counters.tei_packets == 11U &&
              end.value().counters.continuity_errors == 13U &&
              end.value().counters.queue_drops == 17U &&
              end.value().counters.usb_errors == 19U &&
              end.value().counters.empty_intervals == 23U);
        const auto released = request(
            *control, MessageType::RELEASE,
            LeaseRequestPayload{lease.value().lease_id});
        if (terminal_case.first == TunerStreamTerminal::disconnected) {
            // The exact terminal snapshot is transport-loss evidence. Release
            // performs no later receiver write and reports that cleanup could
            // not prove a physical close.
            CHECK(!released && released.error() == Error::DISCONNECTED);
        } else {
            CHECK(released);
        }
        stream.close();
        control->close();
        stop.store(true);
        server_thread.join();
        CHECK(server_ok.load());
        CHECK(server->shutdown());
        server_guard.dismiss();
    }
    return true;
}

bool test_stream_attach_submit_failure_cleanup()
{
    TempRuntime runtime;
    CHECK(runtime.valid());
    constexpr const char* serial = "00001205000966";
    const EndpointConfig endpoint{runtime.path().c_str(), serial,
                                  kControlEndpointName,
                                  EndpointAccess::private_user};
    Backend card_backend;
    Session card_session;
    CardService card_service(card_backend, card_session);
    TunerBackend tuner_backend;
    TunerNonce tuner_nonce;
    TunerTime tuner_time;
    ControlStream stream_control;
    TunerService tuner_service(tuner_backend, tuner_nonce, tuner_time,
                               nullptr, nullptr, &stream_control);
    ControlWorkerStartupOptions worker_options{};
    worker_options.fail_tuner_attach_submit = true;
    ControlWorkerLanes::set_test_startup_options(&worker_options);
    auto server_result = PosixControlServer::create(
        endpoint, card_service, tuner_service, serial, true, 0x03U,
        &stream_control);
    CHECK(server_result);
    std::unique_ptr<PosixControlServer> server = std::move(server_result.value());
    std::atomic<bool> stop{false};
    std::atomic<bool> server_ok{true};
    std::thread server_thread([&]() {
        while (!stop.load()) {
            const auto polled = server->poll_once(Timeout{10U});
            if (!polled) {
                server_ok.store(false);
                return;
            }
        }
    });
    ServerThreadGuard server_guard(stop, server_thread, *server);

    auto control_result = PosixControlClient::connect(
        endpoint, kCapabilityStreamStats, Timeout{2000U});
    CHECK(control_result);
    std::unique_ptr<PosixControlClient> control = std::move(control_result.value());
    const auto acquired = request(*control, MessageType::ACQUIRE,
                                  AcquireRequestPayload{2U});
    CHECK(acquired);
    const auto lease = decode_acquire_response_payload(
        ByteView{acquired.value().payload.data(), acquired.value().payload.size()});
    CHECK(lease);
    CHECK(request(*control, MessageType::TUNE,
                  TuneRequestPayload{lease.value().lease_id, System::ISDB_T,
                                     500000U, 0xffffU, 0xffffU, 6000000U, 0U,
                                     100U}));
    CHECK(request(*control, MessageType::START_STREAM,
                  LeaseRequestPayload{lease.value().lease_id}));

    const EndpointConfig stream_endpoint{runtime.path().c_str(), serial,
                                         kStreamEndpointName,
                                         EndpointAccess::private_user};
    auto stream_result = SocketStream::connect(stream_endpoint, Timeout{2000U});
    CHECK(stream_result);
    SocketStream stream = std::move(stream_result.value());
    std::array<std::uint8_t, kFrameHeaderSize + kMaxControlPayload> storage{};
    StreamFramer framer(MutableByteView{storage.data(), storage.size()});
    std::array<std::uint8_t, kMaxControlPayload> payload{};
    const auto payload_size = encode_payload(
        AttachStreamRequestPayload{lease.value().lease_id, lease.value().nonce},
        MutableByteView{payload.data(), payload.size()});
    CHECK(payload_size && send_raw_request(
        stream, MessageType::ATTACH_STREAM, 0U,
        ByteView{payload.data(), payload_size.value()}));
    FrameCollector frames;
    CHECK(read_raw_frames(stream, framer, frames, 1U));
    const auto error = decode_error_response_payload(
        ByteView{frames.payloads[0U].data(), frames.payload_sizes[0U]});
    CHECK(frames.headers[0U].kind == MessageKind::error_response && error &&
          error.value().error_code == ErrorCode::BUSY);
    std::array<std::uint8_t, 1U> closed_buffer{};
    const auto closed = stream.read_some(
        MutableByteView{closed_buffer.data(), closed_buffer.size()}, Timeout{2000U});
    CHECK(!closed && closed.error() == Error::DISCONNECTED);
    CHECK(request(*control, MessageType::RELEASE,
                  LeaseRequestPayload{lease.value().lease_id}));
    stream.close();
    control->close();
    stop.store(true);
    server_thread.join();
    CHECK(server_ok.load());
    CHECK(server->shutdown());
    server_guard.dismiss();
    return true;
}

bool test_stream_hup_before_attach_completion_cleanup()
{
    TempRuntime runtime;
    CHECK(runtime.valid());
    constexpr const char* serial = "00001205000967";
    const EndpointConfig endpoint{runtime.path().c_str(), serial,
                                  kControlEndpointName,
                                  EndpointAccess::private_user};
    Backend card_backend;
    Session card_session;
    CardService card_service(card_backend, card_session);
    TunerBackend tuner_backend;
    TunerNonce tuner_nonce;
    TunerTime tuner_time;
    ControlStream stream_control;
    TunerService tuner_service(tuner_backend, tuner_nonce, tuner_time,
                               nullptr, nullptr, &stream_control);
    AttachCompletionBarrier completion_barrier;
    ControlWorkerStartupOptions worker_options{};
    worker_options.before_tuner_attach_completion =
        &AttachCompletionBarrier::before_completion;
    worker_options.before_tuner_attach_completion_context = &completion_barrier;
    ControlWorkerLanes::set_test_startup_options(&worker_options);
    auto server_result = PosixControlServer::create(
        endpoint, card_service, tuner_service, serial, true, 0x03U,
        &stream_control);
    CHECK(server_result);
    std::unique_ptr<PosixControlServer> server = std::move(server_result.value());
    std::atomic<bool> stop{false};
    std::atomic<bool> server_ok{true};
    std::thread server_thread([&]() {
        while (!stop.load()) {
            const auto polled = server->poll_once(Timeout{10U});
            if (!polled) {
                server_ok.store(false);
                return;
            }
        }
    });
    ServerThreadGuard server_guard(stop, server_thread, *server);
    AttachCompletionReleaseGuard completion_guard(completion_barrier);

    auto control_result = PosixControlClient::connect(
        endpoint, kCapabilityStreamStats, Timeout{2000U});
    CHECK(control_result);
    std::unique_ptr<PosixControlClient> control = std::move(control_result.value());
    const auto acquired = request(*control, MessageType::ACQUIRE,
                                  AcquireRequestPayload{2U});
    CHECK(acquired);
    const auto lease = decode_acquire_response_payload(
        ByteView{acquired.value().payload.data(), acquired.value().payload.size()});
    CHECK(lease);
    CHECK(request(*control, MessageType::TUNE,
                  TuneRequestPayload{lease.value().lease_id, System::ISDB_T,
                                     500000U, 0xffffU, 0xffffU, 6000000U, 0U,
                                     100U}));
    CHECK(request(*control, MessageType::START_STREAM,
                  LeaseRequestPayload{lease.value().lease_id}));

    const EndpointConfig stream_endpoint{runtime.path().c_str(), serial,
                                         kStreamEndpointName,
                                         EndpointAccess::private_user};
    auto stream_result = SocketStream::connect(stream_endpoint, Timeout{2000U});
    CHECK(stream_result);
    SocketStream stream = std::move(stream_result.value());
    std::array<std::uint8_t, kFrameHeaderSize + kMaxControlPayload> storage{};
    StreamFramer framer(MutableByteView{storage.data(), storage.size()});
    std::array<std::uint8_t, kMaxControlPayload> payload{};
    const auto payload_size = encode_payload(
        AttachStreamRequestPayload{lease.value().lease_id, lease.value().nonce},
        MutableByteView{payload.data(), payload.size()});
    CHECK(payload_size && send_raw_request(
        stream, MessageType::ATTACH_STREAM, 0U,
        ByteView{payload.data(), payload_size.value()}));
    CHECK(completion_barrier.wait_until_entered());

    // The worker has already completed TunerService::attach_stream, but its
    // completion is held before publication.  HUP therefore exercises the
    // retired waiting_attach path rather than the ordinary live path.
    stream.close();
    completion_barrier.release();
    CHECK(stream_control.wait_for_detaches(1U));
    CHECK(request(*control, MessageType::RELEASE,
                  LeaseRequestPayload{lease.value().lease_id}));
    CHECK(tuner_backend.start_capture_calls[2U] == 1U &&
          tuner_backend.stop_capture_calls[2U] == 1U &&
          tuner_backend.close_calls[2U] == 1U &&
          stream_control.detach_calls == 1U);
    control->close();
    stop.store(true);
    server_thread.join();
    CHECK(server_ok.load());
    CHECK(server->shutdown());
    completion_guard.dismiss();
    server_guard.dismiss();
    return true;
}

bool test_stream_endpoint_fragmented_and_duplicate()
{
    TempRuntime runtime;
    CHECK(runtime.valid());
    constexpr const char* serial = "00001205000968";
    const EndpointConfig endpoint{runtime.path().c_str(), serial,
                                  kControlEndpointName,
                                  EndpointAccess::private_user};
    Backend card_backend;
    Session card_session;
    CardService card_service(card_backend, card_session);
    TunerBackend tuner_backend;
    TunerNonce tuner_nonce;
    TunerTime tuner_time;
    ControlStream stream_control;
    TunerService tuner_service(tuner_backend, tuner_nonce, tuner_time,
                               nullptr, nullptr, &stream_control);
    StreamServerObservation observation;
    StreamServerHookGuard hook_guard(observation);
    auto server_result = PosixControlServer::create(
        endpoint, card_service, tuner_service, serial, true, 0x03U,
        &stream_control);
    CHECK(server_result);
    std::unique_ptr<PosixControlServer> server = std::move(server_result.value());
    std::atomic<bool> stop{false};
    std::atomic<bool> server_ok{true};
    std::thread server_thread([&]() {
        while (!stop.load()) {
            const auto polled = server->poll_once(Timeout{10U});
            if (!polled) {
                server_ok.store(false);
                return;
            }
        }
    });
    ServerThreadGuard server_guard(stop, server_thread, *server);

    auto control_result = PosixControlClient::connect(
        endpoint, kCapabilityStreamStats, Timeout{2000U});
    CHECK(control_result);
    std::unique_ptr<PosixControlClient> control = std::move(control_result.value());
    const auto acquired = request(*control, MessageType::ACQUIRE,
                                  AcquireRequestPayload{2U});
    CHECK(acquired);
    const auto lease = decode_acquire_response_payload(
        ByteView{acquired.value().payload.data(), acquired.value().payload.size()});
    CHECK(lease);
    CHECK(request(*control, MessageType::TUNE,
                  TuneRequestPayload{lease.value().lease_id, System::ISDB_T,
                                     500000U, 0xffffU, 0xffffU, 6000000U, 0U,
                                     100U}));
    CHECK(request(*control, MessageType::START_STREAM,
                  LeaseRequestPayload{lease.value().lease_id}));

    const EndpointConfig stream_endpoint{runtime.path().c_str(), serial,
                                         kStreamEndpointName,
                                         EndpointAccess::private_user};
    auto stream_result = SocketStream::connect(stream_endpoint, Timeout{2000U});
    CHECK(stream_result);
    SocketStream stream = std::move(stream_result.value());
    std::array<std::uint8_t, kFrameHeaderSize + kMaxControlPayload> storage{};
    StreamFramer framer(MutableByteView{storage.data(), storage.size()});
    std::array<std::uint8_t, kMaxControlPayload> payload{};
    const auto payload_size = encode_payload(
        AttachStreamRequestPayload{lease.value().lease_id, lease.value().nonce},
        MutableByteView{payload.data(), payload.size()});
    CHECK(payload_size);
    std::array<std::uint8_t, kFrameHeaderSize + kMaxControlPayload> frame{};
    const auto frame_size = encode_frame(
        FrameHeader{kProtocolMajor, kProtocolMinor, MessageType::ATTACH_STREAM,
                    MessageKind::request, 0U,
                    static_cast<std::uint32_t>(payload_size.value())},
        ByteView{payload.data(), payload_size.value()},
        MutableByteView{frame.data(), frame.size()});
    CHECK(frame_size && frame_size.value() > kFrameHeaderSize);
    const std::size_t first_bytes = kFrameHeaderSize - 1U;
    CHECK(send_socket_bytes(stream, frame.data(), first_bytes));
    CHECK(observation.wait_for_partial());
    CHECK(send_socket_bytes(stream, frame.data() + first_bytes,
                            frame_size.value() - first_bytes));
    FrameCollector frames;
    CHECK(read_raw_frames(stream, framer, frames, 1U));
    CHECK(frames.headers[0U].type == MessageType::ATTACH_STREAM &&
          frames.headers[0U].kind == MessageKind::response);

    // A stream connection accepts exactly one inbound frame.  A second
    // ATTACH is a protocol violation and must close the connection while
    // still executing the ordinary exact-identity cleanup.
    CHECK(send_raw_request(stream, MessageType::ATTACH_STREAM, 1U,
                           ByteView{payload.data(), payload_size.value()}));
    std::array<std::uint8_t, 1U> closed_buffer{};
    const auto closed = stream.read_some(
        MutableByteView{closed_buffer.data(), closed_buffer.size()}, Timeout{2000U});
    CHECK(!closed && closed.error() == Error::DISCONNECTED);
    CHECK(stream_control.wait_for_detaches(1U));
    CHECK(request(*control, MessageType::RELEASE,
                  LeaseRequestPayload{lease.value().lease_id}));
    stream.close();
    control->close();
    stop.store(true);
    server_thread.join();
    CHECK(server_ok.load());
    CHECK(server->shutdown());
    server_guard.dismiss();
    return true;
}

bool test_stream_endpoint_capacity_and_retired_reaping()
{
    TempRuntime runtime;
    CHECK(runtime.valid());
    constexpr const char* serial = "00001205000969";
    const EndpointConfig endpoint{runtime.path().c_str(), serial,
                                  kControlEndpointName,
                                  EndpointAccess::private_user};
    Backend card_backend;
    Session card_session;
    CardService card_service(card_backend, card_session);
    TunerBackend tuner_backend;
    TunerNonce tuner_nonce;
    TunerTime tuner_time;
    ControlStream stream_control;
    TunerService tuner_service(tuner_backend, tuner_nonce, tuner_time,
                               nullptr, nullptr, &stream_control);
    StreamServerObservation observation;
    StreamServerHookGuard hook_guard(observation);
    auto server_result = PosixControlServer::create(
        endpoint, card_service, tuner_service, serial, true, 0x03U,
        &stream_control);
    CHECK(server_result);
    std::unique_ptr<PosixControlServer> server = std::move(server_result.value());
    std::atomic<bool> stop{false};
    std::atomic<bool> server_ok{true};
    std::thread server_thread([&]() {
        while (!stop.load()) {
            const auto polled = server->poll_once(Timeout{10U});
            if (!polled) {
                server_ok.store(false);
                return;
            }
        }
    });
    ServerThreadGuard server_guard(stop, server_thread, *server);

    auto control_result = PosixControlClient::connect(
        endpoint, kCapabilityStreamStats, Timeout{2000U});
    CHECK(control_result);
    std::unique_ptr<PosixControlClient> control = std::move(control_result.value());
    const auto acquired = request(*control, MessageType::ACQUIRE,
                                  AcquireRequestPayload{2U});
    CHECK(acquired);
    const auto lease = decode_acquire_response_payload(
        ByteView{acquired.value().payload.data(), acquired.value().payload.size()});
    CHECK(lease);
    CHECK(request(*control, MessageType::TUNE,
                  TuneRequestPayload{lease.value().lease_id, System::ISDB_T,
                                     500000U, 0xffffU, 0xffffU, 6000000U, 0U,
                                     100U}));
    CHECK(request(*control, MessageType::START_STREAM,
                  LeaseRequestPayload{lease.value().lease_id}));
    const EndpointConfig stream_endpoint{runtime.path().c_str(), serial,
                                         kStreamEndpointName,
                                         EndpointAccess::private_user};
    auto attached_result = SocketStream::connect(stream_endpoint, Timeout{2000U});
    CHECK(attached_result);
    SocketStream attached = std::move(attached_result.value());
    std::array<std::uint8_t, kMaxControlPayload> payload{};
    const auto payload_size = encode_payload(
        AttachStreamRequestPayload{lease.value().lease_id, lease.value().nonce},
        MutableByteView{payload.data(), payload.size()});
    CHECK(payload_size && send_raw_request(
        attached, MessageType::ATTACH_STREAM, 0U,
        ByteView{payload.data(), payload_size.value()}));
    std::array<std::uint8_t, kFrameHeaderSize + kMaxControlPayload> attached_storage{};
    StreamFramer attached_framer(
        MutableByteView{attached_storage.data(), attached_storage.size()});
    FrameCollector attached_frames;
    CHECK(read_raw_frames(attached, attached_framer, attached_frames, 1U));
    CHECK(attached_frames.headers[0U].kind == MessageKind::response);

    // kMaxStreamConnections is a private production bound; this test mirrors
    // its exact supported value and observes acceptance through the private
    // test hook rather than guessing from connect() backlog state.
    constexpr std::size_t kSupportedDataClients = 32U;
    std::vector<SocketStream> idle_clients;
    idle_clients.reserve(kSupportedDataClients - 1U);
    for (std::size_t index = 1U; index < kSupportedDataClients; ++index) {
        auto idle_result = SocketStream::connect(stream_endpoint, Timeout{2000U});
        CHECK(idle_result);
        idle_clients.emplace_back(std::move(idle_result.value()));
        CHECK(observation.wait_for_capacity(index + 1U, 0U));
    }

    auto overflow_result = SocketStream::connect(stream_endpoint, Timeout{2000U});
    CHECK(overflow_result);
    SocketStream overflow = std::move(overflow_result.value());
    std::array<std::uint8_t, 1U> overflow_buffer{};
    const auto overflow_closed = overflow.read_some(
        MutableByteView{overflow_buffer.data(), overflow_buffer.size()}, Timeout{2000U});
    CHECK(!overflow_closed && overflow_closed.error() == Error::DISCONNECTED);

    // This removal needs a retired exact-identity owner before its detach
    // completion arrives.  Capacity remains full until that owner is reaped.
    attached.close();
    CHECK(observation.wait_for_retired_owner(kSupportedDataClients - 1U));
    CHECK(stream_control.wait_for_detaches(1U));
    CHECK(observation.wait_for_capacity(kSupportedDataClients - 1U, 0U));

    auto replacement_result = SocketStream::connect(
        stream_endpoint, Timeout{2000U});
    CHECK(replacement_result);
    SocketStream replacement = std::move(replacement_result.value());
    CHECK(observation.wait_for_capacity(kSupportedDataClients, 0U));
    replacement.close();
    overflow.close();
    for (SocketStream& idle : idle_clients) idle.close();
    CHECK(request(*control, MessageType::RELEASE,
                  LeaseRequestPayload{lease.value().lease_id}));
    control->close();
    stop.store(true);
    server_thread.join();
    CHECK(server_ok.load());
    CHECK(server->shutdown());
    server_guard.dismiss();
    return true;
}

bool test_stream_endpoint_nonreading_peer_isolated()
{
    TempRuntime runtime;
    CHECK(runtime.valid());
    constexpr const char* serial = "00001205000970";
    const EndpointConfig endpoint{runtime.path().c_str(), serial,
                                  kControlEndpointName,
                                  EndpointAccess::private_user};
    Backend card_backend;
    Session card_session;
    CardService card_service(card_backend, card_session);
    TunerBackend tuner_backend;
    TunerNonce tuner_nonce;
    TunerTime tuner_time;
    ControlStream stream_control;
    {
        std::lock_guard<std::mutex> lock(stream_control.mutex);
        stream_control.data_packet.fill(0x5aU);
        stream_control.data_packet[0U] = 0x47U;
        stream_control.continuous_data = true;
    }
    TunerService tuner_service(tuner_backend, tuner_nonce, tuner_time,
                               nullptr, nullptr, &stream_control);
    StreamServerObservation observation;
    StreamServerHookGuard hook_guard(observation);
    auto server_result = PosixControlServer::create(
        endpoint, card_service, tuner_service, serial, true, 0x03U,
        &stream_control);
    CHECK(server_result);
    std::unique_ptr<PosixControlServer> server = std::move(server_result.value());
    std::atomic<bool> stop{false};
    std::atomic<bool> server_ok{true};
    std::thread server_thread([&]() {
        while (!stop.load()) {
            const auto polled = server->poll_once(Timeout{10U});
            if (!polled) {
                server_ok.store(false);
                return;
            }
        }
    });
    ServerThreadGuard server_guard(stop, server_thread, *server);

    auto control_result = PosixControlClient::connect(
        endpoint, kCapabilityStreamStats, Timeout{2000U});
    CHECK(control_result);
    std::unique_ptr<PosixControlClient> control = std::move(control_result.value());
    const auto first_acquired = request(*control, MessageType::ACQUIRE,
                                        AcquireRequestPayload{2U});
    const auto second_acquired = request(*control, MessageType::ACQUIRE,
                                         AcquireRequestPayload{3U});
    CHECK(first_acquired && second_acquired);
    const auto first_lease = decode_acquire_response_payload(
        ByteView{first_acquired.value().payload.data(),
                 first_acquired.value().payload.size()});
    const auto second_lease = decode_acquire_response_payload(
        ByteView{second_acquired.value().payload.data(),
                 second_acquired.value().payload.size()});
    CHECK(first_lease && second_lease);
    CHECK(request(*control, MessageType::TUNE,
                  TuneRequestPayload{first_lease.value().lease_id, System::ISDB_T,
                                     500000U, 0xffffU, 0xffffU, 6000000U, 0U,
                                     100U}));
    CHECK(request(*control, MessageType::TUNE,
                  TuneRequestPayload{second_lease.value().lease_id, System::ISDB_T,
                                     500000U, 0xffffU, 0xffffU, 6000000U, 0U,
                                     100U}));
    CHECK(request(*control, MessageType::START_STREAM,
                  LeaseRequestPayload{first_lease.value().lease_id}));
    CHECK(request(*control, MessageType::START_STREAM,
                  LeaseRequestPayload{second_lease.value().lease_id}));

    const EndpointConfig stream_endpoint{runtime.path().c_str(), serial,
                                         kStreamEndpointName,
                                         EndpointAccess::private_user};
    auto first_result = SocketStream::connect(stream_endpoint, Timeout{2000U});
    auto second_result = SocketStream::connect(stream_endpoint, Timeout{2000U});
    CHECK(first_result && second_result);
    SocketStream nonreading = std::move(first_result.value());
    SocketStream sibling = std::move(second_result.value());
    int send_buffer = 256;
    CHECK(::setsockopt(nonreading.native_handle(), SOL_SOCKET, SO_SNDBUF,
                       &send_buffer, static_cast<socklen_t>(sizeof(send_buffer))) == 0);

    auto attach = [&](SocketStream& stream, const AcquireResponsePayload& lease,
                      StreamFramer& framer) noexcept {
        std::array<std::uint8_t, kMaxControlPayload> attach_payload{};
        const auto size = encode_payload(
            AttachStreamRequestPayload{lease.lease_id, lease.nonce},
            MutableByteView{attach_payload.data(), attach_payload.size()});
        if (!size || !send_raw_request(
                stream, MessageType::ATTACH_STREAM, 0U,
                ByteView{attach_payload.data(), size.value()})) return false;
        FrameCollector frames;
        return read_raw_frames(stream, framer, frames, 1U) &&
               frames.headers[0U].type == MessageType::ATTACH_STREAM &&
               frames.headers[0U].kind == MessageKind::response;
    };
    std::array<std::uint8_t, kFrameHeaderSize + kMaxControlPayload> first_storage{};
    std::array<std::uint8_t, kFrameHeaderSize + kMaxControlPayload> second_storage{};
    StreamFramer first_framer(MutableByteView{first_storage.data(), first_storage.size()});
    StreamFramer second_framer(
        MutableByteView{second_storage.data(), second_storage.size()});
    CHECK(attach(nonreading, first_lease.value(), first_framer));
    CHECK(attach(sibling, second_lease.value(), second_framer));

    std::atomic<bool> sibling_stop{false};
    std::atomic<bool> sibling_ok{true};
    std::atomic<std::size_t> sibling_data{0U};
    std::mutex sibling_mutex;
    std::condition_variable sibling_started_condition;
    std::condition_variable sibling_progress_condition;
    bool sibling_started = false;
    std::thread sibling_reader([&]() {
        {
            std::lock_guard<std::mutex> lock(sibling_mutex);
            sibling_started = true;
        }
        sibling_started_condition.notify_all();
        std::array<std::uint8_t, 8192U> input{};
        FrameCollector frames;
        while (!sibling_stop.load()) {
            frames.clear();
            const auto read = sibling.read_frames(
                MutableByteView{input.data(), input.size()}, second_framer, frames,
                Timeout{20U});
            if (!read) {
                if (read.error() == Error::TIMEOUT) continue;
                sibling_ok.store(false);
                sibling_progress_condition.notify_all();
                return;
            }
            for (std::size_t index = 0U; index < frames.count; ++index) {
                if (frames.headers[index].type == MessageType::TS_DATA) {
                    ++sibling_data;
                    sibling_progress_condition.notify_all();
                }
            }
        }
    });
    {
        std::unique_lock<std::mutex> lock(sibling_mutex);
        CHECK(sibling_started_condition.wait_for(lock, std::chrono::seconds(5),
                                                 [&]() noexcept {
                                                     return sibling_started;
                                                 }));
    }
    CHECK(observation.wait_for_write_blocked());
    {
        std::unique_lock<std::mutex> lock(sibling_mutex);
        (void)sibling_progress_condition.wait_for(
            lock, std::chrono::seconds(5), [&]() noexcept {
                return !sibling_ok.load() || sibling_data.load() != 0U;
            });
    }
    CHECK(sibling_ok.load() && sibling_data.load() != 0U);
    sibling_stop.store(true);
    sibling_reader.join();

    const auto stopped = request(*control, MessageType::STOP_STREAM,
                                 LeaseRequestPayload{second_lease.value().lease_id});
    CHECK(stopped);
    StreamEndEventPayload end{};
    CHECK(read_until_stream_end(sibling, second_framer, end));
    CHECK(end.error_code == ErrorCode::OK && end.counters.packets == 17U &&
          end.counters.bytes == 3196U && end.counters.queue_drops == 4U &&
          end.counters.usb_errors == 5U);
    CHECK(request(*control, MessageType::RELEASE,
                  LeaseRequestPayload{second_lease.value().lease_id}));

    // The first peer is intentionally still connected and non-reading.  Stop
    // the poll thread and exercise shutdown directly; it must close/cleanup
    // without waiting for that peer to flush STREAM_END.
    stop.store(true);
    server_thread.join();
    CHECK(server_ok.load());
    CHECK(server->shutdown());
    CHECK(stream_control.detach_calls == 2U);
    server_guard.dismiss();
    return true;
}

}  // namespace

bool run_control_integration_tests()
{
    return test_control_server_card_flow_and_shutdown() &&
           test_no_hello_timeout_and_malformed_disconnect() &&
           test_control_server_tuner_flow() &&
           test_control_server_lanes_are_independent() &&
           test_control_server_disconnect_during_blocked_acquire() &&
           test_control_server_disconnect_during_blocked_tune() &&
           test_stream_endpoint_attach_and_stream_end() &&
           test_stream_endpoint_terminal_mapping() &&
           test_stream_attach_submit_failure_cleanup() &&
           test_stream_hup_before_attach_completion_cleanup() &&
           test_stream_endpoint_fragmented_and_duplicate() &&
           test_stream_endpoint_capacity_and_retired_reaping() &&
           test_stream_endpoint_nonreading_peer_isolated();
}
