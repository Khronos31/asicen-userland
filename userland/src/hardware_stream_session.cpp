// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/hardware_stream_session.h"

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <sys/resource.h>
#include <utility>

namespace asicen {
namespace {
using px4::userland::Error;
using px4::userland::Result;
using px4::userland::TunerAttachment;
using px4::userland::TunerStreamTerminal;
using px4::userland::ipc::System;

bool same_attachment(const TunerAttachment& a, const TunerAttachment& b) {
    return a.owner_client_id == b.owner_client_id && a.lease_id == b.lease_id &&
           a.attachment_id == b.attachment_id && a.receiver == b.receiver &&
           a.system == b.system && a.nonce == b.nonce;
}

bool supported(std::uint8_t receiver, System system) {
    return receiver == 1U && system == System::ISDB_T;
}
}  // namespace

void ExitProcessFatal::terminate_nonzero(int code) noexcept {
    const rlimit no_core{0, 0};
    (void)::setrlimit(RLIMIT_CORE, &no_core);
    std::_Exit(code == 0 ? 70 : code);
}

HardwareStreamService::HardwareStreamService(
    px4::userland::TunerServiceBackend& frontend, StreamCaptureSource& source,
    StreamProcessFatal& fatal, std::size_t queue_capacity_bytes)
    : frontend_(frontend), source_(source), fatal_(fatal),
      queue_capacity_bytes_(queue_capacity_bytes) {}

HardwareStreamService::~HardwareStreamService() noexcept {
    request_stop();
    std::lock_guard<std::mutex> life(lifecycle_mutex_);
    cancel_and_join();
    // Do not issue USB cleanup from a destructor. Normal service shutdown must
    // call stop_capture while the exact claimed handle is still owned.
}

std::uint8_t HardwareStreamService::receiver_count() const noexcept { return 4U; }

bool HardwareStreamService::receiver_supports(std::uint8_t receiver,
                                              System system) const noexcept {
    return supported(receiver, system);
}

bool HardwareStreamService::selects_satellite_stream_before_tune() const noexcept {
    return false;
}

bool HardwareStreamService::requires_terrestrial_lock_settle() const noexcept {
    return frontend_.requires_terrestrial_lock_settle();
}

Result<void> HardwareStreamService::open_receiver(std::uint8_t receiver) noexcept {
    if (receiver != 1U) return Result<void>::failure(Error::UNSUPPORTED);
    return frontend_.open_receiver(receiver);
}

Result<void> HardwareStreamService::tune_terrestrial(
    std::uint8_t receiver, std::uint32_t frequency_khz,
    std::uint32_t timeout_ms) noexcept {
    if (receiver != 1U) return Result<void>::failure(Error::UNSUPPORTED);
    return frontend_.tune_terrestrial(receiver, frequency_khz, timeout_ms);
}

Result<void> HardwareStreamService::tune_satellite(
    std::uint8_t, std::uint32_t, std::uint32_t) noexcept {
    return Result<void>::failure(Error::UNSUPPORTED);
}

Result<bool> HardwareStreamService::is_locked(std::uint8_t receiver,
                                              System system) noexcept {
    if (!supported(receiver, system)) return Result<bool>::failure(Error::UNSUPPORTED);
    return frontend_.is_locked(receiver, system);
}

Result<void> HardwareStreamService::select_satellite_slot(
    std::uint8_t, std::uint8_t, std::uint32_t) noexcept {
    return Result<void>::failure(Error::UNSUPPORTED);
}

Result<void> HardwareStreamService::select_satellite_tsid(
    std::uint8_t, std::uint16_t, std::uint32_t) noexcept {
    return Result<void>::failure(Error::UNSUPPORTED);
}

Result<void> HardwareStreamService::close_receiver(std::uint8_t receiver) noexcept {
    if (receiver != 1U) return Result<void>::failure(Error::UNSUPPORTED);
    return frontend_.close_receiver(receiver);
}

Result<void> HardwareStreamService::begin_tune_power(
    std::uint8_t receiver, System system, std::uint8_t lnb_voltage) noexcept {
    if (!supported(receiver, system) || lnb_voltage != 0U)
        return Result<void>::failure(Error::UNSUPPORTED);
    return frontend_.begin_tune_power(receiver, system, 0U);
}

Result<void> HardwareStreamService::commit_tune_power(std::uint8_t receiver) noexcept {
    if (receiver != 1U) return Result<void>::failure(Error::UNSUPPORTED);
    return frontend_.commit_tune_power(receiver);
}

Result<void> HardwareStreamService::rollback_tune_power(std::uint8_t receiver) noexcept {
    if (receiver != 1U) return Result<void>::failure(Error::UNSUPPORTED);
    return frontend_.rollback_tune_power(receiver);
}

void HardwareStreamService::mark_receiver_disconnected(std::uint8_t receiver) noexcept {
    if (receiver == 1U) frontend_.mark_receiver_disconnected(receiver);
    request_stop();
}

void HardwareStreamService::request_stop() noexcept {
    shutdown_requested_.store(true);
    cancelled_.store(true);
    condition_.notify_all();
    source_.interrupt();
    frontend_.request_stop();
}

Result<void> HardwareStreamService::shutdown() noexcept {
    request_stop();
    std::lock_guard<std::mutex> life(lifecycle_mutex_);
    cancel_and_join();
    bool needs_stop = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == State::quarantined)
            return Result<void>::failure(Error::USB_IO);
        needs_stop = source_prepared_ && !source_stopped_;
    }
    if (needs_stop) {
        const auto stopped = source_.stop();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!stopped) {
            state_ = State::quarantined;
            set_terminal_locked(TunerStreamTerminal::usb_error);
            ++counters_.usb_errors;
            final_ = {counters_, static_cast<std::uint8_t>(terminal_)};
            return Result<void>::failure(stopped.error());
        }
        source_stopped_ = true;
        final_ = {counters_, static_cast<std::uint8_t>(terminal_)};
    }
    return frontend_.shutdown();
}

Result<void> HardwareStreamService::start_capture(
    std::uint8_t receiver, System system) noexcept {
    if (!supported(receiver, system)) return Result<void>::failure(Error::UNSUPPORTED);
    std::lock_guard<std::mutex> life(lifecycle_mutex_);
    if (shutdown_requested_.load()) return Result<void>::failure(Error::NOT_READY);
    bool launch_failed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == State::quarantined) return Result<void>::failure(Error::USB_IO);
        if (state_ != State::idle) return Result<void>::failure(Error::BUSY);
        cancelled_.store(false);
        if (shutdown_requested_.load()) {
            cancelled_.store(true);
            return Result<void>::failure(Error::NOT_READY);
        }
        clear_attempt_locked();
        state_ = State::preparing;
        source_stopped_ = false;
    }
    const auto prepared = source_.prepare(receiver, system, cancelled_);
    if (!prepared) {
        const auto cleaned = source_.stop();
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = cleaned ? State::idle : State::quarantined;
        source_stopped_ = static_cast<bool>(cleaned);
        return Result<void>::failure(prepared.error());
    }
    if (shutdown_requested_.load()) {
        source_.interrupt();
        const auto cleaned = source_.stop();
        std::lock_guard<std::mutex> lock(mutex_);
        source_prepared_ = true;
        source_stopped_ = static_cast<bool>(cleaned);
        state_ = cleaned ? State::idle : State::quarantined;
        return Result<void>::failure(Error::NOT_READY);
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        source_prepared_ = true;
        state_ = State::running;
        try {
            worker_ = std::thread(&HardwareStreamService::run_worker, this);
        } catch (...) {
            state_ = State::quarantined;
            cancelled_.store(true);
            launch_failed = true;
        }
    }
    if (launch_failed) {
        source_.interrupt();
        const auto stopped = source_.stop();
        std::lock_guard<std::mutex> lock(mutex_);
        source_stopped_ = static_cast<bool>(stopped);
        if (stopped) state_ = State::idle;
        return Result<void>::failure(Error::INTERNAL);
    }
    return Result<void>::success();
}

Result<void> HardwareStreamService::stop_capture(
    std::uint8_t receiver, System system) noexcept {
    if (!supported(receiver, system)) return Result<void>::failure(Error::UNSUPPORTED);
    std::lock_guard<std::mutex> life(lifecycle_mutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == State::idle) return Result<void>::success();
        if (state_ == State::quarantined) return Result<void>::failure(Error::USB_IO);
    }
    cancel_and_join();
    bool needs_stop = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        needs_stop = source_prepared_ && !source_stopped_;
    }
    if (needs_stop) {
        const auto stopped = source_.stop();
        std::lock_guard<std::mutex> lock(mutex_);
        if (!stopped) {
            state_ = State::quarantined;
            set_terminal_locked(TunerStreamTerminal::usb_error);
            ++counters_.usb_errors;
            final_ = {counters_, static_cast<std::uint8_t>(terminal_)};
            return Result<void>::failure(stopped.error());
        }
        source_stopped_ = true;
        if (!have_final_attachment_) state_ = State::idle;
        final_ = {counters_, static_cast<std::uint8_t>(terminal_)};
    }
    return Result<void>::success();
}

Result<void> HardwareStreamService::attach(const TunerAttachment& attachment) noexcept {
    if (!supported(attachment.receiver, attachment.system) ||
        attachment.owner_client_id == 0U || attachment.lease_id == 0U ||
        attachment.attachment_id == 0U) {
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != State::running || have_attachment_)
        return Result<void>::failure(Error::NOT_READY);
    attachment_ = attachment;
    have_attachment_ = true;
    return Result<void>::success();
}

Result<void> HardwareStreamService::detach(const TunerAttachment& attachment) noexcept {
    std::lock_guard<std::mutex> life(lifecycle_mutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!matches_locked(attachment)) return Result<void>::failure(Error::NOT_FOUND);
        final_attachment_ = attachment_;
        have_final_attachment_ = true;
        have_attachment_ = false;
        attachment_ = {};
        state_ = State::detached;
        cancelled_.store(true);
        if (terminal_ == TunerStreamTerminal::none)
            terminal_ = TunerStreamTerminal::stopped;
        condition_.notify_all();
    }
    source_.interrupt();
    if (worker_.joinable()) worker_.join();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        final_ = {counters_, static_cast<std::uint8_t>(terminal_)};
    }
    return Result<void>::success();
}

Result<px4::userland::TunerStreamCounters> HardwareStreamService::stats(
    const TunerAttachment& attachment) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!matches_locked(attachment)) return Result<px4::userland::TunerStreamCounters>::failure(Error::NOT_FOUND);
    return Result<px4::userland::TunerStreamCounters>::success(counters_);
}

Result<px4::userland::TunerStreamFinalSnapshot> HardwareStreamService::final_snapshot(
    const TunerAttachment& attachment) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!matches_final_locked(attachment) ||
        (state_ != State::detached && state_ != State::quarantined))
        return Result<px4::userland::TunerStreamFinalSnapshot>::failure(Error::NOT_FOUND);
    return Result<px4::userland::TunerStreamFinalSnapshot>::success(final_);
}

Result<void> HardwareStreamService::release_final(
    const TunerAttachment& attachment) noexcept {
    std::lock_guard<std::mutex> life(lifecycle_mutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!matches_final_locked(attachment)) return Result<void>::failure(Error::NOT_FOUND);
    if (state_ != State::detached || !source_stopped_)
        return Result<void>::failure(Error::NOT_READY);
    clear_attempt_locked();
    state_ = State::idle;
    return Result<void>::success();
}

Result<px4::userland::TunerStreamReadResult> HardwareStreamService::read(
    const TunerAttachment& attachment, px4::userland::MutableByteView output,
    px4::userland::Timeout timeout) noexcept {
    const std::size_t packet_capacity = (output.size / 188U) * 188U;
    if (output.data == nullptr || packet_capacity == 0U)
        return Result<px4::userland::TunerStreamReadResult>::failure(Error::BUFFER_TOO_SMALL);
    std::unique_lock<std::mutex> lock(mutex_);
    if (!matches_locked(attachment))
        return Result<px4::userland::TunerStreamReadResult>::failure(Error::NOT_FOUND);
    if (queue_.empty() && !worker_done_ && state_ == State::running)
        condition_.wait_for(lock, std::chrono::milliseconds(timeout.milliseconds), [&] {
            return !queue_.empty() || worker_done_ || state_ != State::running;
        });
    if (!matches_locked(attachment))
        return Result<px4::userland::TunerStreamReadResult>::failure(Error::NOT_FOUND);
    if (queue_.empty()) {
        const bool eof = worker_done_ || state_ == State::detached ||
                         state_ == State::quarantined;
        return Result<px4::userland::TunerStreamReadResult>::success(
            {0U, eof, !eof, terminal_});
    }
    std::size_t copied = 0U;
    while (!queue_.empty() && copied < packet_capacity) {
        auto& front = queue_.front();
        const std::size_t take = std::min(packet_capacity - copied, front.size());
        std::memcpy(output.data + copied, front.data(), take);
        copied += take;
        queue_bytes_ -= take;
        if (take == front.size()) queue_.pop_front();
        else front.erase(front.begin(), front.begin() + static_cast<std::ptrdiff_t>(take));
    }
    counters_.bytes += copied;
    counters_.packets += copied / 188U;
    return Result<px4::userland::TunerStreamReadResult>::success(
        {copied, false, false, TunerStreamTerminal::none});
}

Result<TunerStreamTerminal> HardwareStreamService::terminal(
    const TunerAttachment& attachment) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!matches_locked(attachment)) return Result<TunerStreamTerminal>::failure(Error::NOT_FOUND);
    return Result<TunerStreamTerminal>::success(terminal_);
}

bool HardwareStreamService::emit_trampoline(void* context,
                                            const std::uint8_t* bytes,
                                            std::size_t size) noexcept {
    return static_cast<HardwareStreamService*>(context)->emit(bytes, size);
}

bool HardwareStreamService::emit(const std::uint8_t* bytes,
                                 std::size_t size) noexcept {
    bool overflow = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cancelled_.load() || state_ == State::quarantined) return false;
        if (bytes == nullptr || size == 0U || size % 188U != 0U) {
            ++counters_.sync_errors;
            set_terminal_locked(TunerStreamTerminal::sync_error);
            cancelled_.store(true);
            overflow = true;
        } else if (size > queue_capacity_bytes_ ||
                   queue_bytes_ > queue_capacity_bytes_ - size) {
            ++counters_.queue_drops;
            set_terminal_locked(TunerStreamTerminal::slow_consumer);
            cancelled_.store(true);
            overflow = true;
        } else {
            queue_.emplace_back(bytes, bytes + size);
            queue_bytes_ += size;
            condition_.notify_all();
            return true;
        }
        condition_.notify_all();
    }
    if (overflow) source_.interrupt();
    return false;
}

void HardwareStreamService::run_worker() noexcept {
    const CaptureRunResult result =
        source_.run(cancelled_, &HardwareStreamService::emit_trampoline, this);
    if (result == CaptureRunResult::fatal_drain) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_ = State::quarantined;
            set_terminal_locked(TunerStreamTerminal::bridge_fatal);
            worker_done_ = true;
            condition_.notify_all();
        }
        fatal_.terminate_nonzero(70);
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

bool HardwareStreamService::matches_locked(const TunerAttachment& attachment) const noexcept {
    return have_attachment_ && same_attachment(attachment_, attachment);
}

bool HardwareStreamService::matches_final_locked(
    const TunerAttachment& attachment) const noexcept {
    return have_final_attachment_ && same_attachment(final_attachment_, attachment);
}

void HardwareStreamService::set_terminal_locked(
    TunerStreamTerminal terminal) noexcept {
    if (terminal_ == TunerStreamTerminal::none ||
        terminal_ == TunerStreamTerminal::stopped)
        terminal_ = terminal;
}

void HardwareStreamService::clear_attempt_locked() noexcept {
    queue_.clear();
    queue_bytes_ = 0U;
    counters_ = {};
    attachment_ = {};
    final_attachment_ = {};
    final_ = {};
    have_attachment_ = false;
    have_final_attachment_ = false;
    worker_done_ = false;
    source_prepared_ = false;
    source_stopped_ = true;
    terminal_ = TunerStreamTerminal::none;
}

void HardwareStreamService::cancel_and_join() noexcept {
    cancelled_.store(true);
    condition_.notify_all();
    source_.interrupt();
    if (worker_.joinable()) worker_.join();
}

}  // namespace asicen
