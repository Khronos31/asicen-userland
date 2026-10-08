// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/libusb_hardware_backend.h"

#include "asicen/device_profile.h"
#include "asicen/hardware_gpio_guard.h"
#include "asicen/transport_transform.h"
#include "asicen/write_protocol.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/random.h>
#include <sys/resource.h>
#include <thread>

namespace asicen {
namespace {
using px4::userland::Error;
using px4::userland::Result;
using px4::userland::ipc::System;

constexpr std::size_t kQueueDepth = 4U;
constexpr std::size_t kChunkSize = 4096U;
constexpr std::chrono::milliseconds kDrainLimit{2000};

[[noreturn]] void fatal_drain_exit() {
    const rlimit no_core{0, 0};
    (void)::setrlimit(RLIMIT_CORE, &no_core);
    std::_Exit(70);
}

}  // namespace

struct LibusbW3u3Hardware::AsyncState {
    struct Slot {
        AsyncState* owner = nullptr;
        std::array<unsigned char, kChunkSize> buffer{};
        libusb_transfer* transfer = nullptr;
        int status = LIBUSB_TRANSFER_ERROR;
        int actual = 0;
        bool pending = false;
        bool ready = false;
        std::uint64_t generation = 0;
    };
    std::array<Slot, kQueueDepth> slots{};
    std::array<std::size_t, kQueueDepth> ready{};
    std::size_t read = 0;
    std::size_t write = 0;
    std::size_t count = 0;
    bool prepared = false;
    bool drain_failed = false;

    static void LIBUSB_CALL completed(libusb_transfer* transfer) {
        auto* slot = static_cast<Slot*>(transfer->user_data);
        if (slot == nullptr || slot->owner == nullptr) return;
        slot->pending = false;
        slot->status = static_cast<int>(transfer->status);
        slot->actual = transfer->actual_length;
        slot->ready = true;
        AsyncState& state = *slot->owner;
        if (state.count == kQueueDepth) {
            state.drain_failed = true;
            return;
        }
        state.ready[state.write] = static_cast<std::size_t>(slot - state.slots.data());
        state.write = (state.write + 1U) % kQueueDepth;
        ++state.count;
    }
};

struct LibusbW3u3Hardware::DrainAdapter final : CaptureDrainOps,
                                                CaptureCleanupOps {
    explicit DrainAdapter(LibusbW3u3Hardware& owner) : owner_(owner) {}
    bool stop_dsc() noexcept override {
        const bool ok = owner_.dsc_stop(1U);
        owner_.dsc_stopped_ = ok;
        return ok;
    }
    void cancel_pending() noexcept override {
        if (!owner_.async_) return;
        for (auto& slot : owner_.async_->slots) {
            if (slot.transfer != nullptr && slot.pending)
                (void)libusb_cancel_transfer(slot.transfer);
        }
    }
    bool has_pending() const noexcept override {
        if (!owner_.async_) return false;
        for (const auto& slot : owner_.async_->slots)
            if (slot.pending) return true;
        return false;
    }
    void pump_events(unsigned timeout_ms) noexcept override {
        (void)owner_.handle_events(timeout_ms);
    }
    void release_transfers() noexcept override {
        if (!owner_.async_) return;
        for (auto& slot : owner_.async_->slots) {
            if (slot.transfer != nullptr) {
                libusb_free_transfer(slot.transfer);
                slot.transfer = nullptr;
            }
        }
        owner_.async_->prepared = false;
        owner_.async_->count = 0U;
    }
    CaptureRunResult cleanup_after_drain(bool dsc_stopped,
                                         bool dsc_attempted) noexcept override {
        return owner_.cleanup_after_drain(dsc_stopped, dsc_attempted);
    }
    bool disconnected() const noexcept override { return owner_.disconnected_.load(); }
    bool clear_seed_and_verify_output() noexcept override {
        return owner_.clear_link_seed_and_verify_controller();
    }
    bool disable_output_and_verify() noexcept override {
        return owner_.write_controller05(0U) && owner_.read_controller05_value_zero();
    }
    bool restore_cf_and_verify() noexcept override {
        if (!owner_.cf_snapshot_valid_) return true;
        const bool written = owner_.write_cf_block(1U, owner_.cf_snapshot_.data(),
                                                    owner_.cf_snapshot_.size());
        std::array<std::uint8_t, 0x45> verify{};
        return written && owner_.read_cf_block(1U, verify.data(), verify.size()) &&
               verify == owner_.cf_snapshot_;
    }
private:
    LibusbW3u3Hardware& owner_;
};

LibusbW3u3Hardware::LibusbW3u3Hardware(
    libusb_context* context, UsbLocation primary, UsbLocation sibling,
    std::vector<std::uint8_t> primary_path,
    std::vector<std::uint8_t> sibling_path)
    : context_(context), primary_claim_(primary_), sibling_claim_(sibling_),
      primary_path_(std::move(primary_path)),
      sibling_path_(std::move(sibling_path)), async_(std::make_unique<AsyncState>()),
      decoder_(nullptr, 0U) {
    if (context_ != nullptr && primary_.open(context_, primary) == 0 &&
        sibling_.open(context_, sibling) == 0) {
        // Handles remain open until release(), including after stream cleanup.
    }
}

LibusbW3u3Hardware::~LibusbW3u3Hardware() noexcept {
    // Destruction is only reached after normal shutdown. Never issue frontend
    // cleanup writes here because the USB address could already have changed.
    (void)release();
}

Result<void> LibusbW3u3Hardware::claim() {
    if (context_ == nullptr || !primary_.is_open() || !sibling_.is_open())
        return Result<void>::failure(Error::DISCONNECTED);
    const auto result = ownership_.claim_w3u3(primary_claim_, sibling_claim_,
                                               primary_path_, sibling_path_);
    if (result != OwnershipError::none)
        return Result<void>::failure(result == OwnershipError::primary_claim_failed ||
                                             result == OwnershipError::sibling_claim_failed
                                         ? Error::BUSY
                                         : Error::INVALID_ARGUMENT);
    claimed_ = true;
    if (!verify_device_revision()) {
        std::fprintf(stderr, "asicend: bridge revision check failed\n");
        (void)release();
        return Result<void>::failure(Error::UNSUPPORTED);
    }
    const auto gpio_read = make_gpio_set(0U, 0U, 1000U);
    std::array<unsigned char, 1> gpio_response{};
    if (control(gpio_read, gpio_response.data()) != gpio_read.length) {
        (void)release();
        return Result<void>::failure(Error::USB_IO);
    }
    gpio_snapshot_ = gpio_response[0];
    gpio_snapshot_valid_ = true;
    return Result<void>::success();
}

Result<void> LibusbW3u3Hardware::release() noexcept {
    const OwnershipError result = ownership_.release();
    const bool ok = result == OwnershipError::none;
    if (ok) {
        claimed_ = false;
        primary_.close();
        sibling_.close();
    }
    return ok ? Result<void>::success() : Result<void>::failure(Error::USB_IO);
}

std::uint8_t LibusbW3u3Hardware::receiver_count() const noexcept { return 4U; }
bool LibusbW3u3Hardware::receiver_supports(std::uint8_t receiver, System system) const noexcept {
    return receiver == 1U && system == System::ISDB_T;
}
bool LibusbW3u3Hardware::selects_satellite_stream_before_tune() const noexcept { return false; }
bool LibusbW3u3Hardware::requires_terrestrial_lock_settle() const noexcept { return true; }

Result<void> LibusbW3u3Hardware::open_receiver(std::uint8_t receiver) noexcept {
    if (!claimed_ || receiver != 1U) return Result<void>::failure(Error::UNSUPPORTED);
    if (!frontend_io_allowed_after_cleanup(cleanup_failed_))
        return Result<void>::failure(Error::USB_IO);
    if (initialized_) return Result<void>::success();
    if (!verify_device_revision()) {
        std::fprintf(stderr, "asicend: bridge revision check failed\n");
        return Result<void>::failure(Error::UNSUPPORTED);
    }
    if (!snapshot_gpio_if_needed(&gpio_snapshot_valid_, &gpio_snapshot_,
            [this](std::uint8_t* value) {
                if (value == nullptr) return false;
                const auto transfer = make_gpio_set(0U, 0U, 1000U);
                std::array<unsigned char, 1> response{};
                if (control(transfer, response.data()) != transfer.length) return false;
                *value = response[0];
                return true;
            })) {
        return Result<void>::failure(Error::USB_IO);
    }
    FrontendPlan power_plan = plan_startup_subset();
    const auto power = plan_safe_power_on();
    power_plan.insert(power_plan.end(), power.begin(), power.end());
    const FrontendPlan init_plan = plan_terrestrial_init_with_satellite_demod();

    // One deadline covers startup, the now-powered controller guard, and
    // demod initialization. Never issue controller I2C before this power
    // sequence: the device NACKs those accesses while its controller is off.
    deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    deadline_active_ = true;
    PoweredControllerCheck controller_check = PoweredControllerCheck::ready;
    const PoweredInitResult result = execute_powered_init_sequence(
        [&] { return run_frontend_plan(power_plan, this) == FrontendRunResult::Completed; },
        [&] {
            controller_check = verify_powered_controller();
            return controller_check == PoweredControllerCheck::ready;
        },
        [&] { return run_frontend_plan(init_plan, this) == FrontendRunResult::Completed; });
    deadline_active_ = false;
    if (result != PoweredInitResult::completed) {
        if (result == PoweredInitResult::controller_guard_failed) {
            const char* reason = "controller guard failed";
            switch (controller_check) {
                case PoweredControllerCheck::type_read_failed:
                    reason = "powered controller type read failed"; break;
                case PoweredControllerCheck::unsupported_type:
                    reason = "powered controller type unsupported"; break;
                case PoweredControllerCheck::output_state_read_failed:
                    reason = "controller output-idle read failed"; break;
                case PoweredControllerCheck::output_busy:
                    reason = "controller output is busy"; break;
                case PoweredControllerCheck::ready: break;
            }
            std::fprintf(stderr, "asicend: %s\n", reason);
        }
        const bool restored = restore_gpio_snapshot_safely();
        if (!restored) cleanup_failed_ = true;
        Error error = Error::USB_IO;
        if (result == PoweredInitResult::controller_guard_failed && restored) {
            if (controller_check == PoweredControllerCheck::unsupported_type)
                error = Error::UNSUPPORTED;
            else if (controller_check == PoweredControllerCheck::output_busy)
                error = Error::BUSY;
        }
        return Result<void>::failure(error);
    }
    initialized_ = true;
    return Result<void>::success();
}

Result<void> LibusbW3u3Hardware::tune_terrestrial(
    std::uint8_t receiver, std::uint32_t frequency_khz,
    std::uint32_t timeout_ms) noexcept {
    if (!frontend_io_allowed_after_cleanup(cleanup_failed_))
        return Result<void>::failure(Error::USB_IO);
    if (!claimed_ || receiver != 1U || !initialized_ || frequency_khz != 557142U)
        return Result<void>::failure(Error::UNSUPPORTED);
    if (!run_plan(plan_terrestrial_tune_full(frequency_khz, 6U), timeout_ms))
        return Result<void>::failure(disconnected_.load() ? Error::DISCONNECTED : Error::USB_IO);
    tuned_ = true;
    gain_applied_ = false;
    return Result<void>::success();
}

Result<void> LibusbW3u3Hardware::tune_satellite(std::uint8_t, std::uint32_t,
                                               std::uint32_t) noexcept {
    return Result<void>::failure(Error::UNSUPPORTED);
}

Result<bool> LibusbW3u3Hardware::is_locked(std::uint8_t receiver,
                                           System system) noexcept {
    if (!frontend_io_allowed_after_cleanup(cleanup_failed_))
        return Result<bool>::failure(Error::USB_IO);
    if (receiver != 1U || system != System::ISDB_T || !tuned_)
        return Result<bool>::failure(Error::UNSUPPORTED);
    std::uint8_t lock = 0;
    bool have = false;
    const auto plan = plan_terrestrial_lock_read(557142U);
    FrontendRunReport report{};
    if (!run_plan(plan, 1000U, &report) || !report.have_last_read)
        return Result<bool>::failure(disconnected_.load() ? Error::DISCONNECTED : Error::USB_IO);
    lock = report.last_read;
    have = true;
    const bool locked = have && (lock & 0x0fU) == 0x09U;
    if (locked && !gain_applied_) {
        if (!run_plan(plan_fc0012_gain_once(1U), 1000U))
            return Result<bool>::failure(Error::USB_IO);
        gain_applied_ = true;
    }
    return Result<bool>::success(locked);
}

Result<void> LibusbW3u3Hardware::select_satellite_slot(std::uint8_t, std::uint8_t,
                                                      std::uint32_t) noexcept {
    return Result<void>::failure(Error::UNSUPPORTED);
}
Result<void> LibusbW3u3Hardware::select_satellite_tsid(std::uint8_t, std::uint16_t,
                                                      std::uint32_t) noexcept {
    return Result<void>::failure(Error::UNSUPPORTED);
}
Result<void> LibusbW3u3Hardware::close_receiver(std::uint8_t receiver) noexcept {
    return receiver == 1U ? Result<void>::success() : Result<void>::failure(Error::UNSUPPORTED);
}
Result<void> LibusbW3u3Hardware::begin_tune_power(std::uint8_t receiver, System system,
                                                 std::uint8_t lnb_voltage) noexcept {
    if (!frontend_io_allowed_after_cleanup(cleanup_failed_))
        return Result<void>::failure(Error::USB_IO);
    if (receiver != 1U || system != System::ISDB_T || lnb_voltage != 0U)
        return Result<void>::failure(Error::UNSUPPORTED);
    // Power-on is part of the source-verified open sequence. The portable
    // TunerService transaction hook is intentionally a no-op for the one safe
    // zero-voltage terrestrial path so it cannot duplicate GPIO writes.
    return initialized_ ? Result<void>::success()
                        : Result<void>::failure(Error::NOT_READY);
}
Result<void> LibusbW3u3Hardware::commit_tune_power(std::uint8_t receiver) noexcept {
    return receiver == 1U ? Result<void>::success() : Result<void>::failure(Error::UNSUPPORTED);
}
Result<void> LibusbW3u3Hardware::rollback_tune_power(std::uint8_t receiver) noexcept {
    return receiver == 1U ? Result<void>::success() : Result<void>::failure(Error::UNSUPPORTED);
}
void LibusbW3u3Hardware::mark_receiver_disconnected(std::uint8_t receiver) noexcept {
    if (receiver == 1U) disconnected_.store(true);
}
void LibusbW3u3Hardware::request_stop() noexcept { stop_requested_.store(true); interrupt(); }
Result<void> LibusbW3u3Hardware::shutdown() noexcept {
    stop_requested_.store(true);
    const bool needs_stop = source_prepared_ || cf_snapshot_valid_ ||
                            link_snapshot_valid_ || (async_ && async_->prepared);
    if (!attempt_hardware_shutdown_cleanup(*this, needs_stop, gpio_snapshot_valid_))
        cleanup_failed_ = true;
    return cleanup_failed_ ? Result<void>::failure(Error::USB_IO)
                           : Result<void>::success();
}

bool LibusbW3u3Hardware::stop_capture_safely() noexcept {
    // stop() exits the process if callbacks cannot be drained within the hard
    // deadline. An ordinary cleanup error returns only after storage is safe.
    return stop().has_value();
}

bool LibusbW3u3Hardware::restore_gpio_snapshot_safely() noexcept {
    if (disconnected_.load()) return true;  // USB writes are forbidden after loss.
    const auto restore = make_gpio_set(gpio_snapshot_, 0xdfU, 1000U);
    std::array<unsigned char, 1> response{};
    if (control(restore, response.data()) != restore.length) return false;
    const auto readback = make_gpio_set(0U, 0U, 1000U);
    if (control(readback, response.data()) != readback.length ||
        (response[0] & 0xdfU) != (gpio_snapshot_ & 0xdfU)) return false;
    gpio_snapshot_valid_ = false;
    return true;
}

bool LibusbW3u3Hardware::run_plan(const FrontendPlan& plan, unsigned timeout_ms,
                                 FrontendRunReport* report) noexcept {
    if (!claimed_ || disconnected_.load() || plan.empty()) return false;
    deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    deadline_active_ = true;
    const auto result = run_frontend_plan(plan, this, report);
    deadline_active_ = false;
    return result == FrontendRunResult::Completed;
}

int LibusbW3u3Hardware::control(const ControlTransfer& original, unsigned char* data) {
    if (!claimed_ || disconnected_.load()) return LIBUSB_ERROR_NO_DEVICE;
    ControlTransfer transfer = original;
    bool skip = false;
    ControlTransfer safe{};
    if (!mask_lnb_gpio_operation(transfer, &safe, &skip)) return LIBUSB_ERROR_ACCESS;
    if (skip) {
        if (data != nullptr && transfer.length > 0) data[0] = 1U;
        return transfer.length;
    }
    transfer = safe;
    if (deadline_active_) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline_ - std::chrono::steady_clock::now()).count();
        if (left <= 0) return LIBUSB_ERROR_TIMEOUT;
        transfer.timeout_ms = static_cast<std::uint16_t>(std::min<long long>(left, 60000));
    }
    const int rc = primary_.control(transfer, data);
    if (rc == LIBUSB_ERROR_NO_DEVICE) disconnected_.store(true);
    return rc;
}
void LibusbW3u3Hardware::delay_ms(unsigned ms) {
    if (cancelled()) return;
    unsigned sleep = ms;
    if (deadline_active_) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline_ - std::chrono::steady_clock::now()).count();
        if (left <= 0) return;
        sleep = std::min<unsigned>(sleep, static_cast<unsigned>(left));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep));
}
bool LibusbW3u3Hardware::cancelled() const { return stop_requested_.load(); }
bool LibusbW3u3Hardware::expired() const {
    return deadline_active_ && std::chrono::steady_clock::now() >= deadline_;
}

Result<void> LibusbW3u3Hardware::prepare(
    std::uint8_t receiver, System system, const std::atomic<bool>& cancelled_flag) noexcept {
    if (!frontend_io_allowed_after_cleanup(cleanup_failed_))
        return Result<void>::failure(Error::USB_IO);
    if (!claimed_ || receiver != 1U || system != System::ISDB_T || !tuned_ ||
        source_prepared_ || stop_requested_.load())
        return Result<void>::failure(Error::UNSUPPORTED);
    capture_interrupted_.store(false);
    link_diagnostic_ = LinkSeedDiagnostic{};
    link_apply_attempted_ = false;
    output_start_attempted_ = false;
    if (cancelled_flag.load()) return Result<void>::failure(Error::NOT_READY);
    if (!snapshot_link_diagnostic()) return Result<void>::failure(Error::NOT_READY);
    link_snapshot_valid_ = true;
    cf_snapshot_valid_ = read_cf_block(1U, cf_snapshot_.data(), cf_snapshot_.size());
    if (!cf_snapshot_valid_) return Result<void>::failure(Error::USB_IO);
    std::size_t random_offset = 0U;
    while (random_offset < link_seed_.size()) {
        const ssize_t received = getrandom(link_seed_.data() + random_offset,
                                           link_seed_.size() - random_offset, 0);
        if (received < 0 && errno == EINTR) continue;
        if (received <= 0) return Result<void>::failure(Error::INTERNAL);
        random_offset += static_cast<std::size_t>(received);
    }
    if (!run_plan(plan_stream_setup(1U, 1U), 5000U)) return Result<void>::failure(Error::USB_IO);
    if (!set_cf_bit(0x03U, true)) return Result<void>::failure(Error::USB_IO);
    if (!async_) async_ = std::make_unique<AsyncState>();
    AsyncState& state = *async_;
    state = AsyncState{};
    for (auto& slot : state.slots) {
        slot.owner = &state;
        slot.transfer = libusb_alloc_transfer(0);
        if (slot.transfer == nullptr) return Result<void>::failure(Error::INTERNAL);
        libusb_fill_bulk_transfer(slot.transfer, primary_.handle(), 0x82U,
                                  slot.buffer.data(), static_cast<int>(slot.buffer.size()),
                                  AsyncState::completed, &slot, 0U);
    }
    state.prepared = true;
    for (auto& slot : state.slots) {
        slot.pending = true;
        ++slot.generation;
        if (libusb_submit_transfer(slot.transfer) != 0) {
            slot.pending = false;
            const auto cleaned = stop_and_drain(false);
            if (cleaned == CaptureRunResult::fatal_drain) fatal_drain_exit();
            if (cleaned != CaptureRunResult::cancelled) cleanup_failed_ = true;
            return Result<void>::failure(Error::USB_IO);
        }
    }
    dsc_attempted_ = true;
    dsc_stopped_ = false;
    if (!dsc_start(1U)) return Result<void>::failure(Error::USB_IO);
    if (!set_cf_bit(0x08U, true))
        return Result<void>::failure(Error::USB_IO);
    link_apply_attempted_ = true;
    output_start_attempted_ = true;
    if (!apply_link_seed()) return Result<void>::failure(Error::USB_IO);
    decoder_.clear();
    source_prepared_ = true;
    decoder_ = TransportCaptureDecoderV7(link_seed_.data(), link_seed_.size());
    link_seed_.fill(0);
    return Result<void>::success();
}

CaptureRunResult LibusbW3u3Hardware::run(
    const std::atomic<bool>& cancelled_flag,
    bool (*emit)(void*, const std::uint8_t*, std::size_t), void* context) noexcept {
    if (cleanup_failed_ || !source_prepared_ || emit == nullptr)
        return CaptureRunResult::usb_error;
    AsyncState& state = *async_;
    CaptureRunResult outcome = CaptureRunResult::cancelled;
    while (!cancelled_flag.load() && !stop_requested_.load() &&
           !capture_interrupted_.load()) {
        if (state.drain_failed) { outcome = CaptureRunResult::usb_error; break; }
        if (state.count == 0U) {
            if (!handle_events(100U)) {
                outcome = disconnected_.load() ? CaptureRunResult::disconnected
                                               : CaptureRunResult::usb_error;
                break;
            }
            continue;
        }
        const std::size_t index = state.ready[state.read];
        state.read = (state.read + 1U) % kQueueDepth;
        --state.count;
        AsyncState::Slot& slot = state.slots[index];
        slot.ready = false;
        if (slot.status != LIBUSB_TRANSFER_COMPLETED && slot.status != LIBUSB_TRANSFER_TIMED_OUT) {
            outcome = disconnected_.load() ? CaptureRunResult::disconnected
                                           : CaptureRunResult::usb_error;
            break;
        }
        if (slot.actual > 0) {
            auto packets = decoder_.push(slot.buffer.data(), static_cast<std::size_t>(slot.actual));
            if (!packets.empty() && !emit(context, packets.data(), packets.size())) {
                outcome = CaptureRunResult::cancelled;
                break;
            }
        }
        slot.actual = 0;
        slot.ready = false;
        slot.pending = true;
        ++slot.generation;
        if (libusb_submit_transfer(slot.transfer) != 0) {
            slot.pending = false;
            outcome = CaptureRunResult::usb_error;
            break;
        }
    }
    const CaptureRunResult cleanup = stop_and_drain(dsc_attempted_);
    if (cleanup == CaptureRunResult::fatal_drain) return cleanup;
    if (cleanup != CaptureRunResult::cancelled) {
        cleanup_failed_ = true;
        return cleanup;
    }
    source_prepared_ = false;
    if (outcome == CaptureRunResult::usb_error || outcome == CaptureRunResult::disconnected)
        return outcome;
    return CaptureRunResult::cancelled;
}

void LibusbW3u3Hardware::interrupt() noexcept {
    capture_interrupted_.store(true);
    if (context_ != nullptr) libusb_interrupt_event_handler(context_);
}

Result<void> LibusbW3u3Hardware::stop() noexcept {
    if (!claimed_) return Result<void>::failure(Error::NOT_READY);
    if (source_prepared_ || (async_ && async_->prepared) ||
        cf_snapshot_valid_ || link_snapshot_valid_) {
        const auto result = stop_and_drain(dsc_attempted_);
        if (result == CaptureRunResult::fatal_drain) fatal_drain_exit();
        if (result != CaptureRunResult::cancelled) {
            cleanup_failed_ = true;
            return Result<void>::failure(Error::USB_IO);
        }
        source_prepared_ = false;
    }
    return cleanup_failed_ ? Result<void>::failure(Error::USB_IO)
                           : Result<void>::success();
}

CaptureRunResult LibusbW3u3Hardware::stop_and_drain(bool dsc_was_attempted) noexcept {
    DrainAdapter ops(*this);
    const CaptureRunResult result = drain_capture_callbacks(
        ops, dsc_was_attempted, dsc_stopped_, kDrainLimit);
    if (result != CaptureRunResult::cancelled) cleanup_failed_ = true;
    return result;
}

CaptureRunResult LibusbW3u3Hardware::cleanup_after_drain(
    bool dsc_ok, bool dsc_was_attempted) noexcept {
    DrainAdapter ops(*this);
    const CaptureRunResult result = cleanup_capture_state(
        ops, dsc_ok, dsc_was_attempted, link_apply_attempted_,
        output_start_attempted_, cf_snapshot_valid_);
    const bool cleaned = result == CaptureRunResult::cancelled;
    decoder_.clear();
    link_seed_.fill(0U);
    if (cleaned || disconnected_.load()) {
        link_snapshot_valid_ = false;
        cf_snapshot_valid_ = false;
        link_apply_attempted_ = false;
        output_start_attempted_ = false;
        link_diagnostic_ = LinkSeedDiagnostic{};
    } else {
        cleanup_failed_ = true;
    }
    return result;
}

bool LibusbW3u3Hardware::handle_events(unsigned timeout_ms) noexcept {
    if (context_ == nullptr) return false;
    timeval timeout{};
    timeout.tv_sec = static_cast<long>(timeout_ms / 1000U);
    timeout.tv_usec = static_cast<long>((timeout_ms % 1000U) * 1000U);
    const int rc = libusb_handle_events_timeout_completed(context_, &timeout, nullptr);
    if (rc == LIBUSB_ERROR_NO_DEVICE) disconnected_.store(true);
    return rc >= 0;
}

bool LibusbW3u3Hardware::dsc_start(std::uint8_t local) {
    const auto transfer = make_dsc_control(local, false, 1000U);
    std::array<unsigned char, 1> response{};
    return control(transfer, response.data()) == transfer.length && response[0] == 1U;
}
bool LibusbW3u3Hardware::dsc_stop(std::uint8_t local) {
    const auto transfer = make_dsc_control(local, true, 1000U);
    std::array<unsigned char, 1> response{};
    return control(transfer, response.data()) == transfer.length && response[0] == 1U;
}
CaptureIo LibusbW3u3Hardware::bulk_read(std::uint8_t, unsigned char*, int, int*, unsigned) {
    return CaptureIo::Error;
}

bool LibusbW3u3Hardware::read_cf40(std::uint8_t local, std::uint8_t* value) {
    if (value == nullptr || local > 1U) return false;
    const auto transfer = make_cf_read(local, 0x40U, 1U, 1000U);
    std::array<unsigned char, 2> response{};
    return control(transfer, response.data()) == transfer.length &&
           ((*value = response[1]), true);
}
bool LibusbW3u3Hardware::write_cf40(std::uint8_t local, std::uint8_t value) {
    ControlTransfer transfer{};
    if (!make_cf_write(local, 0x40U, &value, 1U, &transfer, 1000U)) return false;
    std::array<unsigned char, 2> response{};
    return control(transfer, response.data()) == transfer.length;
}
bool LibusbW3u3Hardware::read_cf_block(std::uint8_t local, std::uint8_t* data,
                                       std::size_t size) {
    if (data == nullptr || local != 1U || size != 0x45U) return false;
    for (std::size_t offset = 0; offset < size;) {
        const std::size_t chunk = std::min<std::size_t>(0x20U, size - offset);
        const auto transfer = make_cf_read(local, static_cast<std::uint8_t>(offset),
                                           static_cast<std::uint16_t>(chunk), 1000U);
        std::array<unsigned char, 0x21> response{};
        if (control(transfer, response.data()) != transfer.length) return false;
        std::copy_n(response.begin() + 1, chunk, data + offset);
        offset += chunk;
    }
    return true;
}
bool LibusbW3u3Hardware::write_cf_block(std::uint8_t local, const std::uint8_t* data,
                                        std::size_t size) {
    if (data == nullptr || local != 1U || size != 0x45U) return false;
    for (std::size_t offset = 0; offset < size;) {
        const std::size_t chunk = std::min<std::size_t>(3U, size - offset);
        ControlTransfer transfer{};
        if (!make_cf_write(local, static_cast<std::uint8_t>(offset), data + offset,
                           chunk, &transfer, 1000U)) return false;
        std::array<unsigned char, 4> response{};
        if (control(transfer, response.data()) != transfer.length) return false;
        offset += chunk;
    }
    return true;
}
bool LibusbW3u3Hardware::terrestrial_locked(
    std::uint8_t local, bool* locked,
    std::chrono::steady_clock::time_point deadline) {
    if (local != 1U || locked == nullptr || std::chrono::steady_clock::now() >= deadline)
        return false;
    FrontendRunReport report{};
    if (!run_plan(plan_terrestrial_lock_read(557142U), 500U, &report) || !report.have_last_read)
        return false;
    *locked = (report.last_read & 0x0fU) == 0x09U;
    return true;
}
bool LibusbW3u3Hardware::set_cf_bit(std::uint8_t mask, bool value) noexcept {
    std::uint8_t current = 0;
    if (!read_cf40(1U, &current)) return false;
    const std::uint8_t next = value ? static_cast<std::uint8_t>(current | mask)
                                    : static_cast<std::uint8_t>(current & ~mask);
    return next == current || write_cf40(1U, next);
}
bool LibusbW3u3Hardware::snapshot_link_diagnostic() {
    std::uint8_t type_reg = 0;
    return verify_device_revision() &&
           read_i2c(0x4aU, 0x09U, 1U, &type_reg) &&
           ((type_reg & 0x3eU) >> 1U) == 0x0fU &&
           link_diagnostic_.snapshot_idle(this);
}
bool LibusbW3u3Hardware::verify_device_revision() noexcept {
    ControlTransfer rev{0, Request::SysCtrlRead, 2, 0, 3, Direction::In, 1000};
    std::array<unsigned char, 3> response{};
    return control(rev, response.data()) == rev.length && response[0] == 1U &&
           response[1] == 0x11U && response[2] == 0x52U;
}
LibusbW3u3Hardware::PoweredControllerCheck
LibusbW3u3Hardware::verify_powered_controller() noexcept {
    std::uint8_t type = 0xffU;
    std::uint8_t state = 0xffU;
    if (!read_i2c(0x4aU, 0x09U, 1U, &type))
        return PoweredControllerCheck::type_read_failed;
    if (((type & 0x3eU) >> 1U) != 0x0fU)
        return PoweredControllerCheck::unsupported_type;
    if (!read_controller05(&state))
        return PoweredControllerCheck::output_state_read_failed;
    return state == 0U ? PoweredControllerCheck::ready
                       : PoweredControllerCheck::output_busy;
}
bool LibusbW3u3Hardware::apply_link_seed() {
    return link_diagnostic_.apply(this, link_seed_.data(), link_seed_.size());
}
bool LibusbW3u3Hardware::clear_link_seed_and_verify_controller() {
    return link_diagnostic_.clear_and_verify_controller(this);
}
bool LibusbW3u3Hardware::read_controller05(std::uint8_t* value) {
    std::uint8_t local = 0;
    if (value == nullptr) value = &local;
    return read_i2c(0x4aU, 0x05U, 1U, value);
}
bool LibusbW3u3Hardware::read_controller05_value_zero() {
    std::uint8_t value = 0xff;
    return read_controller05(&value) && value == 0U;
}
bool LibusbW3u3Hardware::write_controller05(std::uint8_t value) {
    return write_i2c_byte(0x4aU, 0x05U, value);
}
bool LibusbW3u3Hardware::write_link_seed_byte(std::uint8_t reg, std::uint8_t value) {
    return reg >= 0x10U && reg <= 0x1fU && write_i2c_byte(0x4aU, reg, value);
}
bool LibusbW3u3Hardware::read_i2c(std::uint8_t slave, std::uint8_t reg,
                                  std::uint16_t length, std::uint8_t* output) noexcept {
    if (output == nullptr || length == 0U || length > 0x20U) return false;
    const auto transfer = make_i2c_read(slave, reg, length, 0U, 1000U);
    std::array<unsigned char, 0x21> response{};
    const int rc = control(transfer, response.data());
    if (rc != transfer.length || !parse_status_response(response.data(), static_cast<std::size_t>(rc), nullptr, 0))
        return false;
    std::copy_n(response.begin() + 1, length, output);
    return true;
}
bool LibusbW3u3Hardware::write_i2c_byte(std::uint8_t slave, std::uint8_t reg,
                                        std::uint8_t value) noexcept {
    ControlTransfer transfer{};
    if (!make_i2c_write_chunk(slave, reg, &value, 1U, false, &transfer, 1000U)) return false;
    std::array<unsigned char, 2> response{};
    const int rc = control(transfer, response.data());
    return rc == transfer.length && parse_status_response(response.data(), static_cast<std::size_t>(rc), nullptr, 0);
}

}  // namespace asicen
