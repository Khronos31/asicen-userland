// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "px4/tuner_service.h"
#include "px4/transport.h"
#include "asicen/capture_drain.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace asicen {

// A source owns the USB callback/URB storage. run() may emit only complete,
// transformed 188-byte packets. interrupt() must wake a blocked run() without
// destroying callback storage; stop() performs hardware cleanup after run()
// has returned and all callback ownership has been joined.
class StreamCaptureSource {
public:
    virtual ~StreamCaptureSource() noexcept = default;
    virtual px4::userland::Result<void> prepare(
        std::uint8_t receiver, px4::userland::ipc::System system,
        const std::atomic<bool>& cancelled) noexcept = 0;
    virtual CaptureRunResult run(
        const std::atomic<bool>& cancelled,
        bool (*emit)(void*, const std::uint8_t*, std::size_t),
        void* context) noexcept = 0;
    virtual void interrupt() noexcept = 0;
    virtual px4::userland::Result<void> stop() noexcept = 0;
};

class StreamProcessFatal {
public:
    virtual ~StreamProcessFatal() noexcept = default;
    virtual void terminate_nonzero(int code) noexcept = 0;
};

class ExitProcessFatal final : public StreamProcessFatal {
public:
    void terminate_nonzero(int code) noexcept override;
};

class HardwareStreamService final : public px4::userland::TunerServiceBackend,
                                    public px4::userland::TunerStreamControl {
public:
    static constexpr std::size_t kQueueCapacityBytes = 2U * 1024U * 1024U;

    HardwareStreamService(px4::userland::TunerServiceBackend& frontend,
                          StreamCaptureSource& source,
                          StreamProcessFatal& fatal,
                          std::size_t queue_capacity_bytes = kQueueCapacityBytes);
    ~HardwareStreamService() noexcept override;
    HardwareStreamService(const HardwareStreamService&) = delete;
    HardwareStreamService& operator=(const HardwareStreamService&) = delete;

    std::uint8_t receiver_count() const noexcept override;
    bool receiver_supports(std::uint8_t receiver,
                          px4::userland::ipc::System system) const noexcept override;
    bool selects_satellite_stream_before_tune() const noexcept override;
    bool requires_terrestrial_lock_settle() const noexcept override;
    px4::userland::Result<void> open_receiver(std::uint8_t receiver) noexcept override;
    px4::userland::Result<void> tune_terrestrial(
        std::uint8_t receiver, std::uint32_t frequency_khz,
        std::uint32_t timeout_ms) noexcept override;
    px4::userland::Result<void> tune_satellite(
        std::uint8_t receiver, std::uint32_t frequency_khz,
        std::uint32_t timeout_ms) noexcept override;
    px4::userland::Result<bool> is_locked(
        std::uint8_t receiver, px4::userland::ipc::System system) noexcept override;
    px4::userland::Result<void> select_satellite_slot(
        std::uint8_t receiver, std::uint8_t slot,
        std::uint32_t timeout_ms) noexcept override;
    px4::userland::Result<void> select_satellite_tsid(
        std::uint8_t receiver, std::uint16_t tsid,
        std::uint32_t timeout_ms) noexcept override;
    px4::userland::Result<void> close_receiver(std::uint8_t receiver) noexcept override;
    px4::userland::Result<void> begin_tune_power(
        std::uint8_t receiver, px4::userland::ipc::System system,
        std::uint8_t lnb_voltage) noexcept override;
    px4::userland::Result<void> commit_tune_power(std::uint8_t receiver) noexcept override;
    px4::userland::Result<void> rollback_tune_power(std::uint8_t receiver) noexcept override;
    void mark_receiver_disconnected(std::uint8_t receiver) noexcept override;
    void request_stop() noexcept override;
    px4::userland::Result<void> shutdown() noexcept override;
    px4::userland::Result<void> start_capture(
        std::uint8_t receiver, px4::userland::ipc::System system) noexcept override;
    px4::userland::Result<void> stop_capture(
        std::uint8_t receiver, px4::userland::ipc::System system) noexcept override;

    px4::userland::Result<void> attach(
        const px4::userland::TunerAttachment& attachment) noexcept override;
    px4::userland::Result<void> detach(
        const px4::userland::TunerAttachment& attachment) noexcept override;
    px4::userland::Result<px4::userland::TunerStreamCounters> stats(
        const px4::userland::TunerAttachment& attachment) const noexcept override;
    px4::userland::Result<px4::userland::TunerStreamFinalSnapshot> final_snapshot(
        const px4::userland::TunerAttachment& attachment) const noexcept override;
    px4::userland::Result<void> release_final(
        const px4::userland::TunerAttachment& attachment) noexcept override;
    px4::userland::Result<px4::userland::TunerStreamReadResult> read(
        const px4::userland::TunerAttachment& attachment,
        px4::userland::MutableByteView output,
        px4::userland::Timeout timeout) noexcept override;
    px4::userland::Result<px4::userland::TunerStreamTerminal> terminal(
        const px4::userland::TunerAttachment& attachment) const noexcept override;

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

    px4::userland::TunerServiceBackend& frontend_;
    StreamCaptureSource& source_;
    StreamProcessFatal& fatal_;
    const std::size_t queue_capacity_bytes_;
    mutable std::mutex mutex_;
    std::mutex lifecycle_mutex_;
    std::condition_variable condition_;
    std::deque<std::vector<std::uint8_t>> queue_;
    std::size_t queue_bytes_ = 0U;
    px4::userland::TunerStreamCounters counters_{};
    px4::userland::TunerAttachment attachment_{};
    px4::userland::TunerAttachment final_attachment_{};
    px4::userland::TunerStreamFinalSnapshot final_{};
    std::atomic<bool> cancelled_{false};
    std::atomic<bool> shutdown_requested_{false};
    State state_ = State::idle;
    bool have_attachment_ = false;
    bool have_final_attachment_ = false;
    bool worker_done_ = false;
    bool source_prepared_ = false;
    bool source_stopped_ = true;
    px4::userland::TunerStreamTerminal terminal_ =
        px4::userland::TunerStreamTerminal::none;
    std::thread worker_;
};

}  // namespace asicen
