// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef ASICEN_USERLAND_HARDWARE_STREAM_SESSION_H
#define ASICEN_USERLAND_HARDWARE_STREAM_SESSION_H

#include "px4/tuner_service.h"
#include "px4/transport.h"
#include "asicen/capture_drain.h"
#include "asicen/native_thread.h"
#include "asicen/ts_framer.h"

#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <memory>
#include <thread>
#include <vector>

namespace asicen {

using StreamCaptureFramingCounters = TsFramer::Counters;

// A source owns the USB callback/URB storage. run() may emit only complete,
// transformed 188-byte packets. interrupt() must wake a blocked run() without
// destroying callback storage; stop() performs hardware cleanup after run()
// has returned and all callback ownership has been joined.
class StreamCaptureSource {
public:
    virtual ~StreamCaptureSource() noexcept = default;
    virtual px4::userland::Result<void> prepare(std::uint8_t receiver,
                                                px4::userland::ipc::System system,
                                                const std::atomic<bool>& cancelled) noexcept = 0;
    virtual CaptureRunResult run(const std::atomic<bool>& cancelled,
                                 bool (*emit)(void*, const std::uint8_t*, std::size_t),
                                 void* context) noexcept = 0;
    virtual void interrupt() noexcept = 0;
    virtual px4::userland::Result<void> stop() noexcept = 0;
    virtual px4::userland::TunerStreamCounters source_counters() const noexcept { return {}; }
    // Inspect only after run() has returned; this is parser-owner state.
    virtual StreamCaptureFramingCounters source_framing_counters() const noexcept { return {}; }
};

class HardwareStreamService final : public px4::userland::TunerServiceBackend,
                                    public px4::userland::TunerStreamControl {
public:
    static constexpr std::size_t kQueueCapacityBytes = 65536U * 188U;
    static constexpr std::size_t kDefaultQueuePackets = 65536U;
    static constexpr std::size_t kMinQueuePackets = 4096U;
    static constexpr std::size_t kMaxQueuePackets = 262144U;
    static constexpr std::size_t kMaxReadBytes = (px4::userland::kMaxStreamTransfer / 188U) * 188U;

    HardwareStreamService(px4::userland::TunerServiceBackend& frontend, StreamCaptureSource& source,

                          std::size_t queue_packets = kDefaultQueuePackets,
                          NativeThread::Start thread_launcher = nullptr);
    struct TestQueueCapacityBytes {
        std::size_t bytes;
    };
    // Narrow offline fault-injection seam; product callers use packet counts.
    HardwareStreamService(px4::userland::TunerServiceBackend& frontend, StreamCaptureSource& source,
                          TestQueueCapacityBytes queue_capacity,
                          NativeThread::Start thread_launcher = nullptr);
    ~HardwareStreamService() noexcept override;
    HardwareStreamService(const HardwareStreamService&) = delete;
    HardwareStreamService& operator=(const HardwareStreamService&) = delete;

    std::uint8_t receiver_count() const noexcept override;
    bool receiver_supports(std::uint8_t receiver,
                           px4::userland::ipc::System system) const noexcept override;
    bool selects_satellite_stream_before_tune() const noexcept override;
    bool requires_terrestrial_lock_settle() const noexcept override;
    px4::userland::Result<void> open_receiver(std::uint8_t receiver) noexcept override;
    px4::userland::Result<void> tune_terrestrial(std::uint8_t receiver, std::uint32_t frequency_khz,
                                                 std::uint32_t timeout_ms) noexcept override;
    px4::userland::Result<void> tune_satellite(std::uint8_t receiver, std::uint32_t frequency_khz,
                                               std::uint32_t timeout_ms) noexcept override;
    px4::userland::Result<bool> is_locked(std::uint8_t receiver,
                                          px4::userland::ipc::System system) noexcept override;
    px4::userland::Result<void> select_satellite_slot(std::uint8_t receiver, std::uint8_t slot,
                                                      std::uint32_t timeout_ms) noexcept override;
    px4::userland::Result<void> select_satellite_tsid(std::uint8_t receiver, std::uint16_t tsid,
                                                      std::uint32_t timeout_ms) noexcept override;
    px4::userland::Result<void> close_receiver(std::uint8_t receiver) noexcept override;
    px4::userland::Result<void> begin_tune_power(std::uint8_t receiver,
                                                 px4::userland::ipc::System system,
                                                 std::uint8_t lnb_voltage) noexcept override;
    px4::userland::Result<void> commit_tune_power(std::uint8_t receiver) noexcept override;
    px4::userland::Result<void> rollback_tune_power(std::uint8_t receiver) noexcept override;
    void mark_receiver_disconnected(std::uint8_t receiver) noexcept override;
    void request_stop() noexcept override;
    px4::userland::Result<void> shutdown() noexcept override;
    px4::userland::Result<void> start_capture(std::uint8_t receiver,
                                              px4::userland::ipc::System system) noexcept override;
    px4::userland::Result<void> stop_capture(std::uint8_t receiver,
                                             px4::userland::ipc::System system) noexcept override;

    px4::userland::Result<void>
    attach(const px4::userland::TunerAttachment& attachment) noexcept override;
    px4::userland::Result<void>
    detach(const px4::userland::TunerAttachment& attachment) noexcept override;
    px4::userland::Result<px4::userland::TunerStreamCounters>
    stats(const px4::userland::TunerAttachment& attachment) const noexcept override;
    px4::userland::Result<px4::userland::TunerStreamFinalSnapshot>
    final_snapshot(const px4::userland::TunerAttachment& attachment) const noexcept override;
    px4::userland::Result<void>
    release_final(const px4::userland::TunerAttachment& attachment) noexcept override;
    px4::userland::Result<px4::userland::TunerStreamReadResult>
    read(const px4::userland::TunerAttachment& attachment, px4::userland::MutableByteView output,
         px4::userland::Timeout timeout) noexcept override;
    px4::userland::Result<px4::userland::TunerStreamTerminal>
    terminal(const px4::userland::TunerAttachment& attachment) const noexcept override;

private:
    enum class State : std::uint8_t { idle, preparing, running, detached, quarantined };
    static bool emit_trampoline(void* context, const std::uint8_t* bytes,
                                std::size_t size) noexcept;
    bool emit(const std::uint8_t* bytes, std::size_t size) noexcept;
    void run_worker() noexcept;
    bool matches_locked(const px4::userland::TunerAttachment& attachment) const noexcept;
    bool matches_final_locked(const px4::userland::TunerAttachment& attachment) const noexcept;
    void set_terminal_locked(px4::userland::TunerStreamTerminal terminal) noexcept;
    void clear_attempt_locked() noexcept;
    void cancel_and_join() noexcept;
    px4::userland::TunerStreamCounters counters_locked() const noexcept;
    void observe_packet_locked(const std::uint8_t* packet) noexcept;

    px4::userland::TunerServiceBackend& frontend_;
    StreamCaptureSource& source_;
    const std::size_t queue_capacity_bytes_;
    mutable std::mutex mutex_;
    std::mutex lifecycle_mutex_;
    std::condition_variable condition_;
    std::unique_ptr<std::uint8_t[]> queue_;
    std::size_t queue_head_ = 0U;
    std::size_t queue_bytes_ = 0U;
    struct Continuity {
        bool seen = false;
        std::uint8_t counter = 0U;
    };
    std::array<Continuity, 8192U> continuity_{};
    px4::userland::TunerStreamCounters counters_{};
    px4::userland::TunerAttachment attachment_{};
    px4::userland::TunerAttachment final_attachment_{};
    px4::userland::TunerStreamFinalSnapshot final_{};
    std::atomic<bool> cancelled_{false};
    std::atomic<bool> shutdown_requested_{false};
    State state_ = State::idle;
    bool have_attachment_ = false;
    bool have_final_attachment_ = false;
    bool draining_ = false;
    bool final_snapshot_ready_ = false;
    bool worker_done_ = false;
    bool source_prepared_ = false;
    bool source_stopped_ = true;
    px4::userland::TunerStreamTerminal terminal_ = px4::userland::TunerStreamTerminal::none;
    NativeThread worker_;
    NativeThread::Start thread_launcher_;
};

}  // namespace asicen

#endif  // ASICEN_USERLAND_HARDWARE_STREAM_SESSION_H
