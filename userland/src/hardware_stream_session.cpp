// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/hardware_stream_session.h"
#include "px4/ts_continuity.h"

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <new>
#if !defined(_WIN32)
#include <sys/resource.h>
#endif
#include <utility>

namespace asicen {
namespace {
using px4::userland::Error;
using px4::userland::Result;
using px4::userland::TunerAttachment;
using px4::userland::TunerHzProgramming;
using px4::userland::TunerStrictProfile;
using px4::userland::TunerStreamTerminal;
using px4::userland::ipc::System;

bool same_attachment(const TunerAttachment& left,
                     const TunerAttachment& right) noexcept
{
    return left.owner_client_id == right.owner_client_id &&
           left.lease_id == right.lease_id &&
           left.attachment_id == right.attachment_id &&
           left.receiver == right.receiver && left.system == right.system &&
           left.nonce == right.nonce;
}

}  // namespace

HardwareStreamService::HardwareStreamService(px4::userland::TunerServiceBackend& frontend,
                                             StreamCaptureSource& source, std::size_t queue_packets,
                                             NativeThread::Start thread_launcher)
    : HardwareStreamService(frontend, source,
                            TestQueueCapacityBytes{queue_packets < kMinQueuePackets ||
                                                           queue_packets > kMaxQueuePackets
                                                       ? 0U
                                                       : queue_packets * 188U},
                            thread_launcher)
{
}

HardwareStreamService::HardwareStreamService(px4::userland::TunerServiceBackend& frontend,
                                             StreamCaptureSource& source,
                                             TestQueueCapacityBytes queue_capacity,
                                             NativeThread::Start thread_launcher)
    : frontend_(frontend), source_(source),
      queue_capacity_bytes_(queue_capacity.bytes > kMaxQueuePackets * 188U ||
                                    queue_capacity.bytes % 188U != 0U
                                ? 0U
                                : queue_capacity.bytes),
      thread_launcher_(thread_launcher)
{
}

HardwareStreamService::HardwareStreamService(px4::userland::TunerServiceBackend& frontend,
                                             StreamCaptureSource& source,
                                             TestQueueCapacityBytes queue_capacity,
                                             NativeThread::Start thread_launcher,
                                             std::uint8_t receiver, ReceiverSessionTag)
    : frontend_(frontend), source_(source),
      queue_capacity_bytes_(queue_capacity.bytes > kMaxQueuePackets * 188U ||
                                    queue_capacity.bytes % 188U != 0U
                                ? 0U
                                : queue_capacity.bytes),
      pinned_receiver_(receiver), dispatcher_(false),
      queue_(new (std::nothrow) std::uint8_t[queue_capacity_bytes_]),
      continuity_(new (std::nothrow) px4::userland::TsContinuityChecker),
      thread_launcher_(thread_launcher)
{
}

HardwareStreamService* HardwareStreamService::session_for(std::uint8_t receiver) noexcept
{
    if (!dispatcher_ || receiver >= receiver_count() || receiver >= receiver_sessions_.size()) {
        return nullptr;
    }
    auto& session = receiver_sessions_[receiver];
    if (!session) {
        session.reset(new (std::nothrow) HardwareStreamService(
            frontend_, source_, TestQueueCapacityBytes{queue_capacity_bytes_}, thread_launcher_,
            receiver, ReceiverSessionTag{}));
    }
    return session.get();
}

const HardwareStreamService* HardwareStreamService::session_for(std::uint8_t receiver) const noexcept
{
    return !dispatcher_ || receiver >= receiver_count() || receiver >= receiver_sessions_.size()
               ? nullptr
               : receiver_sessions_[receiver].get();
}

HardwareStreamService::~HardwareStreamService() noexcept
{
    request_stop();
    std::lock_guard<std::mutex> life(lifecycle_mutex_);
    cancel_and_join();
    // Do not issue USB cleanup from a destructor. Normal service shutdown must
    // call stop_capture while the exact claimed handle is still owned.
}

std::uint8_t HardwareStreamService::receiver_count() const noexcept
{
    return frontend_.receiver_count();
}

bool HardwareStreamService::receiver_supports(std::uint8_t receiver, System system) const noexcept
{
    return receiver < receiver_count() &&
           (dispatcher_ || receiver == pinned_receiver_) && frontend_.receiver_supports(receiver, system);
}

bool HardwareStreamService::selects_satellite_stream_before_tune() const noexcept
{
    return frontend_.selects_satellite_stream_before_tune();
}

bool HardwareStreamService::requires_terrestrial_lock_settle() const noexcept
{
    return frontend_.requires_terrestrial_lock_settle();
}

Result<void> HardwareStreamService::open_receiver(std::uint8_t receiver) noexcept
{
    if (receiver >= receiver_count()) {
        return Result<void>::failure(Error::UNSUPPORTED);
    }
    return frontend_.open_receiver(receiver);
}

Result<void> HardwareStreamService::tune_terrestrial(std::uint8_t receiver,
                                                     std::uint32_t frequency_khz,
                                                     std::uint32_t timeout_ms) noexcept
{
    if (!receiver_supports(receiver, System::ISDB_T)) {
        return Result<void>::failure(Error::UNSUPPORTED);
    }
    return frontend_.tune_terrestrial(receiver, frequency_khz, timeout_ms);
}

Result<void> HardwareStreamService::tune_satellite(std::uint8_t receiver,
                                                   std::uint32_t frequency_khz,
                                                   std::uint32_t timeout_ms) noexcept
{
    if (!receiver_supports(receiver, System::ISDB_S)) {
        return Result<void>::failure(Error::UNSUPPORTED);
    }
    return frontend_.tune_satellite(receiver, frequency_khz, timeout_ms);
}

Result<TunerHzProgramming> HardwareStreamService::tune_terrestrial_hz(
    std::uint8_t receiver, std::uint64_t frequency_hz, std::uint32_t timeout_ms) noexcept
{
    if (frequency_hz < 40000000U || frequency_hz > 1002000000U ||
        frequency_hz / 1000U > UINT32_MAX)
        return Result<TunerHzProgramming>::failure(Error::UNSUPPORTED);
    // ASICEN's hardware tune commands use integer kHz; report the floor conversion.
    const std::uint32_t frequency_khz = static_cast<std::uint32_t>(frequency_hz / 1000U);
    const auto tuned = tune_terrestrial(receiver, frequency_khz, timeout_ms);
    if (!tuned) return Result<TunerHzProgramming>::failure(tuned.error());
    TunerHzProgramming result;
    result.programmed_hz = static_cast<std::uint64_t>(frequency_khz) * 1000U;
    return Result<TunerHzProgramming>::success(result);
}

Result<TunerHzProgramming> HardwareStreamService::tune_satellite_hz(
    std::uint8_t receiver, std::uint64_t frequency_hz, std::uint32_t timeout_ms) noexcept
{
    if (frequency_hz < 146875000U || frequency_hz > 2350000000U ||
        frequency_hz / 1000U > UINT32_MAX)
        return Result<TunerHzProgramming>::failure(Error::UNSUPPORTED);
    const std::uint32_t frequency_khz = static_cast<std::uint32_t>(frequency_hz / 1000U);
    const auto tuned = tune_satellite(receiver, frequency_khz, timeout_ms);
    if (!tuned) return Result<TunerHzProgramming>::failure(tuned.error());
    TunerHzProgramming result;
    result.programmed_hz = static_cast<std::uint64_t>(frequency_khz) * 1000U;
    return Result<TunerHzProgramming>::success(result);
}

TunerStrictProfile HardwareStreamService::strict_profile(
    std::uint8_t receiver, System system) const noexcept
{
    if (!receiver_supports(receiver, system)) return {};
    TunerStrictProfile result;
    result.native_hz = true;
    if (system == System::ISDB_T) {
        result.min_hz = 40000000U;
        result.max_hz = 1002000000U;
    } else {
        result.min_hz = 146875000U;
        result.max_hz = 2350000000U;
        result.lnb_voltage_mask = 0x03U;
    }
    return result;
}

Result<bool> HardwareStreamService::is_locked(std::uint8_t receiver, System system) noexcept
{
    if (!receiver_supports(receiver, system)) {
        return Result<bool>::failure(Error::UNSUPPORTED);
    }
    return frontend_.is_locked(receiver, system);
}

Result<void> HardwareStreamService::select_satellite_slot(std::uint8_t receiver, std::uint8_t slot,
                                                          std::uint32_t timeout_ms) noexcept
{
    if (!receiver_supports(receiver, System::ISDB_S)) {
        return Result<void>::failure(Error::UNSUPPORTED);
    }
    return frontend_.select_satellite_slot(receiver, slot, timeout_ms);
}

Result<void> HardwareStreamService::select_satellite_tsid(std::uint8_t receiver, std::uint16_t tsid,
                                                          std::uint32_t timeout_ms) noexcept
{
    if (!receiver_supports(receiver, System::ISDB_S)) {
        return Result<void>::failure(Error::UNSUPPORTED);
    }
    return frontend_.select_satellite_tsid(receiver, tsid, timeout_ms);
}

Result<void> HardwareStreamService::close_receiver(std::uint8_t receiver) noexcept
{
    if (receiver >= receiver_count()) {
        return Result<void>::failure(Error::UNSUPPORTED);
    }
    return frontend_.close_receiver(receiver);
}

Result<void> HardwareStreamService::begin_tune_power(std::uint8_t receiver, System system,
                                                     std::uint8_t lnb_voltage) noexcept
{
    if (!receiver_supports(receiver, system)) {
        return Result<void>::failure(Error::UNSUPPORTED);
    }
    if ((lnb_voltage != 0U && lnb_voltage != 15U) ||
        (system == System::ISDB_T && lnb_voltage != 0U)) {
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    }
    // LNB support is a model-specific frontend decision. Preserve its failure
    // and the exact request rather than converting an ON request into OFF.
    return frontend_.begin_tune_power(receiver, system, lnb_voltage);
}

Result<void> HardwareStreamService::commit_tune_power(std::uint8_t receiver) noexcept
{
    if (receiver >= receiver_count()) {
        return Result<void>::failure(Error::UNSUPPORTED);
    }
    return frontend_.commit_tune_power(receiver);
}

Result<void> HardwareStreamService::set_independent_lnb_power(bool enabled) noexcept
{
    return frontend_.set_independent_lnb_power(enabled);
}

dtv::userland::wire::Optional<bool>
HardwareStreamService::independent_lnb_power_state() noexcept
{
    return frontend_.independent_lnb_power_state();
}

Result<void> HardwareStreamService::rollback_tune_power(std::uint8_t receiver) noexcept
{
    if (receiver >= receiver_count()) {
        return Result<void>::failure(Error::UNSUPPORTED);
    }
    return frontend_.rollback_tune_power(receiver);
}

void HardwareStreamService::mark_receiver_disconnected(std::uint8_t receiver) noexcept
{
    if (receiver < receiver_count()) {
        frontend_.mark_receiver_disconnected(receiver);
    }
    request_stop();
}

void HardwareStreamService::request_stop() noexcept
{
    if (dispatcher_) {
        shutdown_requested_.store(true);
        for (auto& session : receiver_sessions_) {
            if (session) session->request_stop();
        }
        frontend_.request_stop();
        return;
    }
    shutdown_requested_.store(true);
    cancelled_.store(true);
    condition_.notify_all();
    const auto receiver = capture_receiver_.load();
    if (receiver < receiver_count()) source_.interrupt_receiver(receiver);
    else source_.interrupt();
    frontend_.request_stop();
}

Result<void> HardwareStreamService::shutdown() noexcept
{
    if (dispatcher_) {
        request_stop();
        bool failed = false;
        Error first_error = Error::USB_IO;
        for (auto& session : receiver_sessions_) {
            if (!session) continue;
            const auto stopped = session->shutdown_capture(false);
            if (!stopped && !failed) {
                first_error = stopped.error();
                failed = true;
            }
        }
        const auto frontend_stopped = frontend_.shutdown();
        if (!frontend_stopped && !failed) {
            first_error = frontend_stopped.error();
            failed = true;
        }
        return failed ? Result<void>::failure(first_error) : Result<void>::success();
    }
    return shutdown_capture(true);
}

Result<void> HardwareStreamService::shutdown_capture(bool shutdown_frontend) noexcept
{
    request_stop();
    std::lock_guard<std::mutex> life(lifecycle_mutex_);
    cancel_and_join();
    bool needs_stop = false;
    bool already_quarantined = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        already_quarantined = state_ == State::quarantined;
        needs_stop = source_prepared_ && !source_stopped_;
    }
    bool failed = already_quarantined;
    Error first_error = Error::USB_IO;
    if (needs_stop) {
        const auto stopped = source_.stop_receiver(capture_receiver_.load());
        std::lock_guard<std::mutex> lock(mutex_);
        if (!stopped) {
            if (!failed) {
                first_error = stopped.error();
            }
            failed = true;
            source_stopped_ = true;  // attempted once; retain quarantine, avoid retry loops
            state_ = State::quarantined;
            set_terminal_locked(TunerStreamTerminal::usb_error);
            ++counters_.usb_errors;
            final_ = {counters_locked(), static_cast<std::uint8_t>(terminal_)};
        } else {
            source_stopped_ = true;
            final_ = {counters_locked(), static_cast<std::uint8_t>(terminal_)};
        }
    }
    // A failed ordinary stream cleanup still permits independent frontend
    // shutdown work (notably safe GPIO restoration). Callback-drain failures
    // retain ownership in source quarantine and reject further hardware I/O.
    if (shutdown_frontend) {
        const auto frontend_stopped = frontend_.shutdown();
        if (!frontend_stopped && !failed) {
            first_error = frontend_stopped.error();
            failed = true;
        }
    }
    return failed ? Result<void>::failure(first_error) : Result<void>::success();
}

Result<void> HardwareStreamService::start_capture(std::uint8_t receiver, System system) noexcept
{
    if (dispatcher_) {
        auto* session = session_for(receiver);
        return session == nullptr ? Result<void>::failure(Error::UNSUPPORTED)
                                  : session->start_capture(receiver, system);
    }
    if (!receiver_supports(receiver, system)) {
        return Result<void>::failure(Error::UNSUPPORTED);
    }
    if (queue_capacity_bytes_ == 0U) {
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    }
    if (!queue_ || !continuity_) {
        return Result<void>::failure(Error::INTERNAL);
    }
    std::lock_guard<std::mutex> life(lifecycle_mutex_);
    if (shutdown_requested_.load()) {
        return Result<void>::failure(Error::NOT_READY);
    }
    bool launch_failed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == State::quarantined) {
            return Result<void>::failure(Error::USB_IO);
        }
        if (state_ != State::idle) {
            return Result<void>::failure(Error::BUSY);
        }
        cancelled_.store(false);
        if (shutdown_requested_.load()) {
            cancelled_.store(true);
            return Result<void>::failure(Error::NOT_READY);
        }
        clear_attempt_locked();
        state_ = State::preparing;
        source_stopped_ = false;
    }
    capture_receiver_.store(receiver);
    const auto prepared = source_.prepare(receiver, system, cancelled_);
    if (!prepared) {
        const auto cleaned = source_.stop_receiver(receiver);
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = cleaned ? State::idle : State::quarantined;
        source_stopped_ = static_cast<bool>(cleaned);
        if (cleaned) capture_receiver_.store(0xffU);
        return Result<void>::failure(prepared.error());
    }
    if (shutdown_requested_.load()) {
        source_.interrupt_receiver(receiver);
        const auto cleaned = source_.stop_receiver(receiver);
        std::lock_guard<std::mutex> lock(mutex_);
        source_prepared_ = true;
        source_stopped_ = static_cast<bool>(cleaned);
        state_ = cleaned ? State::idle : State::quarantined;
        if (cleaned) capture_receiver_.store(0xffU);
        return Result<void>::failure(Error::NOT_READY);
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        source_prepared_ = true;
        state_ = State::running;
        if (!worker_.start(
                [](void* context) -> void* {
                    static_cast<HardwareStreamService*>(context)->run_worker();
                    return nullptr;
                },
                this, thread_launcher_)) {
            state_ = State::quarantined;
            cancelled_.store(true);
            launch_failed = true;
        }
    }
    if (launch_failed) {
        source_.interrupt_receiver(receiver);
        const auto stopped = source_.stop_receiver(receiver);
        std::lock_guard<std::mutex> lock(mutex_);
        source_stopped_ = static_cast<bool>(stopped);
        if (stopped) {
            state_ = State::idle;
            capture_receiver_.store(0xffU);
        }
        return Result<void>::failure(Error::INTERNAL);
    }
    return Result<void>::success();
}

Result<void> HardwareStreamService::stop_capture(std::uint8_t receiver, System system) noexcept
{
    if (dispatcher_) {
        const auto* session = session_for(receiver);
        return session == nullptr ? Result<void>::success()
                                  : const_cast<HardwareStreamService*>(session)->stop_capture(
                                        receiver, system);
    }
    if (!receiver_supports(receiver, system)) {
        return Result<void>::failure(Error::UNSUPPORTED);
    }
    std::lock_guard<std::mutex> life(lifecycle_mutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == State::idle) {
            return Result<void>::success();
        }
        if (state_ == State::quarantined) {
            return Result<void>::failure(Error::USB_IO);
        }
        if (state_ != State::idle && capture_receiver_.load() != receiver) {
            return Result<void>::failure(Error::BUSY);
        }
    }
    cancel_and_join();
    bool needs_stop = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        needs_stop = source_prepared_ && !source_stopped_;
    }
    if (needs_stop) {
        const auto stopped = source_.stop_receiver(receiver);
        std::lock_guard<std::mutex> lock(mutex_);
        if (!stopped) {
            source_stopped_ = true;  // cleanup was attempted; the frontend still gets shutdown
            state_ = State::quarantined;
            set_terminal_locked(TunerStreamTerminal::usb_error);
            ++counters_.usb_errors;
            final_ = {counters_locked(), static_cast<std::uint8_t>(terminal_)};
            return Result<void>::failure(stopped.error());
        }
        source_stopped_ = true;
        if (!have_final_attachment_) {
            state_ = State::idle;
        }
        final_ = {counters_locked(), static_cast<std::uint8_t>(terminal_)};
        capture_receiver_.store(0xffU);
    }
    return Result<void>::success();
}

Result<void> HardwareStreamService::attach(const TunerAttachment& attachment) noexcept
{
    if (dispatcher_) {
        auto* session = session_for(attachment.receiver);
        return session == nullptr ? Result<void>::failure(Error::NOT_READY)
                                  : session->attach(attachment);
    }
    if (!receiver_supports(attachment.receiver, attachment.system) ||
        attachment.owner_client_id == 0U || attachment.lease_id == 0U ||
        attachment.attachment_id == 0U) {
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != State::running || have_attachment_ ||
        attachment.receiver != capture_receiver_.load()) {
        return Result<void>::failure(Error::NOT_READY);
    }
    attachment_ = attachment;
    have_attachment_ = true;
    condition_.notify_all();
    return Result<void>::success();
}

Result<void> HardwareStreamService::detach(const TunerAttachment& attachment) noexcept
{
    if (dispatcher_) {
        const auto* session = session_for(attachment.receiver);
        return session == nullptr ? Result<void>::failure(Error::NOT_FOUND)
                                  : const_cast<HardwareStreamService*>(session)->detach(attachment);
    }
    std::lock_guard<std::mutex> life(lifecycle_mutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!matches_locked(attachment)) {
            return Result<void>::failure(Error::NOT_FOUND);
        }
        final_attachment_ = attachment_;
        have_final_attachment_ = true;
        final_snapshot_ready_ = false;
        have_attachment_ = false;
        draining_ = true;
        attachment_ = {};
        state_ = State::detached;
        cancelled_.store(true);
        if (terminal_ == TunerStreamTerminal::none) {
            terminal_ = TunerStreamTerminal::stopped;
        }
        condition_.notify_all();
    }
    source_.interrupt_receiver(attachment.receiver);
    if (worker_.joinable()) {
        worker_.join();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        final_ = {counters_locked(), static_cast<std::uint8_t>(terminal_)};
        final_snapshot_ready_ = true;
        condition_.notify_all();
    }
    return Result<void>::success();
}

Result<px4::userland::TunerStreamCounters>
HardwareStreamService::stats(const TunerAttachment& attachment) const noexcept
{
    if (dispatcher_) {
        const auto* session = session_for(attachment.receiver);
        return session == nullptr
                   ? Result<px4::userland::TunerStreamCounters>::failure(Error::NOT_FOUND)
                   : session->stats(attachment);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!matches_locked(attachment)) {
        return Result<px4::userland::TunerStreamCounters>::failure(Error::NOT_FOUND);
    }
    return Result<px4::userland::TunerStreamCounters>::success(counters_locked());
}

Result<px4::userland::TunerStreamFinalSnapshot>
HardwareStreamService::final_snapshot(const TunerAttachment& attachment) const noexcept
{
    if (dispatcher_) {
        const auto* session = session_for(attachment.receiver);
        return session == nullptr
                   ? Result<px4::userland::TunerStreamFinalSnapshot>::failure(Error::NOT_FOUND)
                   : session->final_snapshot(attachment);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!matches_final_locked(attachment) || !final_snapshot_ready_ ||
        (state_ != State::detached && state_ != State::quarantined)) {
        return Result<px4::userland::TunerStreamFinalSnapshot>::failure(
            matches_final_locked(attachment) ? Error::NOT_READY : Error::NOT_FOUND);
    }
    return Result<px4::userland::TunerStreamFinalSnapshot>::success(final_);
}

Result<void> HardwareStreamService::release_final(const TunerAttachment& attachment) noexcept
{
    if (dispatcher_) {
        const auto* session = session_for(attachment.receiver);
        return session == nullptr ? Result<void>::failure(Error::NOT_FOUND)
                                  : const_cast<HardwareStreamService*>(session)->release_final(
                                        attachment);
    }
    std::lock_guard<std::mutex> life(lifecycle_mutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!matches_final_locked(attachment)) {
        return Result<void>::failure(Error::NOT_FOUND);
    }
    if (state_ != State::detached || !source_stopped_) {
        return Result<void>::failure(Error::NOT_READY);
    }
    clear_attempt_locked();
    state_ = State::idle;
    return Result<void>::success();
}

Result<px4::userland::TunerStreamReadResult>
HardwareStreamService::read(const TunerAttachment& attachment,
                            px4::userland::MutableByteView output,
                            px4::userland::Timeout timeout) noexcept
{
    if (dispatcher_) {
        const auto* session = session_for(attachment.receiver);
        return session == nullptr
                   ? Result<px4::userland::TunerStreamReadResult>::failure(Error::NOT_FOUND)
                   : const_cast<HardwareStreamService*>(session)->read(attachment, output, timeout);
    }
    if (attachment.receiver >= receiver_count() || output.data == nullptr || output.size == 0U ||
        output.size % 188U != 0U || output.size > kMaxReadBytes) {
        return Result<px4::userland::TunerStreamReadResult>::failure(Error::INVALID_ARGUMENT);
    }
    std::unique_lock<std::mutex> lock(mutex_);
    if (draining_ && matches_final_locked(attachment) && !final_snapshot_ready_) {
        return Result<px4::userland::TunerStreamReadResult>::success(
            {0U, false, true, TunerStreamTerminal::none});
    }
    if (!matches_locked(attachment) && !(draining_ && matches_final_locked(attachment))) {
        if (matches_final_locked(attachment) && state_ == State::detached &&
            !final_snapshot_ready_) {
            return Result<px4::userland::TunerStreamReadResult>::success(
                {0U, false, true, TunerStreamTerminal::none});
        }
        return Result<px4::userland::TunerStreamReadResult>::failure(Error::NOT_FOUND);
    }
    if (queue_bytes_ == 0U && !worker_done_ && state_ == State::running) {
        condition_.wait_for(lock, std::chrono::milliseconds(timeout.milliseconds), [&] {
            return queue_bytes_ != 0U || worker_done_ || state_ != State::running;
        });
    }
    if (draining_ && matches_final_locked(attachment) && !final_snapshot_ready_) {
        return Result<px4::userland::TunerStreamReadResult>::success(
            {0U, false, true, TunerStreamTerminal::none});
    }
    if (!matches_locked(attachment) && !(draining_ && matches_final_locked(attachment))) {
        if (matches_final_locked(attachment) && state_ == State::detached &&
            !final_snapshot_ready_) {
            return Result<px4::userland::TunerStreamReadResult>::success(
                {0U, false, true, TunerStreamTerminal::none});
        }
        return Result<px4::userland::TunerStreamReadResult>::failure(Error::NOT_FOUND);
    }
    if (queue_bytes_ == 0U) {
        const bool eof = worker_done_ || state_ == State::detached || state_ == State::quarantined;
        if (eof) {
            draining_ = false;
        }
        return Result<px4::userland::TunerStreamReadResult>::success({0U, eof, !eof, terminal_});
    }
    const std::size_t copied = std::min(output.size, queue_bytes_);
    const std::size_t first = std::min(copied, queue_capacity_bytes_ - queue_head_);
    std::memcpy(output.data, queue_.get() + queue_head_, first);
    if (copied > first) {
        std::memcpy(output.data + first, queue_.get(), copied - first);
    }
    queue_head_ = (queue_head_ + copied) % queue_capacity_bytes_;
    queue_bytes_ -= copied;
    return Result<px4::userland::TunerStreamReadResult>::success(
        {copied, false, false, TunerStreamTerminal::none});
}

Result<TunerStreamTerminal>
HardwareStreamService::terminal(const TunerAttachment& attachment) const noexcept
{
    if (dispatcher_) {
        const auto* session = session_for(attachment.receiver);
        return session == nullptr
                   ? Result<TunerStreamTerminal>::failure(Error::NOT_FOUND)
                   : session->terminal(attachment);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!matches_locked(attachment)) {
        return Result<TunerStreamTerminal>::failure(Error::NOT_FOUND);
    }
    return Result<TunerStreamTerminal>::success(terminal_);
}

bool HardwareStreamService::emit_trampoline(void* context, const std::uint8_t* bytes,
                                            std::size_t size) noexcept
{
    return static_cast<HardwareStreamService*>(context)->emit(bytes, size);
}

bool HardwareStreamService::emit(const std::uint8_t* bytes, std::size_t size) noexcept
{
    bool overflow = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cancelled_.load() || state_ == State::quarantined) {
            return false;
        }
        if (bytes == nullptr || size == 0U || size % 188U != 0U) {
            ++counters_.sync_errors;
            set_terminal_locked(TunerStreamTerminal::sync_error);
            cancelled_.store(true);
            overflow = true;
        } else {
            for (std::size_t offset = 0U; offset < size; offset += 188U) {
                observe_packet_locked(bytes + offset);
                ++counters_.packets;
                counters_.bytes += 188U;
                if (queue_bytes_ == queue_capacity_bytes_) {
                    ++counters_.queue_drops;
                    set_terminal_locked(TunerStreamTerminal::slow_consumer);
                    cancelled_.store(true);
                    overflow = true;
                    break;
                }
                const std::size_t tail = (queue_head_ + queue_bytes_) % queue_capacity_bytes_;
                std::memcpy(queue_.get() + tail, bytes + offset, 188U);
                queue_bytes_ += 188U;
            }
            condition_.notify_all();
            if (!overflow) {
                return true;
            }
        }
        condition_.notify_all();
    }
    if (overflow) {
        source_.interrupt_receiver(capture_receiver_.load());
    }
    return false;
}

void HardwareStreamService::run_worker() noexcept
{
    {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this] { return have_attachment_ || cancelled_.load(); });
        if (cancelled_.load()) {
            worker_done_ = true;
            condition_.notify_all();
            return;
        }
    }
    const auto receiver = capture_receiver_.load();
    const CaptureRunResult result = source_.run_for_receiver(
        receiver, cancelled_, &HardwareStreamService::emit_trampoline, this);
    if (result == CaptureRunResult::fatal_drain) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_ = State::quarantined;
            set_terminal_locked(TunerStreamTerminal::bridge_fatal);
            worker_done_ = true;
            condition_.notify_all();
        }
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    worker_done_ = true;
    if (result == CaptureRunResult::disconnected) {
        ++counters_.usb_errors;
        set_terminal_locked(TunerStreamTerminal::disconnected);
    } else if (result == CaptureRunResult::usb_error) {
        ++counters_.usb_errors;
        set_terminal_locked(TunerStreamTerminal::usb_error);
    } else if (result == CaptureRunResult::sync_error) {
        ++counters_.sync_errors;
        set_terminal_locked(TunerStreamTerminal::sync_error);
    } else if (result == CaptureRunResult::completed && terminal_ == TunerStreamTerminal::none) {
        terminal_ = TunerStreamTerminal::stopped;
    }
    condition_.notify_all();
}

bool HardwareStreamService::matches_locked(const TunerAttachment& attachment) const noexcept
{
    return have_attachment_ && same_attachment(attachment_, attachment);
}

bool HardwareStreamService::matches_final_locked(const TunerAttachment& attachment) const noexcept
{
    return have_final_attachment_ && same_attachment(final_attachment_, attachment);
}

void HardwareStreamService::set_terminal_locked(TunerStreamTerminal terminal) noexcept
{
    if (terminal_ == TunerStreamTerminal::none || terminal_ == TunerStreamTerminal::stopped) {
        terminal_ = terminal;
    }
}

void HardwareStreamService::clear_attempt_locked() noexcept
{
    queue_head_ = 0U;
    queue_bytes_ = 0U;
    if (continuity_) {
        continuity_->reset();
    }
    counters_ = {};
    counters_.availability.fill(dtv::userland::wire::MeasureState::valid);
    attachment_ = {};
    final_attachment_ = {};
    final_ = {};
    have_attachment_ = false;
    have_final_attachment_ = false;
    draining_ = false;
    final_snapshot_ready_ = false;
    worker_done_ = false;
    source_prepared_ = false;
    source_stopped_ = true;
    capture_receiver_.store(0xffU);
    terminal_ = TunerStreamTerminal::none;
}

void HardwareStreamService::cancel_and_join() noexcept
{
    cancelled_.store(true);
    condition_.notify_all();
    const auto receiver = capture_receiver_.load();
    if (receiver < receiver_count()) source_.interrupt_receiver(receiver);
    else source_.interrupt();
    if (worker_.joinable()) {
        worker_.join();
    }
}

px4::userland::TunerStreamCounters HardwareStreamService::counters_locked() const noexcept
{
    auto counters = counters_;
    const auto source = source_.source_counters_for_receiver(capture_receiver_.load());
    counters.sync_errors += source.sync_errors;
    counters.empty_intervals += source.empty_intervals;
    counters.availability[7U] = source.availability[7U];
    return counters;
}

void HardwareStreamService::observe_packet_locked(const std::uint8_t* packet) noexcept
{
    if ((packet[1U] & 0x80U) != 0U) {
        ++counters_.tei_packets;
    }
    if (continuity_->observe(packet, px4::userland::TsContinuityChecker::kPacketSize).kind ==
        px4::userland::TsContinuityKind::error) {
        ++counters_.continuity_errors;
    }
    // Observation only: no filtering or IT930x PSB startup-drop heuristic.
}

}  // namespace asicen
