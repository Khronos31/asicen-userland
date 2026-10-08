// SPDX-License-Identifier: GPL-2.0-only
#include "px4_ts_core.h"
#include "px4_ts_posix.h"

#include "px4/posix_ipc.h"
#include "test_temp_directory.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <fcntl.h>
#include <iterator>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

using namespace px4::userland;
using namespace px4::userland::cli;
using namespace px4::userland::ipc;
using namespace px4::userland::ipc::posix;

#define CHECK(condition)                                                               \
    do {                                                                               \
        if (!(condition)) {                                                            \
            std::fprintf(stderr, "px4-ts check failed at %s:%d: %s\n",              \
                         __FILE__, __LINE__, #condition);                              \
            return false;                                                             \
        }                                                                              \
    } while (false)

class TempRuntime final {
public:
    TempRuntime() noexcept
    {
        std::string pattern = test::temporary_directory_template("px4-ts-test-");
        char* path = ::mkdtemp(pattern.data());
        if (path != nullptr) {
            path_ = path;
            valid_ = true;
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

class CaptureOutput final : public Px4TsOutput {
public:
    Result<void> write(ByteView bytes, const Px4TsSignal&) noexcept override
    {
        if (fail) return Result<void>::failure(Error::INTERNAL);
        if (bytes.data == nullptr || bytes.size == 0U)
            return Result<void>::failure(Error::INTERNAL);
        data.insert(data.end(), bytes.data, bytes.data + bytes.size);
        return Result<void>::success();
    }

    std::vector<std::uint8_t> data;
    bool fail = false;
};

class PartialOutput final : public Px4TsOutput {
public:
    Result<void> write(ByteView bytes, const Px4TsSignal&) noexcept override
    {
        if (bytes.data == nullptr || bytes.size != 188U)
            return Result<void>::failure(Error::INVALID_ARGUMENT);
        written += 1U;
        return Result<void>::failure(Error::INTERNAL);
    }

    std::size_t written = 0U;
};

class TestClock final : public Px4TsClock {
public:
    using SleepObserver = void (*)(void*, std::uint64_t) noexcept;
    std::uint64_t monotonic_ms() noexcept override
    {
        const std::uint64_t result = now;
        now += monotonic_step_ms;
        return result;
    }
    void sleep_ms(std::uint32_t milliseconds) noexcept override
    {
        now += milliseconds;
        if (observer != nullptr) observer(observer_context, now);
    }
    std::uint64_t now = 0U;
    std::uint64_t monotonic_step_ms = 0U;
    SleepObserver observer = nullptr;
    void* observer_context = nullptr;
};

class TestSignal final : public Px4TsSignal {
public:
    bool stop_requested() const noexcept override
    {
        ++checks;
        return checks >= true_after_checks;
    }

    mutable std::uint32_t checks = 0U;
    std::uint32_t true_after_checks = 0xffffffffU;
};

class IgnoreSigpipe final {
public:
    IgnoreSigpipe() noexcept
    {
        struct sigaction action {};
        action.sa_handler = SIG_IGN;
        if (sigemptyset(&action.sa_mask) == 0 &&
            ::sigaction(SIGPIPE, &action, &old_action_) == 0) {
            installed_ = true;
        }
    }

    ~IgnoreSigpipe() noexcept
    {
        if (installed_) (void)::sigaction(SIGPIPE, &old_action_, nullptr);
    }

    bool installed() const noexcept { return installed_; }

private:
    struct sigaction old_action_ {};
    bool installed_ = false;
};

enum class FakeMode : std::uint8_t {
    normal,
    normal_empty_intervals,
    initial_stall,
    mid_stall,
    duration_stall_tie,
    sequence_gap,
    bad_sync,
    bad_alignment,
    drops,
    terminal_usb,
    terminal_slow,
    terminal_disconnected,
    terminal_sync,
    terminal_fatal,
    bad_final,
    duplicate_attach,
    duration_wait,
    disconnect_after_tune,
    stream_connect_failure,
    disconnect_after_start,
    attach_response_failure,
    data_read_failure,
    counter_sync,
    counter_tei,
    counter_continuity,
    counter_queue,
    counter_usb,
};

struct FakeDaemon;

class ControlFrames final : public FrameConsumer {
public:
    ControlFrames(FakeDaemon& daemon, SocketStream& stream) noexcept
        : daemon_(daemon), stream_(stream) {}
    Result<void> on_frame(const FrameView& frame) noexcept override;
private:
    FakeDaemon& daemon_;
    SocketStream& stream_;
};

class StreamFrames final : public FrameConsumer {
public:
    StreamFrames(FakeDaemon& daemon, SocketStream& stream) noexcept
        : daemon_(daemon), stream_(stream) {}
    Result<void> on_frame(const FrameView& frame) noexcept override;
private:
    FakeDaemon& daemon_;
    SocketStream& stream_;
};

class ThreadCompletionGuard final {
public:
    explicit ThreadCompletionGuard(std::atomic<bool>& completed) noexcept
        : completed_(completed) {}
    ~ThreadCompletionGuard() noexcept { completed_.store(true); }

private:
    std::atomic<bool>& completed_;
};

struct FakeDaemon final {
    FakeDaemon(const std::string& runtime, FakeMode mode_value) noexcept
        : runtime_(runtime), mode(mode_value)
    {
        const EndpointConfig control{runtime_.c_str(), serial, kControlEndpointName,
                                     EndpointAccess::private_user};
        const EndpointConfig stream{runtime_.c_str(), serial, kStreamEndpointName,
                                    EndpointAccess::private_user};
        auto control_result = SocketListener::listen(control);
        auto stream_result = SocketListener::listen(stream);
        if (control_result &&
            (mode == FakeMode::stream_connect_failure ||
             mode == FakeMode::disconnect_after_start || stream_result)) {
            control_listener = std::move(control_result.value());
            if (stream_result && mode != FakeMode::stream_connect_failure &&
                mode != FakeMode::disconnect_after_start)
                stream_listener = std::move(stream_result.value());
            valid = true;
        }
    }

    ~FakeDaemon() noexcept
    {
        shutdown_requested.store(true);
        stop_condition.notify_all();
        join_threads();
    }

    void join_threads() noexcept
    {
        if (control_thread.joinable()) control_thread.join();
        if (stream_thread.joinable()) stream_thread.join();
        threads_joined.store(true);
    }

    bool start() noexcept
    {
        if (!valid) return false;
        control_started.store(true);
        control_thread = std::thread([this] { run_control(); });
        if (stream_listener.valid()) {
            stream_started.store(true);
            stream_thread = std::thread([this] { run_stream(); });
        }
        return true;
    }

    void send(SocketStream& stream, const FrameHeader& header, ByteView payload) noexcept
    {
        std::vector<std::uint8_t> frame(kFrameHeaderSize + kMaxControlPayload, 0U);
        const auto encoded = encode_frame(header, payload,
                                          MutableByteView{frame.data(), frame.size()});
        if (!encoded) return;
        if (stream_end_attempted.load()) {
            stream_end_preflight_error.store(
                decode_frame(ByteView{frame.data(), encoded.value()}) ? 0U : 1U);
        }
        const auto written = stream.write_frame(ByteView{frame.data(), encoded.value()},
                                                Timeout{2000U});
        if (stream_end_attempted.load()) {
            stream_end_write_error.store(written ? 0U :
                                         static_cast<unsigned int>(written.error()) + 1U);
            stream_end_frame_size.store(encoded.value());
            stream_end_header_error.store(
                decode_frame_header(ByteView{frame.data(), kFrameHeaderSize}) ? 0U : 1U);
            stream_end_payload_error.store(
                decode_frame(ByteView{frame.data(), encoded.value()}) ? 0U : 1U);
        }
        // A peer closing the data socket is local to this fake stream.  It
        // must not terminate the control lane before the runner's RELEASE
        // request has been consumed.
        (void)written;
    }

    template <typename Payload>
    void send_payload(SocketStream& stream, const FrameHeader& request,
                      MessageKind kind, const Payload& payload) noexcept
    {
        std::vector<std::uint8_t> payload_buffer(kMaxControlPayload, 0U);
        const auto size = encode_payload(payload,
                                         MutableByteView{payload_buffer.data(), payload_buffer.size()});
        if (!size) return;
        send(stream, FrameHeader{kProtocolMajor, kProtocolMinor, request.type, kind,
                                 request.request_id, static_cast<std::uint32_t>(size.value())},
             ByteView{payload_buffer.data(), size.value()});
    }

    void send_empty(SocketStream& stream, const FrameHeader& request) noexcept
    {
        send(stream, FrameHeader{kProtocolMajor, kProtocolMinor, request.type,
                                 MessageKind::response, request.request_id, 0U},
             ByteView{nullptr, 0U});
    }

    void run_control() noexcept
    {
        ThreadCompletionGuard completed(control_done);
        Result<SocketStream> accepted = Result<SocketStream>::failure(Error::TIMEOUT);
        while (!shutdown_requested.load()) {
            accepted = control_listener.accept(Timeout{100U});
            if (accepted || accepted.error() != Error::TIMEOUT) break;
        }
        if (!accepted) {
            return;
        }
        SocketStream stream = std::move(accepted.value());
        control_accepted.store(true);
        std::vector<std::uint8_t> storage(kFrameHeaderSize + kMaxControlPayload, 0U);
        std::array<std::uint8_t, 8192U> input{};
        StreamFramer framer(MutableByteView{storage.data(), storage.size()});
        ControlFrames consumer(*this, stream);
        while (!shutdown_requested.load() && !released.load()) {
            const auto read = stream.read_frames(MutableByteView{input.data(), input.size()},
                                                 framer, consumer, Timeout{30000U});
            if (!read) {
                break;
            }
        }
    }

    void run_stream() noexcept
    {
        ThreadCompletionGuard completed(stream_done);
        Result<SocketStream> accepted = Result<SocketStream>::failure(Error::TIMEOUT);
        while (!shutdown_requested.load()) {
            accepted = stream_listener.accept(Timeout{100U});
            if (accepted || accepted.error() != Error::TIMEOUT) break;
        }
        if (!accepted) {
            return;
        }
        SocketStream stream = std::move(accepted.value());
        stream_accepted.store(true);
        std::vector<std::uint8_t> storage(kFrameHeaderSize + kMaxControlPayload, 0U);
        std::array<std::uint8_t, 1024U> input{};
        StreamFramer framer(MutableByteView{storage.data(), storage.size()});
        StreamFrames consumer(*this, stream);
        const auto read = stream.read_frames(MutableByteView{input.data(), input.size()}, framer,
                                             consumer, Timeout{5000U});
        if (!read || !attached.load()) {
            return;
        }
        if (mode == FakeMode::duration_wait) {
            {
                std::unique_lock<std::mutex> lock(stop_mutex);
                stop_condition.wait(lock, [this] {
                    return duration_data_allowed || shutdown_requested.load() ||
                           released.load();
                });
                if (shutdown_requested.load() || released.load()) return;
            }
            send_data(stream);
            stream_data_sent.store(true);
            std::unique_lock<std::mutex> lock(stop_mutex);
            stop_condition.wait(lock, [this] {
                return stop_response_sent.load() || shutdown_requested.load() || released.load();
            });
        } else if (mode == FakeMode::initial_stall || mode == FakeMode::mid_stall ||
                   mode == FakeMode::duration_stall_tie) {
            if (mode == FakeMode::mid_stall || mode == FakeMode::duration_stall_tie) {
                send_data(stream);
                stream_data_sent.store(true);
            }
            std::unique_lock<std::mutex> lock(stop_mutex);
            stop_condition.wait(lock, [this] {
                return stop_seen.load() || shutdown_requested.load() || released.load();
            });
            if (shutdown_requested.load() || released.load()) return;
            stream_end_attempted.store(true);
            send_end(stream);
            stream_end_sent.store(true);
            return;
        } else {
            send_data(stream);
            stream_data_sent.store(true);
            if (mode == FakeMode::data_read_failure) return;
            // Queue the terminal frame immediately after the data frame.  The
            // control STOP response and stream terminal event use different
            // sockets; waiting for STOP here made the fixture depend on the
            // runner scheduling the data thread before its injected clock
            // exhausted the post-STOP budget.  Socket ordering keeps the
            // attach response, data, and terminal frame deterministic, while
            // the control side still records and validates STOP/RELEASE.
            stream_end_attempted.store(true);
            send_end(stream);
            stream_end_sent.store(true);
            return;
        }
        if (shutdown_requested.load() && !stop_seen.load()) return;
        stream_end_attempted.store(true);
        send_end(stream);
        stream_end_sent.store(true);
    }

    void release_duration_data() noexcept
    {
        std::lock_guard<std::mutex> lock(stop_mutex);
        duration_data_allowed = true;
        stop_condition.notify_all();
    }

    void send_data(SocketStream& stream) noexcept
    {
        if (mode == FakeMode::bad_alignment) {
            std::vector<std::uint8_t> frame(kFrameHeaderSize + kMaxTsDataPayload, 0U);
            std::vector<std::uint8_t> payload(kMaxTsDataPayload, 0U);
            std::array<std::uint8_t, 188U> packet{};
            packet[0] = 0x47U;
            const TsDataEventPayload valid_data{0U, 0U,
                                                ByteView{packet.data(), packet.size()}};
            const auto payload_size = encode_payload(
                valid_data, MutableByteView{payload.data(), payload.size()});
            if (!payload_size) {
                shutdown_requested.store(true);
                return;
            }
            const auto encoded = encode_frame(
                FrameHeader{kProtocolMajor, kProtocolMinor, MessageType::TS_DATA,
                            MessageKind::event, 0U,
                            static_cast<std::uint32_t>(payload_size.value())},
                ByteView{payload.data(), payload_size.value()},
                MutableByteView{frame.data(), frame.size()});
            if (!encoded) {
                shutdown_requested.store(true);
                return;
            }
            frame[16U] = 21U;
            frame[17U] = 0U;
            frame[18U] = 0U;
            frame[19U] = 0U;
            frame[36U] = 1U;
            frame[37U] = 0U;
            frame[38U] = 0U;
            frame[39U] = 0U;
            std::size_t offset = 0U;
            while (offset < 41U) {
#if defined(MSG_NOSIGNAL)
                constexpr int flags = MSG_NOSIGNAL;
#else
                constexpr int flags = 0;
#endif
                const ssize_t written = ::send(stream.native_handle(), frame.data() + offset,
                                               41U - offset, flags);
                if (written > 0) offset += static_cast<std::size_t>(written);
                else if (written < 0 && errno == EINTR) continue;
                else {
                    shutdown_requested.store(true);
                    return;
                }
            }
            return;
        }
        std::array<std::uint8_t, 376U> packets{};
        for (std::size_t offset = 0U; offset < packets.size(); offset += 188U) {
            packets[offset] = 0x47U;
            packets[offset + 1U] = 0x00U;
            packets[offset + 2U] = static_cast<std::uint8_t>(offset / 188U);
        }
        if (mode == FakeMode::bad_sync) packets[188U] = 0x46U;
        const std::uint64_t sequence = mode == FakeMode::sequence_gap ? 1U : 0U;
        const std::uint64_t drops = mode == FakeMode::drops ? 2U : 0U;
        const TsDataEventPayload data{sequence, drops, ByteView{packets.data(), packets.size()}};
        send_payload(stream, FrameHeader{kProtocolMajor, kProtocolMinor, MessageType::TS_DATA,
                                         MessageKind::event, 0U, 0U}, MessageKind::event, data);
        if (mode == FakeMode::duplicate_attach) {
            send_empty(stream, FrameHeader{kProtocolMajor, kProtocolMinor,
                                           MessageType::ATTACH_STREAM,
                                           MessageKind::request, 0U, 0U});
        }
    }

    CountersPayload counters_for_mode() const noexcept
    {
        CountersPayload counters = final_counters;
        if (mode == FakeMode::normal_empty_intervals)
            counters.empty_intervals = 3U;
        if (mode == FakeMode::counter_sync) counters.sync_errors = 1U;
        if (mode == FakeMode::counter_tei) counters.tei_packets = 1U;
        if (mode == FakeMode::counter_continuity) counters.continuity_errors = 1U;
        if (mode == FakeMode::counter_queue) counters.queue_drops = 1U;
        if (mode == FakeMode::counter_usb) counters.usb_errors = 1U;
        return counters;
    }

    void send_end(SocketStream& stream) noexcept
    {
        const ErrorCode code = mode == FakeMode::terminal_usb ? ErrorCode::USB_IO :
                               mode == FakeMode::terminal_slow ? ErrorCode::SLOW_CONSUMER :
                               mode == FakeMode::terminal_disconnected ? ErrorCode::DISCONNECTED :
                               mode == FakeMode::terminal_sync ? ErrorCode::PROTOCOL_ERROR :
                               mode == FakeMode::terminal_fatal ? ErrorCode::INTERNAL :
                               ErrorCode::OK;
        CountersPayload counters = counters_for_mode();
        if (mode == FakeMode::bad_final) counters.bytes = 188U;
        send_payload(stream, FrameHeader{kProtocolMajor, kProtocolMinor, MessageType::STREAM_END,
                                         MessageKind::event, 0U, 0U}, MessageKind::event,
                     StreamEndEventPayload{counters, code});
    }

    std::string runtime_;
    const char* serial = "00001205000960";
    FakeMode mode;
    SocketListener control_listener;
    SocketListener stream_listener;
    std::thread control_thread;
    std::thread stream_thread;
    std::atomic<bool> valid{false};
    // These are deliberately separate lifecycle states.  A data peer closing
    // must not stop the control lane before RELEASE has been consumed.
    std::atomic<bool> shutdown_requested{false};
    std::atomic<bool> control_started{false};
    std::atomic<bool> control_accepted{false};
    std::atomic<bool> control_done{false};
    std::atomic<bool> stream_started{false};
    std::atomic<bool> stream_accepted{false};
    std::atomic<bool> stream_done{false};
    std::atomic<bool> stream_data_sent{false};
    std::atomic<bool> stream_waiting{false};
    std::atomic<bool> stream_end_attempted{false};
    std::atomic<bool> stream_end_sent{false};
    std::atomic<unsigned int> stream_end_write_error{0U};
    std::atomic<std::size_t> stream_end_frame_size{0U};
    std::atomic<unsigned int> stream_end_header_error{0U};
    std::atomic<unsigned int> stream_end_payload_error{0U};
    std::atomic<unsigned int> stream_end_preflight_error{0U};
    std::atomic<bool> threads_joined{false};
    std::atomic<bool> attached{false};
    std::atomic<bool> released{false};
    std::atomic<bool> stop_seen{false};
    std::atomic<bool> stop_response_sent{false};
    std::atomic<std::size_t> stop_requests{0U};
    std::atomic<std::size_t> release_requests{0U};
    std::atomic<std::uint32_t> attach_request_id{0U};
    std::atomic<bool> attach_seen{false};
    std::atomic<std::uint8_t> acquired_receiver{0xffU};
    std::atomic<std::uint64_t> observed_frequency{0U};
    std::atomic<std::uint16_t> observed_stream_id{0xffffU};
    std::atomic<std::uint16_t> observed_slot{0xffffU};
    std::atomic<std::uint32_t> observed_bandwidth{0U};
    std::atomic<std::uint8_t> observed_lnb{0U};
    std::atomic<std::uint32_t> observed_timeout{0U};
    std::mutex stop_mutex;
    std::condition_variable stop_condition;
    bool duration_data_allowed = false;
    std::uint64_t lease_id = 0x1122334455667788ULL;
    std::array<std::uint8_t, kNonceLength> nonce{
        0x00U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U, 0x77U,
        0x88U, 0x99U, 0xaaU, 0xbbU, 0xccU, 0xddU, 0xeeU, 0xffU};
    CountersPayload final_counters{2U, 376U, 0U, 0U, 0U, 0U, 0U, 0U};
    std::atomic<System> observed_system{System::ISDB_T};
};

Result<void> ControlFrames::on_frame(const FrameView& frame) noexcept
{
    if (frame.header.type == MessageType::HELLO) {
        daemon_.send_payload(stream_, frame.header, MessageKind::response,
                             HelloResponsePayload{kProtocolMajor, kProtocolMinor,
                                                  kCapabilityEvents | kCapabilityStreamStats});
    } else if (frame.header.type == MessageType::ACQUIRE) {
        const auto acquire = decode_acquire_request_payload(frame.payload);
        if (!acquire) return Result<void>::failure(Error::PROTOCOL_ERROR);
        daemon_.acquired_receiver = acquire.value().receiver_id;
        daemon_.send_payload(stream_, frame.header, MessageKind::response,
                             AcquireResponsePayload{daemon_.lease_id, daemon_.nonce});
    } else if (frame.header.type == MessageType::TUNE) {
        const auto tune = decode_tune_request_payload(frame.payload);
        if (!tune) return Result<void>::failure(Error::PROTOCOL_ERROR);
        daemon_.observed_frequency = tune.value().frequency_khz;
        daemon_.observed_stream_id = tune.value().stream_id;
        daemon_.observed_slot = tune.value().slot;
        daemon_.observed_bandwidth = tune.value().bandwidth_hz;
        daemon_.observed_lnb = tune.value().lnb_voltage;
        daemon_.observed_timeout = tune.value().timeout_ms;
        daemon_.observed_system = tune.value().system;
        if (daemon_.mode == FakeMode::disconnect_after_tune) {
            daemon_.shutdown_requested.store(true);
            daemon_.stop_condition.notify_all();
            return Result<void>::failure(Error::DISCONNECTED);
        }
        daemon_.send_payload(stream_, frame.header, MessageKind::response,
                             TuneResponsePayload{1U, 0});
    } else if (frame.header.type == MessageType::START_STREAM) {
        daemon_.send_empty(stream_, frame.header);
    } else if (frame.header.type == MessageType::STOP_STREAM) {
        ++daemon_.stop_requests;
        daemon_.send_payload(stream_, frame.header, MessageKind::response,
                             daemon_.counters_for_mode());
        // The stream endpoint must not race the control STOP response.  The
        // runner starts consuming STREAM_END only after that response, so
        // this is an explicit fixture lifecycle barrier rather than a timing
        // dependency between the two fake endpoint threads.
        daemon_.stop_response_sent.store(true);
        daemon_.stop_seen.store(true);
        daemon_.stop_condition.notify_all();
    } else if (frame.header.type == MessageType::RELEASE) {
        ++daemon_.release_requests;
        daemon_.released.store(true);
        daemon_.stop_condition.notify_all();
        daemon_.send_empty(stream_, frame.header);
    }
    return Result<void>::success();
}

Result<void> StreamFrames::on_frame(const FrameView& frame) noexcept
{
    if (frame.header.type != MessageType::ATTACH_STREAM) return Result<void>::failure(Error::PROTOCOL_ERROR);
    const auto attach = decode_attach_stream_request_payload(frame.payload);
    if (!attach || attach.value().lease_id != daemon_.lease_id ||
        attach.value().nonce != daemon_.nonce)
        return Result<void>::failure(Error::NOT_FOUND);
    daemon_.attached.store(true);
    daemon_.attach_seen.store(true);
    daemon_.attach_request_id = frame.header.request_id;
    if (daemon_.mode == FakeMode::attach_response_failure)
        return Result<void>::success();
    daemon_.send_empty(stream_, frame.header);
    return Result<void>::success();
}

Px4TsArguments parse(std::initializer_list<const char*> values)
{
    std::vector<const char*> argv(values.begin(), values.end());
    return parse_px4_ts_arguments(static_cast<int>(argv.size()), argv.data());
}

Px4TsArguments terrestrial_arguments(bool packet_limit = true)
{
    return parse(packet_limit ?
        std::initializer_list<const char*>{"px4-ts", "--device", "00001205000960",
            "--receiver", "0", "--system", "isdb-t", "--frequency-khz", "40000",
            "--packet-count", "1"} :
        std::initializer_list<const char*>{"px4-ts", "--device", "00001205000960",
            "--receiver", "0", "--system", "isdb-t", "--frequency-khz", "40000",
            "--duration-seconds", "1"});
}

bool run_fake(FakeMode mode, Px4TsArguments arguments, Error expected,
              std::size_t expected_output, bool expect_satellite = false,
              Px4TsFailureKind expected_kind = Px4TsFailureKind::ipc,
              int expected_exit = -1)
{
    if (!arguments.valid) {
        return false;
    }
    if (mode == FakeMode::initial_stall || mode == FakeMode::mid_stall)
        arguments.duration_seconds = 10U;
    if (mode == FakeMode::duration_stall_tie)
        arguments.duration_seconds = 5U;
    TempRuntime runtime;
    if (!runtime.valid()) return false;
    FakeDaemon daemon(runtime.path(), mode);
    arguments.runtime_directory = runtime.path();
    if (!daemon.start()) return false;
    CaptureOutput output;
    TestClock clock;
    if (mode == FakeMode::duration_wait) {
        clock.observer_context = &daemon;
        clock.observer = [](void* context, std::uint64_t now) noexcept {
            if (now >= 30U)
                static_cast<FakeDaemon*>(context)->release_duration_data();
        };
    } else if (mode == FakeMode::initial_stall || mode == FakeMode::mid_stall) {
        // 100 ms remains within the required one-second observation bound,
        // while making the fake-clock timeout independent of wall time.
        clock.monotonic_step_ms = 100U;
    }
    TestSignal signal;
    Px4TsFailureKind failure_kind = Px4TsFailureKind::ipc;
    Px4TsRunDiagnostics diagnostics;
    const auto result = Px4TsRunner::run(arguments, output, clock, signal,
                                         &failure_kind, &diagnostics);
    // Runner cleanup closes both client sockets after RELEASE.  Join the fake
    // endpoints before inspecting their state so no endpoint thread can still
    // race the assertions or the fixture destructor.
    daemon.join_threads();
    const bool started = mode != FakeMode::disconnect_after_tune;
    const bool daemon_available = mode != FakeMode::disconnect_after_tune;
    const auto report_failure = [&](const char* invariant) noexcept {
        const unsigned int actual_error = result ? 0U : static_cast<unsigned int>(result.error());
        const unsigned int actual_exit = result ? 0U : px4_ts_exit_status(result.error(), failure_kind);
        const CountersPayload result_counters = result ? result.value().counters : CountersPayload{};
        const CountersPayload expected_counters = daemon.counters_for_mode();
        std::size_t first_bad_sync = output.data.size();
        for (std::size_t index = 0U; index < output.data.size(); ++index) {
            if ((index % 188U) == 0U && output.data[index] != 0x47U) {
                first_bad_sync = index;
                break;
            }
        }
        std::fprintf(stderr,
                     "run_fake invariant=%s mode=%u result=%u/%u failure_kind=%u/%u exit=%u/%d "
                     "result_counters=%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu "
                     "expected_counters=%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu "
                     "acquire=%u/%u system=%u/%u freq=%llu/%llu stream=%u/%u slot=%u/%u "
                     "bandwidth=%u/%u lnb=%u/%u timeout=%u/%u attach=%u attach_id=%u "
                     "stop=%zu/%u release=%zu/%u output=%zu/%zu first_bad_sync=%zu "
                     "threads={control:%u,%u,%u stream:%u,%u,%u data:%u waiting:%u end:%u,%u joined:%u}\n",
                     invariant, static_cast<unsigned int>(mode), actual_error,
                     static_cast<unsigned int>(expected), static_cast<unsigned int>(failure_kind),
                     static_cast<unsigned int>(expected_kind), actual_exit, expected_exit,
                     static_cast<unsigned long long>(result_counters.packets),
                     static_cast<unsigned long long>(result_counters.bytes),
                     static_cast<unsigned long long>(result_counters.sync_errors),
                     static_cast<unsigned long long>(result_counters.tei_packets),
                     static_cast<unsigned long long>(result_counters.continuity_errors),
                     static_cast<unsigned long long>(result_counters.queue_drops),
                     static_cast<unsigned long long>(result_counters.usb_errors),
                     static_cast<unsigned long long>(result_counters.empty_intervals),
                     static_cast<unsigned long long>(expected_counters.packets),
                     static_cast<unsigned long long>(expected_counters.bytes),
                     static_cast<unsigned long long>(expected_counters.sync_errors),
                     static_cast<unsigned long long>(expected_counters.tei_packets),
                     static_cast<unsigned long long>(expected_counters.continuity_errors),
                     static_cast<unsigned long long>(expected_counters.queue_drops),
                     static_cast<unsigned long long>(expected_counters.usb_errors),
                     static_cast<unsigned long long>(expected_counters.empty_intervals),
                     static_cast<unsigned int>(daemon.acquired_receiver.load()),
                     static_cast<unsigned int>(arguments.receiver),
                     static_cast<unsigned int>(daemon.observed_system.load()),
                     static_cast<unsigned int>(arguments.system),
                     static_cast<unsigned long long>(daemon.observed_frequency.load()),
                     static_cast<unsigned long long>(arguments.frequency_khz),
                     static_cast<unsigned int>(daemon.observed_stream_id.load()),
                     static_cast<unsigned int>(arguments.stream_id),
                     static_cast<unsigned int>(daemon.observed_slot.load()),
                     static_cast<unsigned int>(arguments.slot),
                     daemon.observed_bandwidth.load(), arguments.bandwidth_hz,
                     static_cast<unsigned int>(daemon.observed_lnb.load()),
                     static_cast<unsigned int>(arguments.lnb_voltage), daemon.observed_timeout.load(),
                     arguments.tune_timeout_ms, static_cast<unsigned int>(daemon.attach_seen.load()),
                     daemon.attach_request_id.load(), daemon.stop_requests.load(),
                     static_cast<unsigned int>(started), daemon.release_requests.load(),
                     static_cast<unsigned int>(daemon_available), output.data.size(), expected_output,
                     first_bad_sync, static_cast<unsigned int>(daemon.control_started.load()),
                     static_cast<unsigned int>(daemon.control_accepted.load()),
                     static_cast<unsigned int>(daemon.control_done.load()),
                     static_cast<unsigned int>(daemon.stream_started.load()),
                     static_cast<unsigned int>(daemon.stream_accepted.load()),
                     static_cast<unsigned int>(daemon.stream_done.load()),
                     static_cast<unsigned int>(daemon.stream_data_sent.load()),
                     static_cast<unsigned int>(daemon.stream_waiting.load()),
                     static_cast<unsigned int>(daemon.stream_end_attempted.load()),
                     static_cast<unsigned int>(daemon.stream_end_sent.load()),
                     static_cast<unsigned int>(daemon.threads_joined.load()));
        return false;
    };
    if (result) {
        if (expected != Error::OK) {
            return report_failure("result_error_unexpected_success");
        }
        if (result.value().counters.packets != daemon.final_counters.packets) {
            return report_failure("result_counter_packets");
        }
    } else if (result.error() != expected) {
        return report_failure("result_error");
    }
    if (!result && failure_kind != expected_kind) {
        return report_failure("failure_kind");
    }
    if (!result && expected_exit >= 0 &&
        px4_ts_exit_status(result.error(), failure_kind) != expected_exit) {
        return report_failure("exit_status");
    }
    if (mode == FakeMode::duration_wait && clock.now < 1030U) {
        return report_failure("duration_starts_at_first_output");
    }
    const bool expects_consumed_end =
        mode == FakeMode::normal || mode == FakeMode::normal_empty_intervals ||
        mode == FakeMode::initial_stall || mode == FakeMode::mid_stall ||
        mode == FakeMode::duration_stall_tie ||
        mode == FakeMode::duration_wait ||
        mode == FakeMode::terminal_usb || mode == FakeMode::terminal_slow ||
        mode == FakeMode::terminal_disconnected || mode == FakeMode::terminal_sync ||
        mode == FakeMode::terminal_fatal ||
        mode == FakeMode::counter_sync || mode == FakeMode::counter_tei ||
        mode == FakeMode::counter_continuity || mode == FakeMode::counter_queue ||
        mode == FakeMode::counter_usb;
    if (expects_consumed_end) {
        if (!diagnostics.stop_counters_valid || !diagnostics.stream_end_valid ||
            diagnostics.packets_written != output.data.size() / 188U) {
            return report_failure("run_diagnostics_presence");
        }
        const CountersPayload expected_diagnostics = daemon.counters_for_mode();
        const CountersPayload& stop = diagnostics.stop_counters;
        const CountersPayload& end = diagnostics.stream_end.counters;
        if (stop.packets != expected_diagnostics.packets ||
            stop.bytes != expected_diagnostics.bytes ||
            stop.sync_errors != expected_diagnostics.sync_errors ||
            stop.tei_packets != expected_diagnostics.tei_packets ||
            stop.continuity_errors != expected_diagnostics.continuity_errors ||
            stop.queue_drops != expected_diagnostics.queue_drops ||
            stop.usb_errors != expected_diagnostics.usb_errors ||
            stop.empty_intervals != expected_diagnostics.empty_intervals ||
            end.packets != expected_diagnostics.packets ||
            end.sync_errors != expected_diagnostics.sync_errors ||
            end.tei_packets != expected_diagnostics.tei_packets ||
            end.continuity_errors != expected_diagnostics.continuity_errors ||
            end.queue_drops != expected_diagnostics.queue_drops ||
            end.usb_errors != expected_diagnostics.usb_errors ||
            end.empty_intervals != expected_diagnostics.empty_intervals ||
            (mode != FakeMode::bad_final && end.bytes != expected_diagnostics.bytes)) {
            return report_failure("run_diagnostics_counters");
        }
    }
    if (daemon.acquired_receiver.load() != arguments.receiver ||
        daemon.observed_system.load() != arguments.system ||
        daemon.observed_frequency.load() != arguments.frequency_khz ||
        daemon.observed_stream_id.load() != arguments.stream_id ||
        daemon.observed_slot.load() != arguments.slot ||
        daemon.observed_bandwidth.load() != arguments.bandwidth_hz ||
        daemon.observed_lnb.load() != arguments.lnb_voltage ||
        daemon.observed_timeout.load() != arguments.tune_timeout_ms) {
        return report_failure("tune_fields");
    }
    if (expect_satellite && daemon.observed_system.load() != System::ISDB_S)
        return report_failure("satellite_system");
    if (daemon.attach_seen.load() && daemon.attach_request_id.load() != 0U) {
        return report_failure("attach_request_id");
    }
    if (daemon.release_requests.load() != (daemon_available ? 1U : 0U) ||
        daemon.stop_requests.load() != (started ? 1U : 0U)) {
        return report_failure("cleanup_counts");
    }
    for (std::size_t offset = 0U; offset < output.data.size(); offset += 188U) {
        if (output.data[offset] != 0x47U) return report_failure("output_sync");
    }
    if (output.data.size() != expected_output) return report_failure("output_size");
    return true;
}

bool test_argument_contract()
{
    CHECK(terrestrial_arguments().valid);
    const Px4TsArguments default_timeout = parse(
        {"px4-ts", "--device", "00001205000960", "--receiver", "0",
         "--system", "isdb-t", "--frequency-khz", "40000"});
    CHECK(default_timeout.valid && default_timeout.tune_timeout_ms == 10000U);
    const Px4TsArguments named = parse(
        {"px4-ts", "--instance", "receiver.left", "--receiver", "0",
         "--system", "isdb-t", "--frequency-khz", "40000"});
    CHECK(named.valid && named.instance == "receiver.left" && named.device.empty());
    CHECK(!parse({"px4-ts", "--device", "00001205000960", "--instance",
                  "receiver.left", "--receiver", "0", "--system", "isdb-t",
                  "--frequency-khz", "40000"}).valid);
    CHECK(!parse({"px4-ts", "--instance", "../bad", "--receiver", "0",
                  "--system", "isdb-t", "--frequency-khz", "40000"}).valid);
    CHECK(!parse({"px4-ts", "--instance", "..", "--receiver", "0",
                  "--system", "isdb-t", "--frequency-khz", "40000"}).valid);
    CHECK(!parse({"px4-ts", "--instance", "999999999999999", "--receiver", "0",
                  "--system", "isdb-t", "--frequency-khz", "40000"}).valid);
    CHECK(parse({"px4-ts", "--device", "00001205000960", "--receiver", "7",
                 "--system", "isdb-s", "--frequency-khz", "146875", "--slot", "0"}).valid);
    CHECK(parse({"px4-ts", "--device", "00001205000960", "--receiver", "0",
                 "--system", "isdb-t", "--frequency-khz", "40000"}).valid);
    for (unsigned int receiver = 0U; receiver < 8U; ++receiver) {
        const std::string value = std::to_string(receiver);
        CHECK(parse({"px4-ts", "--device", "00001205000960", "--receiver", value.c_str(),
                     "--system", "isdb-t", "--frequency-khz", "1002000"}).valid);
    }
    CHECK(!parse({"px4-ts", "--device", "00001205000960", "--receiver", "8",
                  "--system", "isdb-t", "--frequency-khz", "40000"}).valid);
    CHECK(!parse({"px4-ts", "--device", "00001205000960", "--receiver", "0",
                  "--system", "isdb-t", "--frequency-khz", "40000", "--duration-seconds", "1",
                  "--packet-count", "1"}).valid);
    CHECK(!parse({"px4-ts", "--device", "00001205000960", "--receiver", "0",
                  "--system", "isdb-s", "--frequency-khz", "146875", "--slot", "1",
                  "--stream-id", "2"}).valid);
    CHECK(!parse({"px4-ts", "--device", "00001205000960", "--receiver", "0",
                  "--system", "isdb-t", "--frequency-khz", "40000", "--receiver", "1"}).valid);
    CHECK(!parse({"px4-ts", "--device", "00001205000960", "--receiver", "0",
                  "--system", "isdb-t", "--frequency-khz", "40000", "--tune-timeout-ms", "99"}).valid);
    CHECK(!parse({"px4-ts", "--device", "00001205000960", "--receiver", "0",
                  "--system", "isdb-t", "--frequency-khz", "40000", "--tune-timeout-ms", "30001"}).valid);
    const std::string maximum_duration = std::to_string(
        std::numeric_limits<std::uint64_t>::max() / 1000U);
    CHECK(parse({"px4-ts", "--device", "00001205000960", "--receiver", "0",
                 "--system", "isdb-t", "--frequency-khz", "40000",
                 "--duration-seconds", maximum_duration.c_str()}).valid);
    const std::string overflowing_duration = std::to_string(
        std::numeric_limits<std::uint64_t>::max() / 1000U + 1U);
    CHECK(!parse({"px4-ts", "--device", "00001205000960", "--receiver", "0",
                  "--system", "isdb-t", "--frequency-khz", "40000",
                  "--duration-seconds", overflowing_duration.c_str()}).valid);
    CHECK(!parse({"px4-ts", "--device", "00001205000960", "--receiver", "0",
                  "--system", "isdb-t", "--frequency-khz", "40000", "--lnb-voltage", "15"}).valid);
    CHECK(parse({"px4-ts", "--device", "00001205000960", "--receiver", "0",
                 "--system", "isdb-t", "--frequency-khz", "40000", "--group",
                 "--output", "capture.ts"}).group);
    return true;
}

bool test_channel_conversion()
{
    for (unsigned int number = 13U; number <= 62U; ++number) {
        char text[4]{};
        std::snprintf(text, sizeof(text), "T%02u", number);
        const Px4TsChannel labelled = parse_px4_ts_channel(text);
        CHECK(labelled.valid && labelled.system == System::ISDB_T &&
              labelled.kind == Px4TsChannelKind::terrestrial &&
              labelled.slot == 0xffffU &&
              labelled.frequency_khz ==
                  395142U + static_cast<std::uint64_t>(number) * 6000U);
        const Px4TsChannel bare = parse_px4_ts_channel(std::string_view(text).substr(1U));
        CHECK(bare.valid && bare.system == System::ISDB_T &&
              bare.frequency_khz == labelled.frequency_khz);
    }
    for (unsigned int number = 1U; number <= 23U; number += 2U) {
        char transponder[8]{};
        std::snprintf(transponder, sizeof(transponder), "BS%02u", number);
        const Px4TsChannel without_slot = parse_px4_ts_channel(transponder);
        CHECK(without_slot.valid && without_slot.system == System::ISDB_S &&
              without_slot.kind == Px4TsChannelKind::satellite_transponder &&
              without_slot.slot == 0xffffU &&
              without_slot.frequency_khz ==
                  1049480U + ((number - 1U) / 2U) * 38360U);
        for (unsigned int slot = 0U; slot <= 11U; slot += 11U) {
            char text[16]{};
            std::snprintf(text, sizeof(text), "BS%02u_%u", number, slot);
            const Px4TsChannel with_slot = parse_px4_ts_channel(text);
            CHECK(with_slot.valid && with_slot.system == System::ISDB_S &&
                  with_slot.kind == Px4TsChannelKind::satellite_slot &&
                  with_slot.slot == slot &&
                  with_slot.frequency_khz == without_slot.frequency_khz);
        }
    }
    for (unsigned int number = 2U; number <= 24U; number += 2U) {
        char text[8]{};
        std::snprintf(text, sizeof(text), "CS%u", number);
        const Px4TsChannel cs = parse_px4_ts_channel(text);
        CHECK(cs.valid && cs.system == System::ISDB_S &&
              cs.kind == Px4TsChannelKind::satellite_cs && cs.slot == 0U &&
              cs.frequency_khz == 1613000U + ((number - 2U) / 2U) * 40000U);
    }
    CHECK(parse_px4_ts_channel("T27").frequency_khz == 557142U);
    CHECK(parse_px4_ts_channel("27").frequency_khz == 557142U);
    CHECK(parse_px4_ts_channel("BS01_0").frequency_khz == 1049480U &&
          parse_px4_ts_channel("BS01_0").slot == 0U);
    CHECK(parse_px4_ts_channel("BS23_3").frequency_khz == 1471440U &&
          parse_px4_ts_channel("BS23_3").slot == 3U);
    CHECK(parse_px4_ts_channel("CS2").frequency_khz == 1613000U);
    CHECK(parse_px4_ts_channel("CS24").frequency_khz == 2053000U);
    const char* rejected[] = {
        "T12", "T63", "12", "63", "T7", "t27", "T027", " T27", "T27 ",
        "BS02_0", "BS25_0", "BS1_0", "BS01_12", "BS01_00", "BS01_",
        "BS01_0_1", "CS1", "CS3", "CS26", "CS02", "CS", "BS", ""};
    for (const char* text : rejected) CHECK(!parse_px4_ts_channel(text).valid);
    return true;
}

bool test_channel_argument_contract()
{
    CHECK(!parse({"px4-ts", "--device", "00001205000960", "--receiver", "0",
                  "--system", "isdb-t", "--channel", "T27"}).valid);
    CHECK(!parse({"px4-ts", "--device", "00001205000960", "--receiver", "0",
                  "--frequency-khz", "40000", "--channel", "T27"}).valid);
    CHECK(!parse({"px4-ts", "--device", "00001205000960", "--receiver", "0",
                  "--channel", "T12"}).valid);
    const Px4TsArguments terrestrial = parse(
        {"px4-ts", "--device", "00001205000960", "--receiver", "0", "--channel", "T27"});
    CHECK(terrestrial.valid && terrestrial.system == System::ISDB_T &&
          terrestrial.frequency_khz == 557142U);
    CHECK(parse({"px4-ts", "--device", "00001205000960", "--receiver", "0",
                 "--channel", "27"}).valid);
    CHECK(!parse({"px4-ts", "--device", "00001205000960", "--receiver", "0",
                  "--channel", "T27", "--slot", "0"}).valid);
    CHECK(!parse({"px4-ts", "--device", "00001205000960", "--receiver", "0",
                  "--channel", "T27", "--stream-id", "1"}).valid);
    const Px4TsArguments bs_slot = parse(
        {"px4-ts", "--device", "00001205000960", "--receiver", "0", "--channel", "BS01_0"});
    CHECK(bs_slot.valid && bs_slot.system == System::ISDB_S && bs_slot.slot == 0U &&
          bs_slot.frequency_khz == 1049480U);
    CHECK(!parse({"px4-ts", "--device", "00001205000960", "--receiver", "0",
                  "--channel", "BS01_0", "--slot", "5"}).valid);
    CHECK(!parse({"px4-ts", "--device", "00001205000960", "--receiver", "0",
                  "--channel", "BS01_0", "--stream-id", "5"}).valid);
    CHECK(!parse({"px4-ts", "--device", "00001205000960", "--receiver", "0",
                  "--channel", "BS01"}).valid);
    const Px4TsArguments bs_stream = parse({"px4-ts", "--device", "00001205000960",
        "--receiver", "0", "--channel", "BS01", "--stream-id", "42"});
    CHECK(bs_stream.valid && bs_stream.system == System::ISDB_S &&
          bs_stream.stream_id == 42U && bs_stream.slot == 0xffffU);
    const Px4TsArguments bs_explicit_slot = parse({"px4-ts", "--device", "00001205000960",
        "--receiver", "0", "--channel", "BS01", "--slot", "3"});
    CHECK(bs_explicit_slot.valid && bs_explicit_slot.slot == 3U);
    const Px4TsArguments cs = parse(
        {"px4-ts", "--device", "00001205000960", "--receiver", "0", "--channel", "CS2"});
    CHECK(cs.valid && cs.system == System::ISDB_S && cs.slot == 0U &&
          cs.frequency_khz == 1613000U);
    const Px4TsArguments cs_slot = parse({"px4-ts", "--device", "00001205000960",
        "--receiver", "0", "--channel", "CS2", "--slot", "5"});
    CHECK(cs_slot.valid && cs_slot.slot == 5U);
    CHECK(terrestrial_arguments().valid);
    CHECK(parse({"px4-ts", "--device", "00001205000960", "--receiver", "7",
                 "--system", "isdb-s", "--frequency-khz", "146875", "--slot", "0"}).valid);
    return true;
}

bool test_runner_packets_and_satellite()
{
    CHECK(run_fake(FakeMode::normal, terrestrial_arguments(), Error::OK, 188U));
    CHECK(run_fake(FakeMode::normal_empty_intervals, terrestrial_arguments(), Error::OK, 188U));
    Px4TsArguments satellite = parse({"px4-ts", "--device", "00001205000960", "--receiver", "4",
        "--system", "isdb-s", "--frequency-khz", "146875", "--slot", "3",
        "--packet-count", "1"});
    CHECK(run_fake(FakeMode::normal, satellite, Error::OK, 188U, true));
    Px4TsArguments satellite_stream = parse({"px4-ts", "--device", "00001205000960",
        "--receiver", "5", "--system", "isdb-s", "--frequency-khz", "2350000",
        "--stream-id", "42", "--lnb-voltage", "15", "--tune-timeout-ms", "1234",
        "--packet-count", "1"});
    CHECK(run_fake(FakeMode::normal, satellite_stream, Error::OK, 188U, true));
    return true;
}

bool test_runner_protocol_and_cleanup_errors()
{
    CHECK(run_fake(FakeMode::initial_stall, terrestrial_arguments(false),
                   Error::TIMEOUT, 0U, false,
                   Px4TsFailureKind::ts_integrity, 8));
    CHECK(run_fake(FakeMode::mid_stall, terrestrial_arguments(false),
                   Error::TIMEOUT, 376U, false,
                   Px4TsFailureKind::ts_integrity, 8));
    CHECK(run_fake(FakeMode::duration_stall_tie, terrestrial_arguments(false),
                   Error::OK, 376U));
    CHECK(run_fake(FakeMode::sequence_gap, terrestrial_arguments(), Error::PROTOCOL_ERROR,
                   0U, false, Px4TsFailureKind::ts_integrity, 8));
    CHECK(run_fake(FakeMode::bad_sync, terrestrial_arguments(), Error::PROTOCOL_ERROR,
                   0U, false, Px4TsFailureKind::ts_integrity, 8));
    CHECK(run_fake(FakeMode::bad_alignment, terrestrial_arguments(), Error::PROTOCOL_ERROR,
                   0U, false, Px4TsFailureKind::ts_integrity, 8));
    CHECK(run_fake(FakeMode::drops, terrestrial_arguments(), Error::SLOW_CONSUMER,
                   0U, false, Px4TsFailureKind::ts_integrity, 8));
    CHECK(run_fake(FakeMode::terminal_usb, terrestrial_arguments(), Error::USB_IO, 188U));
    CHECK(run_fake(FakeMode::terminal_slow, terrestrial_arguments(), Error::SLOW_CONSUMER, 188U));
    CHECK(run_fake(FakeMode::terminal_disconnected, terrestrial_arguments(), Error::DISCONNECTED, 188U));
    CHECK(run_fake(FakeMode::terminal_sync, terrestrial_arguments(), Error::PROTOCOL_ERROR,
                   188U, false, Px4TsFailureKind::ts_integrity, 8));
    CHECK(run_fake(FakeMode::terminal_fatal, terrestrial_arguments(), Error::INTERNAL, 188U));
    CHECK(run_fake(FakeMode::bad_final, terrestrial_arguments(), Error::PROTOCOL_ERROR,
                   188U, false, Px4TsFailureKind::ipc, 6));
    CHECK(run_fake(FakeMode::duplicate_attach, terrestrial_arguments(), Error::PROTOCOL_ERROR,
                   188U, false, Px4TsFailureKind::ipc, 6));
    CHECK(run_fake(FakeMode::disconnect_after_tune, terrestrial_arguments(), Error::DISCONNECTED, 0U));
    CHECK(run_fake(FakeMode::stream_connect_failure, terrestrial_arguments(), Error::NOT_FOUND, 0U));
    CHECK(run_fake(FakeMode::disconnect_after_start, terrestrial_arguments(), Error::NOT_FOUND, 0U));
    CHECK(run_fake(FakeMode::attach_response_failure, terrestrial_arguments(), Error::PROTOCOL_ERROR,
                   0U, false, Px4TsFailureKind::ipc, 6));
    CHECK(run_fake(FakeMode::data_read_failure, terrestrial_arguments(), Error::DISCONNECTED, 188U));
    CHECK(run_fake(FakeMode::counter_sync, terrestrial_arguments(), Error::PROTOCOL_ERROR,
                   188U, false, Px4TsFailureKind::ts_integrity, 8));
    CHECK(run_fake(FakeMode::counter_tei, terrestrial_arguments(), Error::PROTOCOL_ERROR,
                   188U, false, Px4TsFailureKind::ts_integrity, 8));
    CHECK(run_fake(FakeMode::counter_continuity, terrestrial_arguments(), Error::PROTOCOL_ERROR,
                   188U, false, Px4TsFailureKind::ts_integrity, 8));
    CHECK(run_fake(FakeMode::counter_queue, terrestrial_arguments(), Error::PROTOCOL_ERROR,
                   188U, false, Px4TsFailureKind::ts_integrity, 8));
    CHECK(run_fake(FakeMode::counter_usb, terrestrial_arguments(), Error::PROTOCOL_ERROR,
                   188U, false, Px4TsFailureKind::ts_integrity, 8));
    CHECK(px4_ts_exit_status(Error::PROTOCOL_ERROR) == 6);
    return true;
}

bool test_runner_duration_signal_and_output()
{
    CHECK(run_fake(FakeMode::duration_wait, terrestrial_arguments(false), Error::OK, 376U));
    TempRuntime signal_runtime;
    CHECK(signal_runtime.valid());
    FakeDaemon signal_daemon(signal_runtime.path(), FakeMode::normal);
    CHECK(signal_daemon.start());
    Px4TsArguments signal_arguments = parse({"px4-ts", "--device", "00001205000960",
        "--receiver", "0", "--system", "isdb-t", "--frequency-khz", "40000"});
    signal_arguments.runtime_directory = signal_runtime.path();
    CaptureOutput signal_output;
    TestClock signal_clock;
    TestSignal signal_source;
    signal_source.true_after_checks = 2U;
    const auto signal_result = Px4TsRunner::run(signal_arguments, signal_output,
                                                 signal_clock, signal_source);
    signal_daemon.join_threads();
    if (!(signal_result && signal_result.value().signal_stopped &&
          signal_output.data.size() == 376U)) {
        const unsigned int error = signal_result ? 0U : static_cast<unsigned int>(signal_result.error());
        std::fprintf(stderr,
                     "signal_run invariant=signal_stop result=%u signal=%u output=%zu checks=%u "
                     "control={started:%u accepted:%u done:%u} stream={started:%u accepted:%u "
                     "done:%u data:%u waiting:%u end:%u,%u joined:%u} stop=%zu response:%u release=%zu "
                     "shutdown:%u released:%u end_write_error:%u end_frame_size:%zu "
                     "header_error:%u preflight_error:%u frame_error:%u\n",
                     error, signal_result ? static_cast<unsigned int>(signal_result.value().signal_stopped) : 0U,
                     signal_output.data.size(), signal_source.checks,
                     static_cast<unsigned int>(signal_daemon.control_started.load()),
                     static_cast<unsigned int>(signal_daemon.control_accepted.load()),
                     static_cast<unsigned int>(signal_daemon.control_done.load()),
                     static_cast<unsigned int>(signal_daemon.stream_started.load()),
                     static_cast<unsigned int>(signal_daemon.stream_accepted.load()),
                     static_cast<unsigned int>(signal_daemon.stream_done.load()),
                     static_cast<unsigned int>(signal_daemon.stream_data_sent.load()),
                     static_cast<unsigned int>(signal_daemon.stream_waiting.load()),
                     static_cast<unsigned int>(signal_daemon.stream_end_attempted.load()),
                     static_cast<unsigned int>(signal_daemon.stream_end_sent.load()),
                     static_cast<unsigned int>(signal_daemon.threads_joined.load()),
                     signal_daemon.stop_requests.load(),
                     static_cast<unsigned int>(signal_daemon.stop_response_sent.load()),
                     signal_daemon.release_requests.load(),
                     static_cast<unsigned int>(signal_daemon.shutdown_requested.load()),
                     static_cast<unsigned int>(signal_daemon.released.load()),
                     signal_daemon.stream_end_write_error.load(),
                     signal_daemon.stream_end_frame_size.load(),
                     signal_daemon.stream_end_header_error.load(),
                     signal_daemon.stream_end_preflight_error.load(),
                     signal_daemon.stream_end_payload_error.load());
        return false;
    }
    CHECK(signal_daemon.stop_requests == 1U && signal_daemon.release_requests == 1U);
    TempRuntime runtime;
    CHECK(runtime.valid());
    FakeDaemon daemon(runtime.path(), FakeMode::normal);
    CHECK(daemon.start());
    Px4TsArguments output_arguments = terrestrial_arguments();
    output_arguments.runtime_directory = runtime.path();
    CaptureOutput output;
    output.fail = true;
    TestClock clock;
    TestSignal signal;
    const auto result = Px4TsRunner::run(output_arguments, output, clock, signal);
    CHECK(!result && result.error() == Error::INTERNAL);

    TempRuntime partial_runtime;
    CHECK(partial_runtime.valid());
    FakeDaemon partial_daemon(partial_runtime.path(), FakeMode::normal);
    CHECK(partial_daemon.start());
    output_arguments.runtime_directory = partial_runtime.path();
    PartialOutput partial_output;
    TestClock partial_clock;
    TestSignal partial_signal;
    const auto partial_result = Px4TsRunner::run(
        output_arguments, partial_output, partial_clock, partial_signal);
    CHECK(!partial_result && partial_result.error() == Error::INTERNAL);
    CHECK(partial_output.written == 1U && partial_daemon.stop_requests == 1U &&
          partial_daemon.release_requests == 1U);

    TempRuntime file_runtime;
    CHECK(file_runtime.valid());
    FakeDaemon file_daemon(file_runtime.path(), FakeMode::normal);
    CHECK(file_daemon.start());
    const std::string file_path = file_runtime.path() + "/capture.ts";
    const int file_fd = ::open(file_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    CHECK(file_fd >= 0);
    Px4TsFdOutput file_output(file_fd, true);
    CHECK(file_output.ready());
    output_arguments.runtime_directory = file_runtime.path();
    TestClock file_clock;
    TestSignal file_signal;
    const auto file_result = Px4TsRunner::run(
        output_arguments, file_output, file_clock, file_signal);
    file_daemon.join_threads();
    if (!file_result) {
        std::fprintf(stderr,
                     "file_run invariant=file_result result=%u checks=%u "
                     "control={started:%u accepted:%u done:%u} stream={started:%u accepted:%u "
                     "done:%u data:%u waiting:%u end:%u,%u joined:%u} stop=%zu response:%u release=%zu "
                     "shutdown:%u released:%u end_write_error:%u frame_size:%zu header:%u preflight:%u frame:%u\n",
                     static_cast<unsigned int>(file_result.error()), file_signal.checks,
                     static_cast<unsigned int>(file_daemon.control_started.load()),
                     static_cast<unsigned int>(file_daemon.control_accepted.load()),
                     static_cast<unsigned int>(file_daemon.control_done.load()),
                     static_cast<unsigned int>(file_daemon.stream_started.load()),
                     static_cast<unsigned int>(file_daemon.stream_accepted.load()),
                     static_cast<unsigned int>(file_daemon.stream_done.load()),
                     static_cast<unsigned int>(file_daemon.stream_data_sent.load()),
                     static_cast<unsigned int>(file_daemon.stream_waiting.load()),
                     static_cast<unsigned int>(file_daemon.stream_end_attempted.load()),
                     static_cast<unsigned int>(file_daemon.stream_end_sent.load()),
                     static_cast<unsigned int>(file_daemon.threads_joined.load()),
                     file_daemon.stop_requests.load(),
                     static_cast<unsigned int>(file_daemon.stop_response_sent.load()),
                     file_daemon.release_requests.load(),
                     static_cast<unsigned int>(file_daemon.shutdown_requested.load()),
                     static_cast<unsigned int>(file_daemon.released.load()),
                     file_daemon.stream_end_write_error.load(),
                     file_daemon.stream_end_frame_size.load(),
                     file_daemon.stream_end_header_error.load(),
                     file_daemon.stream_end_preflight_error.load(),
                     file_daemon.stream_end_payload_error.load());
        return false;
    }
    std::ifstream capture(file_path, std::ios::binary);
    std::vector<std::uint8_t> file_bytes((std::istreambuf_iterator<char>(capture)),
                                         std::istreambuf_iterator<char>());
    CHECK(file_bytes.size() == 188U && file_bytes[0] == 0x47U);

    TempRuntime signal_file_runtime;
    CHECK(signal_file_runtime.valid());
    FakeDaemon signal_file_daemon(signal_file_runtime.path(), FakeMode::normal);
    CHECK(signal_file_daemon.start());
    const std::string signal_file_path = signal_file_runtime.path() + "/signal.ts";
    const int signal_file_fd = ::open(signal_file_path.c_str(),
                                      O_WRONLY | O_CREAT | O_TRUNC, 0600);
    CHECK(signal_file_fd >= 0);
    Px4TsFdOutput signal_file_output(signal_file_fd, true);
    CHECK(signal_file_output.ready());
    Px4TsArguments signal_file_arguments = terrestrial_arguments(false);
    signal_file_arguments.runtime_directory = signal_file_runtime.path();
    TestClock signal_file_clock;
    TestSignal signal_file_signal;
    signal_file_signal.true_after_checks = 2U;
    const auto signal_file_result = Px4TsRunner::run(
        signal_file_arguments, signal_file_output, signal_file_clock,
        signal_file_signal);
    signal_file_daemon.join_threads();
    if (!(signal_file_result && signal_file_result.value().signal_stopped)) {
        const unsigned int error = signal_file_result ? 0U :
            static_cast<unsigned int>(signal_file_result.error());
        std::fprintf(stderr,
                     "signal_run invariant=file_signal_stop result=%u signal=%u checks=%u "
                     "control={started:%u accepted:%u done:%u} stream={started:%u accepted:%u "
                     "done:%u data:%u waiting:%u end:%u,%u joined:%u} stop=%zu response:%u release=%zu "
                     "shutdown:%u released:%u end_write_error:%u end_frame_size:%zu "
                     "header_error:%u preflight_error:%u frame_error:%u\n",
                     error,
                     signal_file_result ? static_cast<unsigned int>(signal_file_result.value().signal_stopped) : 0U,
                     signal_file_signal.checks,
                     static_cast<unsigned int>(signal_file_daemon.control_started.load()),
                     static_cast<unsigned int>(signal_file_daemon.control_accepted.load()),
                     static_cast<unsigned int>(signal_file_daemon.control_done.load()),
                     static_cast<unsigned int>(signal_file_daemon.stream_started.load()),
                     static_cast<unsigned int>(signal_file_daemon.stream_accepted.load()),
                     static_cast<unsigned int>(signal_file_daemon.stream_done.load()),
                     static_cast<unsigned int>(signal_file_daemon.stream_data_sent.load()),
                     static_cast<unsigned int>(signal_file_daemon.stream_waiting.load()),
                     static_cast<unsigned int>(signal_file_daemon.stream_end_attempted.load()),
                     static_cast<unsigned int>(signal_file_daemon.stream_end_sent.load()),
                     static_cast<unsigned int>(signal_file_daemon.threads_joined.load()),
                     signal_file_daemon.stop_requests.load(),
                     static_cast<unsigned int>(signal_file_daemon.stop_response_sent.load()),
                     signal_file_daemon.release_requests.load(),
                     static_cast<unsigned int>(signal_file_daemon.shutdown_requested.load()),
                     static_cast<unsigned int>(signal_file_daemon.released.load()),
                     signal_file_daemon.stream_end_write_error.load(),
                     signal_file_daemon.stream_end_frame_size.load(),
                     signal_file_daemon.stream_end_header_error.load(),
                     signal_file_daemon.stream_end_preflight_error.load(),
                     signal_file_daemon.stream_end_payload_error.load());
        return false;
    }
    CHECK(signal_file_daemon.stop_requests == 1U &&
          signal_file_daemon.release_requests == 1U);
    std::ifstream signal_capture(signal_file_path, std::ios::binary);
    CHECK(signal_capture.peek() == std::ifstream::traits_type::eof());

    int pipe_fds[2] = {-1, -1};
    CHECK(::pipe(pipe_fds) == 0);
    const int pipe_flags = ::fcntl(pipe_fds[1], F_GETFL);
    CHECK(pipe_flags >= 0 && ::fcntl(pipe_fds[1], F_SETFL, pipe_flags | O_NONBLOCK) == 0);
    std::array<std::uint8_t, 4096U> fill{};
    while (::write(pipe_fds[1], fill.data(), fill.size()) > 0) {}
    Px4TsFdOutput blocked_output(pipe_fds[1], true);
    TestSignal blocked_signal;
    blocked_signal.true_after_checks = 2U;
    std::array<std::uint8_t, 188U> packet{};
    packet[0] = 0x47U;
    const auto blocked_result = blocked_output.write(
        ByteView{packet.data(), packet.size()}, blocked_signal);
    CHECK(!blocked_result && blocked_result.error() == Error::TIMEOUT);
    (void)::close(pipe_fds[0]);

    IgnoreSigpipe ignore_sigpipe;
    CHECK(ignore_sigpipe.installed());
    int closed_pipe[2] = {-1, -1};
    CHECK(::pipe(closed_pipe) == 0);
    (void)::close(closed_pipe[0]);
    Px4TsFdOutput closed_output(closed_pipe[1], true);
    CHECK(closed_output.ready());
    TestSignal no_stop_signal;
    const auto epipe_result = closed_output.write(
        ByteView{packet.data(), packet.size()}, no_stop_signal);
    CHECK(!epipe_result && epipe_result.error() == Error::INTERNAL);
    return true;
}

}  // namespace

bool run_px4_ts_tests()
{
    return test_argument_contract() && test_channel_conversion() &&
           test_channel_argument_contract() && test_runner_packets_and_satellite() &&
           test_runner_protocol_and_cleanup_errors() && test_runner_duration_signal_and_output();
}
