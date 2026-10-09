// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/libusb_hardware_backend.h"

#include "asicen/device_profile.h"
#include "asicen/hardware_gpio_guard.h"
#include "asicen/secure_entropy.h"
#include "asicen/transport_transform.h"
#include "asicen/write_protocol.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/resource.h>
#include <sys/time.h>
#include <thread>

namespace asicen {
namespace {
using px4::userland::Error;
using px4::userland::Result;
using px4::userland::ipc::System;

constexpr std::size_t kQueueDepth = 4U;
constexpr std::size_t kChunkSize = 4096U;
constexpr std::chrono::milliseconds kDrainLimit{2000};
SatelliteOperationResult v2_satellite_result(V2FrontendResult result) noexcept {
    switch (result) {
        case V2FrontendResult::Completed: return SatelliteOperationResult::Completed;
        case V2FrontendResult::InvalidArgument:
        case V2FrontendResult::UnsupportedChip: return SatelliteOperationResult::InvalidArgument;
        case V2FrontendResult::ShortTransfer: return SatelliteOperationResult::ShortTransfer;
        case V2FrontendResult::Cancelled: return SatelliteOperationResult::Cancelled;
        case V2FrontendResult::DeadlineExceeded:
        case V2FrontendResult::NotLocked: return SatelliteOperationResult::DeadlineExceeded;
        case V2FrontendResult::FailedTransfer: return SatelliteOperationResult::FailedTransfer;
    }
    return SatelliteOperationResult::FailedTransfer;
}

Error satellite_error(SatelliteOperationResult result) noexcept {
    switch (result) {
        case SatelliteOperationResult::Completed: return Error::OK;
        case SatelliteOperationResult::InvalidArgument: return Error::INVALID_ARGUMENT;
        case SatelliteOperationResult::Cancelled: return Error::NOT_READY;
        case SatelliteOperationResult::DeadlineExceeded: return Error::TIMEOUT;
        case SatelliteOperationResult::FailedTransfer:
        case SatelliteOperationResult::ShortTransfer: return Error::USB_IO;
        case SatelliteOperationResult::VerificationFailed: return Error::PROTOCOL_ERROR;
    }
    return Error::USB_IO;
}

std::uint8_t receiver_local(std::uint8_t receiver) noexcept { return receiver; }

[[noreturn]] void fatal_drain_exit() {
    const rlimit no_core{0, 0};
    (void)::setrlimit(RLIMIT_CORE, &no_core);
    std::_Exit(70);
}

class AtomicFlagScope {
public:
    explicit AtomicFlagScope(std::atomic<bool>& flag) noexcept
        : flag_(flag), previous_(flag.exchange(true, std::memory_order_acq_rel)) {}
    ~AtomicFlagScope() { flag_.store(previous_, std::memory_order_release); }
    AtomicFlagScope(const AtomicFlagScope&) = delete;
    AtomicFlagScope& operator=(const AtomicFlagScope&) = delete;

private:
    std::atomic<bool>& flag_;
    bool previous_;
};

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
    std::mutex mutex;

    static void LIBUSB_CALL completed(libusb_transfer* transfer) {
        auto* slot = static_cast<Slot*>(transfer->user_data);
        if (slot == nullptr || slot->owner == nullptr) return;
        AsyncState& state = *slot->owner;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            slot->pending = false;
            slot->status = static_cast<int>(transfer->status);
            slot->actual = transfer->actual_length;
            slot->ready = true;
            if (state.count == kQueueDepth) {
                state.drain_failed = true;
                return;
            }
            state.ready[state.write] = static_cast<std::size_t>(slot - state.slots.data());
            state.write = (state.write + 1U) % kQueueDepth;
            ++state.count;
        }
    }
};

struct LibusbW3u3Hardware::DrainAdapter final : CaptureDrainOps,
                                                CaptureCleanupOps {
    explicit DrainAdapter(LibusbW3u3Hardware& owner) : owner_(owner) {}
    bool stop_dsc() noexcept override {
        if (owner_.source_receiver_ > 1U) return false;
        const std::uint8_t local = receiver_local(owner_.source_receiver_);
        const bool ok = owner_.dsc_stop(local);
        owner_.dsc_stopped_ = ok;
        return ok;
    }
    void cancel_pending() noexcept override {
        if (!owner_.async_) return;
        std::array<libusb_transfer*, kQueueDepth> transfers{};
        {
            std::lock_guard<std::mutex> lock(owner_.async_->mutex);
            for (std::size_t i = 0; i < owner_.async_->slots.size(); ++i) {
                const auto& slot = owner_.async_->slots[i];
                if (slot.transfer != nullptr && slot.pending) transfers[i] = slot.transfer;
            }
        }
        for (auto* transfer : transfers)
            if (transfer != nullptr) (void)owner_.cancel_transfer(transfer);
    }
    bool has_pending() const noexcept override {
        if (!owner_.async_) return false;
        std::lock_guard<std::mutex> lock(owner_.async_->mutex);
        for (const auto& slot : owner_.async_->slots)
            if (slot.pending) return true;
        return false;
    }
    void pump_events(unsigned timeout_ms) noexcept override {
        (void)owner_.handle_events(timeout_ms);
    }
    void release_transfers() noexcept override {
        if (!owner_.async_) return;
        std::array<libusb_transfer*, kQueueDepth> transfers{};
        {
            std::lock_guard<std::mutex> lock(owner_.async_->mutex);
            for (std::size_t i = 0; i < owner_.async_->slots.size(); ++i) {
                auto& slot = owner_.async_->slots[i];
                transfers[i] = slot.transfer;
                slot.transfer = nullptr;
                slot.pending = false;
            }
            owner_.async_->prepared = false;
            owner_.async_->count = 0U;
            owner_.async_->read = 0U;
            owner_.async_->write = 0U;
            owner_.async_->drain_failed = false;
        }
        for (auto* transfer : transfers)
            if (transfer != nullptr) owner_.free_transfer(transfer);
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
        if (owner_.source_receiver_ > 1U) return false;
        const std::uint8_t local = receiver_local(owner_.source_receiver_);
        const bool written = owner_.write_cf_block(local, owner_.cf_snapshot_.data(),
                                                    owner_.cf_snapshot_.size());
        std::array<std::uint8_t, 0x45> verify{};
        return written && owner_.read_cf_block(local, verify.data(), verify.size()) &&
               verify == owner_.cf_snapshot_;
    }
private:
    LibusbW3u3Hardware& owner_;
};

LibusbW3u3Hardware::LibusbW3u3Hardware(
    libusb_context* context, UsbLocation primary, UsbLocation sibling,
    std::vector<std::uint8_t> primary_path,
    std::vector<std::uint8_t> sibling_path,
    const DeviceProfile* expected_profile)
    : context_(context), primary_claim_(primary_), sibling_claim_(sibling_),
      primary_path_(std::move(primary_path)),
      sibling_path_(std::move(sibling_path)), async_(std::make_unique<AsyncState>()),
      decoder_(nullptr, 0U) {
    if (expected_profile != nullptr) {
        const auto* known = find_profile(expected_profile->vid, expected_profile->pid);
        if (known != nullptr && known->model_id == expected_profile->model_id &&
            known->frontend_family == expected_profile->frontend_family &&
            known->source_supported == expected_profile->source_supported)
            profile_ = known;
    }
    if (context_ != nullptr && primary_.open(context_, primary) == 0) {
        if (expected_profile == nullptr) {
            libusb_device_descriptor descriptor{};
            if (libusb_get_device_descriptor(primary_.device(), &descriptor) == 0)
                profile_ = find_profile(descriptor.idVendor, descriptor.idProduct);
        }
        if (profile_ != nullptr && profile_->expected_runtime_functions == 2U)
            (void)sibling_.open(context_, sibling);
        // Handles remain open until release(), including after stream cleanup.
    }
}

LibusbW3u3Hardware::~LibusbW3u3Hardware() noexcept {
    // Destruction is only reached after normal shutdown. Never issue frontend
    // cleanup writes here because the USB address could already have changed.
    (void)release();
}

Result<void> LibusbW3u3Hardware::claim() {
    // Reject unsupported or mismatched families before claims, GPIO reads,
    // revision/controller requests, or any other vendor transfer.
    if (!runtime_model_supported()) return Result<void>::failure(Error::UNSUPPORTED);
    if (context_ == nullptr || !primary_.is_open() ||
        (profile_->expected_runtime_functions == 2U && !sibling_.is_open()))
        return Result<void>::failure(Error::DISCONNECTED);
    const auto result = ownership_.claim_profile(
        *profile_, primary_claim_,
        profile_->expected_runtime_functions == 2U ? &sibling_claim_ : nullptr,
        primary_path_, sibling_path_);
    if (result != OwnershipError::none)
        return Result<void>::failure(result == OwnershipError::primary_claim_failed ||
                                             result == OwnershipError::sibling_claim_failed
                                         ? Error::BUSY
                                         : Error::INVALID_ARGUMENT);
    claimed_ = true;
    if (!verify_device_revision()) {
        std::fprintf(stderr, "asicend: bridge revision unavailable or unsupported (requires 11/52; 16/52 is not implemented)\n");
        (void)release();
        return Result<void>::failure(Error::UNSUPPORTED);
    }
    if (uses_v2_frontend() && !verify_v2_pair_roles()) {
        std::fprintf(stderr, "asicend: V2 requires role0/role1 runtime functions, both revision 11/52\n");
        (void)release();
        return Result<void>::failure(Error::UNSUPPORTED);
    }
    if (!snapshot_gpio_state()) {
        (void)release();
        return Result<void>::failure(Error::USB_IO);
    }
    return Result<void>::success();
}

Result<void> LibusbW3u3Hardware::release() noexcept {
    const OwnershipError result = ownership_.release();
    const bool ok = result == OwnershipError::none;
    if (ok) {
        claimed_ = false;
        v2_roles_verified_ = false;
        v2_identity_verified_ = false;
        primary_.close();
        sibling_.close();
    }
    return ok ? Result<void>::success() : Result<void>::failure(Error::USB_IO);
}

SatelliteProbeSummary LibusbW3u3Hardware::probe_satellite(
    std::uint32_t rf_khz, bool select_slot, std::size_t slot,
    const volatile std::sig_atomic_t* stop_flag) noexcept {
    SatelliteProbeSummary summary{};
    if (!runtime_model_supported() || !claimed_ || cleanup_failed_.load() ||
        !is_w3u3_satellite_rf_khz(rf_khz) ||
        (select_slot && slot >= kW3u3SatelliteTsidSlots)) {
        summary.result = SatelliteOperationResult::InvalidArgument;
        return summary;
    }
    if (stop_flag != nullptr && *stop_flag != 0) {
        summary.result = SatelliteOperationResult::Cancelled;
        return summary;
    }

    const auto absolute_deadline = steady_now() +
                                   std::chrono::seconds(60);
    diagnostic_stop_flag_ = stop_flag;
    deadline_ = absolute_deadline;
    deadline_active_ = true;
    const auto opened = open_receiver(0U);
    if (!opened) {
        if (cancelled()) summary.result = SatelliteOperationResult::Cancelled;
        else if (steady_now() >= absolute_deadline)
            summary.result = SatelliteOperationResult::DeadlineExceeded;
        else summary.result = SatelliteOperationResult::FailedTransfer;
        deadline_active_ = false;
        diagnostic_stop_flag_ = nullptr;
        return summary;
    }

    // open_receiver owns a separate bounded startup deadline. Restore the
    // diagnostic's one absolute deadline for tune, lock polling and TSID I/O.
    deadline_ = absolute_deadline;
    deadline_active_ = true;
    summary.result = run_model_satellite_tune(rf_khz);
    if (summary.result != SatelliteOperationResult::Completed) {
        deadline_active_ = false;
        diagnostic_stop_flag_ = nullptr;
        return summary;
    }

    const auto lock_deadline = std::min(absolute_deadline,
        steady_now() + std::chrono::seconds(5));
    deadline_ = lock_deadline;
    const SatelliteLockResult lock = poll_model_satellite_lock();
    summary.result = lock.result;
    summary.locked = lock.locked;

    if (summary.result == SatelliteOperationResult::Completed && summary.locked) {
        deadline_ = absolute_deadline;
        const auto list = wait_model_satellite_tsid(
            select_slot ? slot : kW3u3SatelliteTsidSlots, 0U, false);
        summary.result = list.result;
        if (list.result == SatelliteOperationResult::Completed) {
            for (const std::uint16_t tsid : list.tsids) {
                if (tsid != 0U && tsid != kW3u3SatelliteNoTsid)
                    ++summary.nonempty_tsid_slots;
            }
            if (select_slot) {
                const auto selected = select_model_satellite_tsid(slot, list.tsids);
                summary.result = selected.result;
                summary.selected_slot = selected.result == SatelliteOperationResult::Completed;
            }
        }
    }
    deadline_active_ = false;
    diagnostic_stop_flag_ = nullptr;
    return summary;
}

CardProbeSummary LibusbW3u3Hardware::probe_card(
    const volatile std::sig_atomic_t* stop_flag) noexcept {
    CardProbeSummary summary{};
    summary.error = Error::UNSUPPORTED;
    if (!runtime_model_supported() || !claimed_ || cleanup_failed_.load()) return summary;
    if (stop_flag != nullptr && *stop_flag != 0) {
        summary.error = Error::TIMEOUT;
        return summary;
    }

    const auto absolute_deadline = steady_now() +
                                   std::chrono::seconds(30);
    diagnostic_stop_flag_ = stop_flag;
    deadline_ = absolute_deadline;
    deadline_active_ = true;
    const auto opened = open_receiver(profile_->combined_isdb_ts ? 0U : 1U);
    if (!opened) {
        summary.error = opened.error();
        deadline_active_ = false;
        diagnostic_stop_flag_ = nullptr;
        return summary;
    }
    deadline_ = absolute_deadline;
    deadline_active_ = true;

    W3u3CardMailboxHardware mailbox(*this, profile_->model_id);
    px4::userland::CardSession session(mailbox, mailbox);
    const auto atr = px4::userland::reset_and_read_card_atr(mailbox, mailbox);
    if (!atr) {
        summary.error = atr.error();
    } else {
        summary.atr_valid = true;
        summary.atr_length = atr.value().length;
        if (atr.value().edc != px4::userland::CardEdc::lrc ||
            atr.value().baud_rate != px4::userland::It930xCardBaudRate::baud_19200) {
            summary.error = Error::UNSUPPORTED;
        } else {
            const auto initialized = session.initialize_with_atr(atr.value());
            if (!initialized) {
                summary.error = initialized.error();
            } else {
                const std::array<std::uint8_t, 5> apdu{{0x90U, 0x30U, 0x00U, 0x00U, 0x00U}};
                std::array<std::uint8_t, 258> response{};
                const auto transmitted = session.transmit(
                    px4::userland::ByteView{apdu.data(), apdu.size()},
                    px4::userland::MutableByteView{response.data(), response.size()});
                if (!transmitted) {
                    summary.error = transmitted.error();
                } else if (transmitted.value() < 2U) {
                    summary.error = Error::PROTOCOL_ERROR;
                } else {
                    summary.response_length = transmitted.value();
                    summary.status_word = static_cast<std::uint16_t>(
                        (static_cast<std::uint16_t>(response[transmitted.value() - 2U]) << 8U) |
                        response[transmitted.value() - 1U]);
                    summary.error = transmitted.value() >= 57U && summary.status_word == 0x9000U
                        ? Error::OK : Error::PROTOCOL_ERROR;
                }
                volatile std::uint8_t* response_bytes = response.data();
                for (std::size_t i = 0U; i < response.size(); ++i)
                    response_bytes[i] = 0U;
            }
        }
    }

    // Cleanup gets a fresh finite budget and ignores the signal cancellation,
    // while the first operation error remains the reported result.
    diagnostic_stop_flag_ = nullptr;
    deadline_ = steady_now() + std::chrono::seconds(2);
    deadline_active_ = true;
    const auto card_cleanup = mailbox.shutdown_controller();
    if (!card_cleanup) {
        mark_cleanup_failed(Error::USB_IO);
        if (summary.error == Error::OK) summary.error = card_cleanup.error();
    }
    deadline_active_ = false;
    diagnostic_stop_flag_ = nullptr;
    return summary;
}

bool LibusbW3u3Hardware::runtime_model_supported() const noexcept {
    return profile_ != nullptr && profile_runtime_supported(*profile_) &&
           (profile_->frontend_family == FrontendFamily::W3u3 ||
            profile_->frontend_family == FrontendFamily::S3u ||
            profile_->frontend_family == FrontendFamily::S3u2 ||
            profile_->frontend_family == FrontendFamily::Nmi);
}

std::uint8_t LibusbW3u3Hardware::receiver_count() const noexcept {
    // Secondary capture needs independent snapshots, routed I/O and cleanup.
    // Do not advertise it merely because the enclosure contains four tuners.
    return runtime_model_supported() ?
        (profile_->combined_isdb_ts ? 1U : 2U) : 0U;
}
bool LibusbW3u3Hardware::receiver_supports(std::uint8_t receiver, System system) const noexcept {
    return runtime_model_supported() && receiver < receiver_count() &&
           ((profile_->combined_isdb_ts &&
             (system == System::ISDB_S || system == System::ISDB_T)) ||
            (receiver == 0U && system == System::ISDB_S) ||
            (receiver == 1U && system == System::ISDB_T));
}
bool LibusbW3u3Hardware::selects_satellite_stream_before_tune() const noexcept { return false; }
bool LibusbW3u3Hardware::requires_terrestrial_lock_settle() const noexcept {
    return runtime_model_supported() && !uses_legacy_frontend() && !uses_v2_frontend();
}

SatelliteOperationResult LibusbW3u3Hardware::run_model_satellite_tune(std::uint32_t rf) noexcept {
    if (uses_v2_frontend()) {
        if (!v2_identity_verified_) return SatelliteOperationResult::InvalidArgument;
        return v2_satellite_result(tune_v2_frontend(this, {1U}, rf));
    }
    return uses_legacy_frontend()
        ? run_legacy_satellite_tune(legacy_frontend(), this, rf)
        : run_w3u3_satellite_tune(this, rf);
}

SatelliteLockResult LibusbW3u3Hardware::read_model_satellite_lock() noexcept {
    if (uses_v2_frontend()) {
        SatelliteLockResult result{};
        result.result = v2_satellite_result(read_v2_frontend_lock(this, {1U}, &result.locked));
        return result;
    }
    return uses_legacy_frontend()
        ? read_legacy_satellite_lock(legacy_frontend(), this)
        : read_w3u3_satellite_lock(this);
}

SatelliteLockResult LibusbW3u3Hardware::poll_model_satellite_lock() noexcept {
    if (!uses_v2_frontend())
        return uses_legacy_frontend()
            ? poll_legacy_satellite_lock(legacy_frontend(), this)
            : poll_w3u3_satellite_lock(this);
    SatelliteLockResult result{};
    for (unsigned attempt = 0; attempt < 50U; ++attempt) {
        if (cancelled()) { result.result = SatelliteOperationResult::Cancelled; break; }
        if (expired()) { result.result = SatelliteOperationResult::DeadlineExceeded; break; }
        result = read_model_satellite_lock();
        if (result.result != SatelliteOperationResult::Completed || result.locked) break;
        delay_ms(100U);
    }
    return result;
}

SatelliteTsidReadyResult LibusbW3u3Hardware::wait_model_satellite_tsid(
    std::size_t slot, std::uint16_t tsid, bool by_value) noexcept {
    if (!uses_v2_frontend()) {
        if (uses_legacy_frontend()) {
            if (by_value) return wait_legacy_satellite_tsid_ready(legacy_frontend(), this, tsid);
            return slot < kW3u3SatelliteTsidSlots
                ? wait_legacy_satellite_slot_ready(legacy_frontend(), this, slot)
                : wait_legacy_satellite_any_ready(legacy_frontend(), this);
        }
        if (by_value) return wait_w3u3_satellite_tsid_ready(this, tsid);
        return slot < kW3u3SatelliteTsidSlots
            ? wait_w3u3_satellite_slot_ready(this, slot) : wait_w3u3_satellite_any_ready(this);
    }
    SatelliteTsidReadyResult result{};
    for (unsigned attempt = 0U; attempt < 1000U; ++attempt) {
        if (cancelled()) { result.result = SatelliteOperationResult::Cancelled; return result; }
        if (expired()) { result.result = SatelliteOperationResult::DeadlineExceeded; return result; }
        result.result = v2_satellite_result(read_v2_frontend_tsids(this, {1U}, &result.tsids));
        if (result.result != SatelliteOperationResult::Completed) return result;
        for (std::size_t i = 0; i < result.tsids.size(); ++i) {
            const auto value = result.tsids[i];
            if (value != 0U && value != kW3u3SatelliteNoTsid &&
                (by_value ? value == tsid : slot >= kW3u3SatelliteTsidSlots || i == slot)) {
                result.slot = i;
                return result;
            }
        }
        delay_ms(10U);
    }
    result.result = SatelliteOperationResult::DeadlineExceeded;
    return result;
}

SatelliteTsidSelectResult LibusbW3u3Hardware::select_model_satellite_tsid(
    std::size_t slot, const std::array<std::uint16_t, kW3u3SatelliteTsidSlots>& tsids) noexcept {
    if (!uses_v2_frontend())
        return uses_legacy_frontend()
            ? select_legacy_satellite_tsid(legacy_frontend(), this, slot, tsids)
            : select_w3u3_satellite_tsid(this, slot, tsids);
    SatelliteTsidSelectResult result{};
    if (slot >= tsids.size() || tsids[slot] == 0U || tsids[slot] == kW3u3SatelliteNoTsid)
        return result;
    result.result = v2_satellite_result(select_v2_frontend_tsid(this, {1U}, tsids[slot]));
    if (result.result == SatelliteOperationResult::Completed) result.selected_tsid = tsids[slot];
    return result;
}

bool LibusbW3u3Hardware::uses_legacy_frontend() const noexcept {
    return profile_ != nullptr &&
           (profile_->frontend_family == FrontendFamily::S3u ||
            profile_->frontend_family == FrontendFamily::S3u2);
}

bool LibusbW3u3Hardware::uses_v2_frontend() const noexcept {
    return profile_ != nullptr && profile_->frontend_family == FrontendFamily::Nmi;
}

bool LibusbW3u3Hardware::verify_v2_pair_roles() noexcept {
    v2_roles_verified_ = false;
    const ControlTransfer info{0U, Request::CustomerInfo, 0U, 0U,
                               kCustomerInfoSize, Direction::In, 1000U};
    for (std::uint8_t function = 0; function < 2U; ++function) {
        // Revision16 follows a different identity/link branch on either
        // function. Verify both before any shared-board startup write.
        const ControlTransfer revision{0U, Request::SysCtrlRead, 2U, 0U, 3U,
                                       Direction::In, 1000U};
        std::array<std::uint8_t, 3> revision_response{};
        if (control_function(function, revision, revision_response.data()) != revision.length ||
            revision_response[0] != 1U || revision_response[1] != 0x11U ||
            revision_response[2] != 0x52U) return false;
        std::array<std::uint8_t, kCustomerInfoSize> response{};
        if (control_function(function, info, response.data()) != info.length) return false;
        const auto route = v2_source_route(response.data(), response.size(), 0U);
        if (!route.valid || route.device_role != function || route.rf_master_role != 0U)
            return false;
    }
    v2_roles_verified_ = true;
    return true;
}

bool LibusbW3u3Hardware::verify_v2_pair_identity() noexcept {
    v2_identity_verified_ = false;
    if (!v2_roles_verified_) return false;
    std::array<std::array<std::uint8_t, 17>, 2> identities{};
    bool read_ok = true;
    for (std::uint8_t function = 0; function < 2U; ++function) {
        // The source first probes one byte, then waits before reading the
        // opaque board identity. These are post-power, read-only operations.
        const auto probe = make_i2c_read(0xa8U, 0xb0U, 1U, 0U, 1000U);
        std::array<std::uint8_t, 2> response{};
        if (control_function(function, probe, response.data()) != probe.length ||
            response[0] != 1U) { read_ok = false; break; }
        delay_ms(10U);
        const auto read = make_i2c_read(0xa8U, 0xb0U, 16U, 0U, 1000U);
        if (control_function(function, read, identities[function].data()) != read.length ||
            identities[function][0] != 1U) { read_ok = false; break; }
    }
    bool same = read_ok;
    for (std::size_t i = 1U; i < 17U; ++i)
        same = (identities[0][i] == identities[1][i]) && same;
    // Never retain or log the private board identifier.
    for (auto& identity : identities) {
        volatile std::uint8_t* bytes = identity.data();
        for (std::size_t i = 0; i < identity.size(); ++i) bytes[i] = 0U;
    }
    v2_identity_verified_ = same;
    return same;
}

LegacyFrontendProfile LibusbW3u3Hardware::legacy_frontend() const noexcept {
    return profile_->frontend_family == FrontendFamily::S3u
        ? LegacyFrontendProfile::S3u : LegacyFrontendProfile::S3u2;
}

Result<void> LibusbW3u3Hardware::open_receiver(std::uint8_t receiver) noexcept {
    if (!runtime_model_supported() || receiver >= receiver_count())
        return Result<void>::failure(Error::UNSUPPORTED);
    const ReceiverReservationResult reservation = active_receiver_.reserve(receiver);
    const bool newly_reserved = reservation == ReceiverReservationResult::reserved;
    if (reservation == ReceiverReservationResult::busy ||
        reservation == ReceiverReservationResult::invalid)
        return Result<void>::failure(Error::BUSY);
    const auto fail_open = [&](Error error) {
        if (newly_reserved) (void)active_receiver_.release(receiver);
        return Result<void>::failure(error);
    };
    const auto gate_deadline = steady_now() +
                               std::chrono::seconds(20);
    if (!acquire_control_gate(gate_deadline)) return fail_open(Error::TIMEOUT);
    std::unique_lock<std::recursive_timed_mutex> gate(control_gate_, std::adopt_lock);
    // Reserve before reading mutable session/cleanup state so a competing
    // receiver cannot race a capture teardown and start frontend I/O.
    if (!claimed_) return fail_open(Error::NOT_READY);
    if (uses_v2_frontend() && !v2_roles_verified_) return fail_open(Error::UNSUPPORTED);
    if (!frontend_io_allowed_after_cleanup(cleanup_failed_.load()))
        return fail_open(Error::USB_IO);
    if (receiver == 0U &&
        !ownership_.primary_supports_bulk_endpoint(kBulkEndpointLane0))
        return fail_open(Error::UNSUPPORTED);
    if (initialized_) return Result<void>::success();
    if (!verify_device_revision()) {
        std::fprintf(stderr, "asicend: bridge revision unavailable or unsupported (requires 11/52; 16/52 is not implemented)\n");
        return fail_open(Error::UNSUPPORTED);
    }
    if (!snapshot_gpio_state()) return fail_open(Error::USB_IO);
    FrontendPlan power_plan = uses_legacy_frontend()
        ? plan_legacy_frontend_startup(legacy_frontend())
        : uses_v2_frontend() ? plan_v2_revision11_startup_prefix() : plan_startup_subset();
    if (uses_legacy_frontend()) {
        const auto startup_off = plan_legacy_frontend_startup_off(legacy_frontend());
        power_plan.insert(power_plan.end(), startup_off.begin(), startup_off.end());
    }
    const auto power = uses_legacy_frontend()
        ? plan_legacy_frontend_power(legacy_frontend(), true)
        : uses_v2_frontend() ? plan_v2_shared_power_on() : plan_safe_power_on();
    if (!uses_v2_frontend() && !uses_legacy_frontend())
        power_plan.insert(power_plan.end(), power.begin(), power.end());
    const FrontendPlan init_plan = uses_legacy_frontend()
        ? plan_legacy_frontend_init(legacy_frontend())
        : uses_v2_frontend() ? FrontendPlan{} : plan_terrestrial_init_with_satellite_demod();

    // One deadline covers startup, the now-powered controller guard, and
    // demod initialization. Never issue controller I2C before this power
    // sequence: the device NACKs those accesses while its controller is off.
    deadline_ = steady_now() + std::chrono::seconds(20);
    deadline_active_ = true;
    PoweredControllerCheck controller_check = PoweredControllerCheck::ready;
    const PoweredInitResult result = execute_powered_init_sequence(
        [&] {
            board_power_attempted_ = true;
            if (run_frontend_plan(power_plan, this) != FrontendRunResult::Completed) return false;
            if (uses_legacy_frontend()) {
                // DTV_Init rechecks the silicon after DTV_Start's off phase.
                return verify_device_revision() &&
                    run_frontend_plan(plan_legacy_frontend_init_prelude(legacy_frontend()), this) ==
                        FrontendRunResult::Completed &&
                    run_frontend_plan(power, this) == FrontendRunResult::Completed;
            }
            if (!uses_v2_frontend()) return true;
            return verify_v2_pair_identity() &&
                run_frontend_plan(plan_v2_revision11_startup_tail(), this) ==
                    FrontendRunResult::Completed &&
                run_frontend_plan(power, this) == FrontendRunResult::Completed;
        },
        [&] {
            controller_check = verify_powered_controller();
            return controller_check == PoweredControllerCheck::ready;
        },
        [&] {
            if (!uses_v2_frontend())
                return run_frontend_plan(init_plan, this) == FrontendRunResult::Completed;
            // Role0 is the shared RF master for both role0/role1 frontends.
            // Initialize T0,S0,T1,S1, while exposing only primary USB lanes.
            for (std::uint8_t source = 0U; source < 4U; ++source)
                if (initialize_v2_frontend(this, {source}, nullptr, 4000U) !=
                    V2FrontendResult::Completed) return false;
            return true;
        });
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
        if (!restored) mark_cleanup_failed(Error::USB_IO);
        Error error = Error::USB_IO;
        if (result == PoweredInitResult::controller_guard_failed && restored) {
            if (controller_check == PoweredControllerCheck::unsupported_type)
                error = Error::UNSUPPORTED;
            else if (controller_check == PoweredControllerCheck::output_busy)
                error = Error::BUSY;
        }
        return fail_open(error);
    }
    initialized_ = true;
    return Result<void>::success();
}

Result<void> LibusbW3u3Hardware::tune_terrestrial(
    std::uint8_t receiver, std::uint32_t frequency_khz,
    std::uint32_t timeout_ms) noexcept {
    if (!frontend_io_allowed_after_cleanup(cleanup_failed_.load()))
        return Result<void>::failure(Error::USB_IO);
    if (!claimed_ || !receiver_supports(receiver, System::ISDB_T) ||
        !active_receiver_.owns(receiver) || !initialized_)
        return Result<void>::failure(Error::UNSUPPORTED);
    if (timeout_ms == 0U) return Result<void>::failure(Error::INVALID_ARGUMENT);
    if (uses_v2_frontend()) {
        if (!v2_identity_verified_ || v2_tune_frequency_hz({0U}, frequency_khz) == 0U)
            return Result<void>::failure(Error::INVALID_ARGUMENT);
        const auto until = steady_now() + std::chrono::milliseconds(timeout_ms);
        if (!acquire_control_gate(until)) return Result<void>::failure(Error::TIMEOUT);
        std::unique_lock<std::recursive_timed_mutex> gate(control_gate_, std::adopt_lock);
        const auto previous_deadline = deadline_;
        const bool previous_active = deadline_active_;
        deadline_ = previous_active ? std::min(previous_deadline, until) : until;
        deadline_active_ = true;
        const auto result = tune_v2_frontend(this, {0U}, frequency_khz, nullptr, timeout_ms);
        deadline_ = previous_deadline;
        deadline_active_ = previous_active;
        if (result != V2FrontendResult::Completed)
            return Result<void>::failure(satellite_error(v2_satellite_result(result)));
        tuned_ = true;
        tuned_receiver_ = receiver;
        tuned_system_ = System::ISDB_T;
        tuned_frequency_khz_ = frequency_khz;
        gain_applied_ = false;
        return Result<void>::success();
    }
    const auto plan = uses_legacy_frontend()
        ? plan_legacy_terrestrial_tune(legacy_frontend(), frequency_khz, 6U)
        : plan_terrestrial_tune_full(frequency_khz, 6U);
    if (plan.empty()) return Result<void>::failure(Error::INVALID_ARGUMENT);
    if (!run_plan(plan, timeout_ms))
        return Result<void>::failure(disconnected_.load() ? Error::DISCONNECTED : Error::USB_IO);
    tuned_ = true;
    tuned_receiver_ = receiver;
    tuned_system_ = System::ISDB_T;
    tuned_frequency_khz_ = frequency_khz;
    gain_applied_ = false;
    return Result<void>::success();
}

Result<void> LibusbW3u3Hardware::tune_satellite(
    std::uint8_t receiver, std::uint32_t frequency_khz,
    std::uint32_t timeout_ms) noexcept {
    if (!frontend_io_allowed_after_cleanup(cleanup_failed_.load()))
        return Result<void>::failure(Error::USB_IO);
    std::uint32_t rf_khz = 0U;
    if (!claimed_ || !receiver_supports(receiver, System::ISDB_S) ||
        !active_receiver_.owns(receiver) ||
        !initialized_ || timeout_ms == 0U ||
        !w3u3_satellite_if_to_rf_khz(frequency_khz, &rf_khz))
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    const auto gate_deadline = steady_now() +
                               std::chrono::milliseconds(timeout_ms);
    if (!acquire_control_gate(gate_deadline)) return Result<void>::failure(Error::TIMEOUT);
    std::unique_lock<std::recursive_timed_mutex> gate(control_gate_, std::adopt_lock);
    const auto previous_deadline = deadline_;
    const bool previous_deadline_active = deadline_active_;
    const auto selection_deadline = gate_deadline;
    deadline_ = previous_deadline_active
        ? std::min(previous_deadline, selection_deadline)
        : selection_deadline;
    deadline_active_ = true;
    const SatelliteOperationResult tuned = run_model_satellite_tune(rf_khz);
    deadline_ = previous_deadline;
    deadline_active_ = previous_deadline_active;
    const Error error = satellite_error(tuned);
    if (error != Error::OK) return Result<void>::failure(error);
    tuned_ = true;
    tuned_receiver_ = receiver;
    tuned_system_ = System::ISDB_S;
    gain_applied_ = false;
    return Result<void>::success();
}

Result<bool> LibusbW3u3Hardware::is_locked(std::uint8_t receiver,
                                           System system) noexcept {
    if (!frontend_io_allowed_after_cleanup(cleanup_failed_.load()))
        return Result<bool>::failure(Error::USB_IO);
    if (!active_receiver_.owns(receiver) || receiver != tuned_receiver_ ||
        system != tuned_system_ || !tuned_)
        return Result<bool>::failure(Error::UNSUPPORTED);
    const auto gate_deadline = steady_now() +
                               std::chrono::seconds(2);
    if (!acquire_control_gate(gate_deadline)) return Result<bool>::failure(Error::TIMEOUT);
    std::unique_lock<std::recursive_timed_mutex> gate(control_gate_, std::adopt_lock);
    if (uses_v2_frontend()) {
        bool locked = false;
        const auto result = read_v2_frontend_lock(
            this, {static_cast<std::uint8_t>(receiver ^ 1U)}, &locked);
        return result == V2FrontendResult::Completed
            ? Result<bool>::success(locked)
            : Result<bool>::failure(satellite_error(v2_satellite_result(result)));
    }
    if (system == System::ISDB_S) {
        const SatelliteLockResult result = read_model_satellite_lock();
        if (result.result != SatelliteOperationResult::Completed)
            return Result<bool>::failure(satellite_error(result.result));
        return Result<bool>::success(result.locked);
    }
    std::uint8_t lock = 0;
    bool have = false;
    const auto plan = uses_legacy_frontend()
        ? plan_legacy_terrestrial_lock(legacy_frontend(), tuned_frequency_khz_)
        : plan_terrestrial_lock_read(tuned_frequency_khz_);
    FrontendRunReport report{};
    if (!run_plan(plan, 1000U, &report) || !report.have_last_read)
        return Result<bool>::failure(disconnected_.load() ? Error::DISCONNECTED : Error::USB_IO);
    lock = report.last_read;
    have = true;
    const bool locked = have && (lock & 0x0fU) == 0x09U;
    if (locked && !gain_applied_ && !uses_legacy_frontend() && !uses_v2_frontend()) {
        if (!run_plan(plan_fc0012_gain_once(receiver_local(receiver)), 1000U))
            return Result<bool>::failure(Error::USB_IO);
        gain_applied_ = true;
    }
    return Result<bool>::success(locked);
}

Result<void> LibusbW3u3Hardware::select_satellite_slot(
    std::uint8_t receiver, std::uint8_t slot, std::uint32_t timeout_ms) noexcept {
    if (!frontend_io_allowed_after_cleanup(cleanup_failed_.load()))
        return Result<void>::failure(Error::USB_IO);
    if (receiver != 0U || !active_receiver_.owns(receiver) ||
        tuned_receiver_ != receiver || tuned_system_ != System::ISDB_S ||
        !tuned_ || slot >= kW3u3SatelliteTsidSlots || timeout_ms == 0U)
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    const auto gate_deadline = steady_now() +
                               std::chrono::milliseconds(timeout_ms);
    if (!acquire_control_gate(gate_deadline)) return Result<void>::failure(Error::TIMEOUT);
    std::unique_lock<std::recursive_timed_mutex> gate(control_gate_, std::adopt_lock);
    const auto previous_deadline = deadline_;
    const bool previous_deadline_active = deadline_active_;
    const auto selection_deadline = gate_deadline;
    deadline_ = previous_deadline_active
        ? std::min(previous_deadline, selection_deadline)
        : selection_deadline;
    deadline_active_ = true;
    const auto list = wait_model_satellite_tsid(slot, 0U, false);
    SatelliteOperationResult result = list.result;
    if (result == SatelliteOperationResult::Completed)
        result = select_model_satellite_tsid(slot, list.tsids).result;
    deadline_ = previous_deadline;
    deadline_active_ = previous_deadline_active;
    const Error error = satellite_error(result);
    return error == Error::OK ? Result<void>::success()
                              : Result<void>::failure(error);
}
Result<void> LibusbW3u3Hardware::select_satellite_tsid(
    std::uint8_t receiver, std::uint16_t tsid, std::uint32_t timeout_ms) noexcept {
    if (!frontend_io_allowed_after_cleanup(cleanup_failed_.load()))
        return Result<void>::failure(Error::USB_IO);
    if (receiver != 0U || !active_receiver_.owns(receiver) ||
        tuned_receiver_ != receiver || tuned_system_ != System::ISDB_S ||
        !tuned_ || timeout_ms == 0U || tsid == 0U || tsid == kW3u3SatelliteNoTsid)
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    const auto gate_deadline = steady_now() +
                               std::chrono::milliseconds(timeout_ms);
    if (!acquire_control_gate(gate_deadline)) return Result<void>::failure(Error::TIMEOUT);
    std::unique_lock<std::recursive_timed_mutex> gate(control_gate_, std::adopt_lock);
    const auto previous_deadline = deadline_;
    const bool previous_deadline_active = deadline_active_;
    const auto selection_deadline = gate_deadline;
    deadline_ = previous_deadline_active
        ? std::min(previous_deadline, selection_deadline)
        : selection_deadline;
    deadline_active_ = true;
    const auto list = wait_model_satellite_tsid(0U, tsid, true);
    SatelliteOperationResult result = list.result;
    if (result == SatelliteOperationResult::Completed) {
        result = select_model_satellite_tsid(list.slot, list.tsids).result;
    }
    deadline_ = previous_deadline;
    deadline_active_ = previous_deadline_active;
    const Error error = satellite_error(result);
    return error == Error::OK ? Result<void>::success()
                              : Result<void>::failure(error);
}
Result<void> LibusbW3u3Hardware::close_receiver(std::uint8_t receiver) noexcept {
    if (!frontend_io_allowed_after_cleanup(cleanup_failed_.load()))
        return Result<void>::failure(Error::USB_IO);
    if (!runtime_model_supported() || receiver >= receiver_count())
        return Result<void>::failure(Error::UNSUPPORTED);
    if (!active_receiver_.owns(receiver)) return Result<void>::failure(Error::NOT_FOUND);
    if (source_prepared_) return Result<void>::failure(Error::BUSY);
    tuned_ = false;
    tuned_receiver_ = kNoActiveReceiver;
    gain_applied_ = false;
    (void)active_receiver_.release(receiver);
    return Result<void>::success();
}
Result<void> LibusbW3u3Hardware::begin_tune_power(std::uint8_t receiver, System system,
                                                 std::uint8_t lnb_voltage) noexcept {
    if (!frontend_io_allowed_after_cleanup(cleanup_failed_.load()))
        return Result<void>::failure(Error::USB_IO);
    if (!receiver_supports(receiver, system) ||
        !active_receiver_.owns(receiver) || lnb_voltage != 0U)
        return Result<void>::failure(Error::UNSUPPORTED);
    // Power-on is part of the source-verified shared open sequence. This
    // transaction hook does not duplicate board writes. Legacy TC_SetLNB is
    // a no-op: zero means no selectable LNB request, not measured zero volts.
    // Actual electrical effects of the source board startup are unverified.
    return initialized_ ? Result<void>::success()
                        : Result<void>::failure(Error::NOT_READY);
}
Result<void> LibusbW3u3Hardware::commit_tune_power(std::uint8_t receiver) noexcept {
    if (!frontend_io_allowed_after_cleanup(cleanup_failed_.load()))
        return Result<void>::failure(Error::USB_IO);
    return runtime_model_supported() && receiver < receiver_count() && active_receiver_.owns(receiver)
        ? Result<void>::success() : Result<void>::failure(Error::UNSUPPORTED);
}
Result<void> LibusbW3u3Hardware::rollback_tune_power(std::uint8_t receiver) noexcept {
    if (!frontend_io_allowed_after_cleanup(cleanup_failed_.load()))
        return Result<void>::failure(Error::USB_IO);
    return runtime_model_supported() && receiver < receiver_count() && active_receiver_.owns(receiver)
        ? Result<void>::success() : Result<void>::failure(Error::UNSUPPORTED);
}
void LibusbW3u3Hardware::mark_receiver_disconnected(std::uint8_t receiver) noexcept {
    if (receiver <= 1U) {
        disconnected_.store(true);
        interrupt();
    }
}
void LibusbW3u3Hardware::request_stop() noexcept { stop_requested_.store(true); interrupt(); }

std::chrono::steady_clock::time_point LibusbW3u3Hardware::steady_now() const noexcept {
    if (capture_usb_hooks_ != nullptr && capture_usb_hooks_->now != nullptr)
        return capture_usb_hooks_->now(capture_usb_hooks_->context);
    return std::chrono::steady_clock::now();
}

bool LibusbW3u3Hardware::acquire_control_gate(
    std::chrono::steady_clock::time_point deadline,
    const volatile std::sig_atomic_t* stop_flag, bool cleanup) noexcept {
    for (;;) {
        const auto now = steady_now();
        if (now >= deadline) return false;
        const auto slice = std::min(deadline, now + std::chrono::milliseconds(10));
        if (control_gate_.try_lock_until(slice)) {
            const bool stopped = stop_requested_.load(std::memory_order_acquire) ||
                (stop_flag != nullptr && *stop_flag != 0);
            if (!cleanup && (cleanup_failed_.load(std::memory_order_acquire) ||
                             disconnected_.load(std::memory_order_acquire) || stopped)) {
                control_gate_.unlock();
                return false;
            }
            return true;
        }
        if (!cleanup && (cleanup_failed_.load(std::memory_order_acquire) ||
                         stop_requested_.load() ||
                         (stop_flag != nullptr && *stop_flag != 0)))
            return false;
    }
}

void LibusbW3u3Hardware::mark_cleanup_failed(Error error) noexcept {
    int expected = 0;
    (void)cleanup_failure_error_.compare_exchange_strong(
        expected, static_cast<int>(error), std::memory_order_acq_rel);
    cleanup_failed_.store(true, std::memory_order_release);
    capture_interrupted_.store(true, std::memory_order_release);
    if (capture_usb_hooks_ != nullptr && capture_usb_hooks_->interrupt_events != nullptr)
        capture_usb_hooks_->interrupt_events(capture_usb_hooks_->context);
    else if (context_ != nullptr)
        libusb_interrupt_event_handler(context_);
}

bool LibusbW3u3Hardware::begin_card_operation(
    std::uint32_t timeout_ms,
    const volatile std::sig_atomic_t* stop_flag) noexcept {
    if (!runtime_model_supported() || !claimed_ || !initialized_ ||
        cleanup_failed_.load() || disconnected_.load() ||
        timeout_ms == 0U)
        return false;
    const auto deadline = steady_now() +
                          std::chrono::milliseconds(timeout_ms);
    if (!acquire_control_gate(deadline, stop_flag)) return false;
    if (cleanup_failed_.load() || disconnected_.load()) {
        control_gate_.unlock();
        return false;
    }
    diagnostic_stop_flag_ = stop_flag;
    deadline_ = deadline;
    deadline_active_ = true;
    return true;
}

void LibusbW3u3Hardware::end_card_operation() noexcept {
    deadline_active_ = false;
    diagnostic_stop_flag_ = nullptr;
    control_gate_.unlock();
}

bool LibusbW3u3Hardware::begin_card_cleanup(std::uint32_t timeout_ms) noexcept {
    if (!runtime_model_supported() || !claimed_ || disconnected_.load() || timeout_ms == 0U) {
        mark_cleanup_failed(Error::USB_IO);
        return false;
    }
    const auto deadline = steady_now() +
                          std::chrono::milliseconds(timeout_ms);
    if (!acquire_control_gate(deadline, nullptr, true)) {
        mark_cleanup_failed(Error::TIMEOUT);
        return false;
    }
    diagnostic_stop_flag_ = nullptr;
    deadline_ = deadline;
    deadline_active_ = true;
    card_cleanup_active_ = true;
    return true;
}

void LibusbW3u3Hardware::end_card_cleanup(bool cleanup_succeeded) noexcept {
    if (!cleanup_succeeded) mark_cleanup_failed(Error::USB_IO);
    card_cleanup_active_ = false;
    deadline_active_ = false;
    diagnostic_stop_flag_ = nullptr;
    control_gate_.unlock();
}

Result<void> LibusbW3u3Hardware::shutdown() noexcept {
    stop_requested_.store(true);
    const auto gate_deadline = steady_now() +
                               std::chrono::seconds(10);
    if (!acquire_control_gate(gate_deadline, nullptr, true)) {
        mark_cleanup_failed(Error::TIMEOUT);
        return Result<void>::failure(Error::TIMEOUT);
    }
    std::unique_lock<std::recursive_timed_mutex> gate(control_gate_, std::adopt_lock);
    AtomicFlagScope cleanup_scope(cleanup_io_active_);
    const bool needs_stop = source_prepared_ || cf_snapshot_valid_ ||
                            link_snapshot_valid_ || (async_ && async_->prepared);
    if (!attempt_hardware_shutdown_cleanup(*this, needs_stop, gpio_snapshot_valid_))
        mark_cleanup_failed(Error::USB_IO);
    if (!cleanup_failed_.load(std::memory_order_acquire))
        return Result<void>::success();
    const int stored = cleanup_failure_error_.load(std::memory_order_acquire);
    return Result<void>::failure(stored == 0 ? Error::USB_IO
                                             : static_cast<Error>(stored));
}

bool LibusbW3u3Hardware::stop_capture_safely() noexcept {
    // stop() exits the process if callbacks cannot be drained within the hard
    // deadline. An ordinary cleanup error returns only after storage is safe.
    return stop().has_value();
}

bool LibusbW3u3Hardware::snapshot_gpio_state() noexcept {
    if (!snapshot_gpio_if_needed(&gpio_snapshot_valid_, &gpio_snapshot_,
            [this](std::uint8_t* value) {
                const auto transfer = make_gpio_set(0U, 0U, 1000U);
                std::array<unsigned char, 1> response{};
                if (control(transfer, response.data()) != transfer.length) return false;
                *value = response[0];
                return true;
            })) return false;
    return true;
}

bool LibusbW3u3Hardware::restore_gpio_snapshot_safely() noexcept {
    if (disconnected_.load()) return true;  // USB writes are forbidden after loss.
    if (uses_legacy_frontend() || uses_v2_frontend()) {
        // GPIOExGet samples physical pins, not the previous output latch.
        // A sampled-low released line must never be restored as driven-low.
        // Use the documented board-off sequence instead of claiming rollback
        // to unobservable GPIOEx latch state. V2 also has a source-defined
        // idle path which does not restore unproven GPIO20 electrical state.
        if (!board_power_attempted_) {
            gpio_snapshot_valid_ = false;
            return true;
        }
        AtomicFlagScope cleanup_scope(cleanup_io_active_);
        const auto previous_deadline = deadline_;
        const bool previous_active = deadline_active_;
        deadline_ = steady_now() + std::chrono::seconds(3);
        deadline_active_ = true;
        bool off = true;
        const auto plan = uses_v2_frontend() ? plan_v2_shared_power_off()
            : plan_legacy_frontend_power(legacy_frontend(), false);
        // Best effort across independent board lines: a failed GPIO/Ex write
        // must not suppress the remaining source-defined shutdown steps.
        for (const auto& operation : plan) {
            if (run_frontend_plan(FrontendPlan{operation}, this) !=
                FrontendRunResult::Completed) off = false;
        }
        if (expired()) off = false;
        deadline_ = previous_deadline;
        deadline_active_ = previous_active;
        if (off) {
            board_power_attempted_ = false;
            gpio_snapshot_valid_ = false;
            initialized_ = false;
            v2_identity_verified_ = false;
        }
        return off;
    }
    const std::uint8_t mask = 0xdfU;
    const auto restore = make_gpio_set(gpio_snapshot_, mask, 1000U);
    std::array<unsigned char, 1> response{};
    bool restored = control(restore, response.data()) == restore.length;
    const auto readback = make_gpio_set(0U, 0U, 1000U);
    restored = control(readback, response.data()) == readback.length &&
               (response[0] & mask) == (gpio_snapshot_ & mask) && restored;
    if (restored) {
        gpio_snapshot_valid_ = false;
        board_power_attempted_ = false;
    }
    return restored;
}

bool LibusbW3u3Hardware::run_plan(const FrontendPlan& plan, unsigned timeout_ms,
                                 FrontendRunReport* report) noexcept {
    if (!claimed_ || disconnected_.load() || plan.empty()) return false;
    const auto operation_deadline = steady_now() +
                                    std::chrono::milliseconds(timeout_ms);
    if (!acquire_control_gate(operation_deadline)) return false;
    std::unique_lock<std::recursive_timed_mutex> gate(control_gate_, std::adopt_lock);
    const auto previous_deadline = deadline_;
    const bool previous_active = deadline_active_;
    deadline_ = previous_active ? std::min(previous_deadline, operation_deadline)
                                : operation_deadline;
    deadline_active_ = true;
    const auto result = run_frontend_plan(plan, this, report);
    deadline_ = previous_deadline;
    deadline_active_ = previous_active;
    return result == FrontendRunResult::Completed;
}

int LibusbW3u3Hardware::control(const ControlTransfer& original, unsigned char* data) {
    return control_function(0U, original, data);
}

int LibusbW3u3Hardware::control_function(std::uint8_t function,
                                         const ControlTransfer& original,
                                         unsigned char* data) {
    // Defense in depth: no unsupported profile can bypass the public entry
    // guards through card, diagnostic, teardown or future plan dispatch.
    if (!runtime_model_supported()) return LIBUSB_ERROR_NOT_SUPPORTED;
    // Secondary access is limited to V2's source-backed read-only pair checks.
    // Every frontend/card/link/capture operation still targets primary.
    if (function > 1U || (function == 1U &&
        (!uses_v2_frontend() || original.direction != Direction::In ||
         !(original.request == Request::CustomerInfo ||
           (original.request == Request::SysCtrlRead && original.value == 2U &&
            original.index == 0U && original.length == 3U) ||
           (original.request == Request::I2cRead && original.value == 0xb0a8U &&
            original.index == 0U && (original.length == 2U || original.length == 17U))))))
        return LIBUSB_ERROR_NOT_SUPPORTED;
    if (uses_v2_frontend() && !v2_roles_verified_ &&
        original.request != Request::SysCtrlRead && original.request != Request::CustomerInfo)
        return LIBUSB_ERROR_ACCESS;
    const unsigned requested_timeout = original.timeout_ms == 0U
        ? 1000U : original.timeout_ms;
    const auto gate_deadline = steady_now() +
        std::chrono::milliseconds(requested_timeout);
    if (!acquire_control_gate(gate_deadline, nullptr, true))
        return LIBUSB_ERROR_TIMEOUT;
    std::unique_lock<std::recursive_timed_mutex> gate(control_gate_, std::adopt_lock);
    if (cleanup_failed_.load(std::memory_order_acquire) &&
        !card_cleanup_active_ && !cleanup_io_active_.load(std::memory_order_acquire))
        return LIBUSB_ERROR_ACCESS;
    if (!claimed_ || disconnected_.load()) return LIBUSB_ERROR_NO_DEVICE;
    ControlTransfer transfer = original;
    if (!uses_legacy_frontend() && !uses_v2_frontend()) {
        // Retain the historical W3U2/W3U3 no-selectable-LNB policy. The
        // source-specific S3 boards require their own GPIO/GPIOEx sequence.
        bool skip = false;
        ControlTransfer safe{};
        if (!mask_lnb_gpio_operation(transfer, &safe, &skip)) return LIBUSB_ERROR_ACCESS;
        if (skip) {
            if (data != nullptr && transfer.length > 0) data[0] = 1U;
            return transfer.length;
        }
        transfer = safe;
    }
    if (deadline_active_) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline_ - steady_now()).count();
        if (left <= 0) return LIBUSB_ERROR_TIMEOUT;
        const unsigned requested = transfer.timeout_ms == 0U ? 1000U : transfer.timeout_ms;
        transfer.timeout_ms = static_cast<std::uint16_t>(
            std::min<long long>(left, requested));
    }
    int rc = LIBUSB_ERROR_NOT_SUPPORTED;
    if (capture_usb_hooks_ != nullptr && capture_usb_hooks_->control_function != nullptr)
        rc = capture_usb_hooks_->control_function(capture_usb_hooks_->context,
                                                  function, transfer, data);
    else if (function == 0U && capture_usb_hooks_ != nullptr &&
             capture_usb_hooks_->control != nullptr)
        rc = capture_usb_hooks_->control(capture_usb_hooks_->context, transfer, data);
    else
        rc = function == 0U ? primary_.control(transfer, data)
                            : sibling_.control(transfer, data);
    if (rc == LIBUSB_ERROR_NO_DEVICE) disconnected_.store(true);
    return rc;
}
void LibusbW3u3Hardware::delay_ms(unsigned ms) {
    if (cancelled()) return;
    unsigned sleep = ms;
    if (deadline_active_) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline_ - steady_now()).count();
        if (left <= 0) return;
        sleep = std::min<unsigned>(sleep, static_cast<unsigned>(left));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep));
}
bool LibusbW3u3Hardware::cancelled() const {
    if (cleanup_io_active_.load(std::memory_order_acquire)) return false;
    return (!card_cleanup_active_ && stop_requested_.load()) ||
           (diagnostic_stop_flag_ != nullptr && *diagnostic_stop_flag_ != 0);
}
bool LibusbW3u3Hardware::expired() const {
    return deadline_active_ && steady_now() >= deadline_;
}

Result<void> LibusbW3u3Hardware::prepare(
    std::uint8_t receiver, System system, const std::atomic<bool>& cancelled_flag) noexcept {
    if (!frontend_io_allowed_after_cleanup(cleanup_failed_.load()))
        return Result<void>::failure(Error::USB_IO);
    const auto gate_deadline = steady_now() +
                               std::chrono::seconds(15);
    if (!acquire_control_gate(gate_deadline, nullptr))
        return Result<void>::failure(Error::TIMEOUT);
    std::unique_lock<std::recursive_timed_mutex> gate(control_gate_, std::adopt_lock);
    if (!receiver_supports(receiver, system) || !claimed_ ||
        !active_receiver_.owns(receiver) ||
        tuned_receiver_ != receiver || tuned_system_ != system || !tuned_ ||
        source_prepared_ || stop_requested_.load())
        return Result<void>::failure(Error::UNSUPPORTED);
    const std::uint8_t local = receiver_local(receiver);
    const std::uint8_t endpoint = bulk_endpoint_for_local(local);
    if (endpoint == 0U) return Result<void>::failure(Error::UNSUPPORTED);
    source_receiver_ = receiver;
    dsc_attempted_ = false;
    dsc_stopped_ = true;
    capture_interrupted_.store(false);
    link_diagnostic_ = LinkSeedDiagnostic{};
    link_apply_attempted_ = false;
    output_start_attempted_ = false;
    const auto fail_prepare = [this](Error error) {
        const CaptureRunResult cleanup = stop_and_drain(dsc_attempted_);
        if (cleanup == CaptureRunResult::fatal_drain) fatal_drain_exit();
        if (cleanup != CaptureRunResult::cancelled) mark_cleanup_failed(Error::USB_IO);
        return Result<void>::failure(error);
    };
    if (cancelled_flag.load()) return fail_prepare(Error::NOT_READY);
    if (uses_legacy_frontend() && system == System::ISDB_T && !gain_applied_) {
        // These tune wrappers establish the vendor's default cached state.
        // Apply the model-specific one-shot once per tune, without importing
        // W3U3's lock-settle/retry policy or starting a polling thread.
        if (!run_plan(plan_legacy_default_gain(legacy_frontend(), true), 1000U))
            return fail_prepare(Error::USB_IO);
        gain_applied_ = true;
    }
    if (!snapshot_link_diagnostic()) return fail_prepare(Error::NOT_READY);
    link_snapshot_valid_ = true;
    cf_snapshot_valid_ = read_cf_block(local, cf_snapshot_.data(), cf_snapshot_.size());
    if (!cf_snapshot_valid_) return fail_prepare(Error::USB_IO);
    if (!fill_secure_entropy(link_seed_.data(), link_seed_.size()))
        return fail_prepare(Error::INTERNAL);
    if (!run_plan(plan_stream_setup(local, 1U), 5000U))
        return fail_prepare(Error::USB_IO);
    if (!set_cf_bit(local, 0x03U, true)) return fail_prepare(Error::USB_IO);
    if (!async_) async_ = std::make_unique<AsyncState>();
    AsyncState& state = *async_;
    bool already_prepared = false;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        already_prepared = state.prepared;
        state.read = state.write = state.count = 0U;
        state.drain_failed = false;
        for (auto& slot : state.slots) {
            slot.owner = &state;
            slot.status = LIBUSB_TRANSFER_ERROR;
            slot.actual = 0;
            slot.pending = false;
            slot.ready = false;
            slot.generation = 0U;
        }
    }
    if (already_prepared) return fail_prepare(Error::BUSY);
    for (auto& slot : state.slots) {
        slot.transfer = allocate_transfer();
        if (slot.transfer == nullptr) return fail_prepare(Error::INTERNAL);
        libusb_fill_bulk_transfer(slot.transfer, primary_.handle(), endpoint,
                                  slot.buffer.data(), static_cast<int>(slot.buffer.size()),
                                  AsyncState::completed, &slot, 0U);
    }
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.prepared = true;
    }
    for (auto& slot : state.slots) {
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            slot.pending = true;
            ++slot.generation;
        }
        if (submit_transfer(slot.transfer) != 0) {
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                slot.pending = false;
            }
            return fail_prepare(Error::USB_IO);
        }
    }
    dsc_attempted_ = true;
    dsc_stopped_ = false;
    if (!dsc_start(local)) return fail_prepare(Error::USB_IO);
    output_start_attempted_ = true;
    if (!set_cf_bit(local, 0x08U, true)) return fail_prepare(Error::USB_IO);
    link_apply_attempted_ = true;
    if (!apply_link_seed()) return fail_prepare(Error::USB_IO);
    decoder_.clear();
    source_prepared_ = true;
    decoder_ = TransportCaptureDecoderV7(link_seed_.data(), link_seed_.size());
    link_seed_.fill(0);
    return Result<void>::success();
}

CaptureRunResult LibusbW3u3Hardware::run(
    const std::atomic<bool>& cancelled_flag,
    bool (*emit)(void*, const std::uint8_t*, std::size_t), void* context) noexcept {
    if (!source_prepared_ || emit == nullptr)
        return CaptureRunResult::usb_error;
    AsyncState& state = *async_;
    CaptureRunResult outcome = CaptureRunResult::cancelled;
    while (!cleanup_failed_.load(std::memory_order_acquire) &&
           !cancelled_flag.load() && !stop_requested_.load() &&
           !capture_interrupted_.load()) {
        bool drain_failed = false;
        std::size_t index = kQueueDepth;
        int slot_status = LIBUSB_TRANSFER_ERROR;
        int slot_actual = 0;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            drain_failed = state.drain_failed;
            if (!drain_failed && state.count != 0U) {
                index = state.ready[state.read];
                state.read = (state.read + 1U) % kQueueDepth;
                --state.count;
                AsyncState::Slot& queued = state.slots[index];
                queued.ready = false;
                slot_status = queued.status;
                slot_actual = queued.actual;
            }
        }
        if (drain_failed) { outcome = CaptureRunResult::usb_error; break; }
        if (index == kQueueDepth) {
            if (!handle_events(100U)) {
                outcome = disconnected_.load() ? CaptureRunResult::disconnected
                                               : CaptureRunResult::usb_error;
                break;
            }
            continue;
        }
        AsyncState::Slot& slot = state.slots[index];
        if (slot_status != LIBUSB_TRANSFER_COMPLETED &&
            slot_status != LIBUSB_TRANSFER_TIMED_OUT) {
            outcome = disconnected_.load() ? CaptureRunResult::disconnected
                                           : CaptureRunResult::usb_error;
            break;
        }
        if (slot_actual > 0) {
            auto packets = decoder_.push(slot.buffer.data(), static_cast<std::size_t>(slot_actual));
            if (cleanup_failed_.load(std::memory_order_acquire)) {
                outcome = CaptureRunResult::usb_error;
                break;
            }
            if (!packets.empty() && !emit(context, packets.data(), packets.size())) {
                outcome = CaptureRunResult::cancelled;
                break;
            }
        }
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            if (cleanup_failed_.load(std::memory_order_acquire)) {
                outcome = CaptureRunResult::usb_error;
                break;
            }
            slot.actual = 0;
            slot.ready = false;
            slot.pending = true;
            ++slot.generation;
        }
        if (submit_transfer(slot.transfer) != 0) {
            std::lock_guard<std::mutex> lock(state.mutex);
            slot.pending = false;
            outcome = CaptureRunResult::usb_error;
            break;
        }
    }
    if (cleanup_failed_.load(std::memory_order_acquire))
        outcome = CaptureRunResult::usb_error;
    const CaptureRunResult cleanup = stop_and_drain(dsc_attempted_);
    if (cleanup == CaptureRunResult::fatal_drain) return cleanup;
    if (cleanup != CaptureRunResult::cancelled) {
        mark_cleanup_failed(Error::USB_IO);
        return cleanup;
    }
    source_prepared_ = false;
    if (outcome == CaptureRunResult::usb_error || outcome == CaptureRunResult::disconnected)
        return outcome;
    return CaptureRunResult::cancelled;
}

void LibusbW3u3Hardware::interrupt() noexcept {
    capture_interrupted_.store(true);
    if (capture_usb_hooks_ != nullptr && capture_usb_hooks_->interrupt_events != nullptr)
        capture_usb_hooks_->interrupt_events(capture_usb_hooks_->context);
    else if (context_ != nullptr)
        libusb_interrupt_event_handler(context_);
}

Result<void> LibusbW3u3Hardware::stop() noexcept {
    if (!claimed_) return Result<void>::failure(Error::NOT_READY);
    if (source_prepared_ || (async_ && async_->prepared) ||
        cf_snapshot_valid_ || link_snapshot_valid_) {
        const auto result = stop_and_drain(dsc_attempted_);
        if (result == CaptureRunResult::fatal_drain) fatal_drain_exit();
        if (result != CaptureRunResult::cancelled) {
            mark_cleanup_failed(Error::USB_IO);
            return Result<void>::failure(Error::USB_IO);
        }
        source_prepared_ = false;
    }
    if (!cleanup_failed_.load()) source_receiver_ = kNoActiveReceiver;
    if (!cleanup_failed_.load()) return Result<void>::success();
    const int stored = cleanup_failure_error_.load(std::memory_order_acquire);
    return Result<void>::failure(stored == 0 ? Error::USB_IO
                                             : static_cast<Error>(stored));
}

CaptureRunResult LibusbW3u3Hardware::stop_and_drain(bool dsc_was_attempted) noexcept {
    // A card transaction owns the bridge until its bounded operation ends.
    // Hold the same gate across DSC stop, callback drain and restoration so
    // no controller operation can enter between cleanup stages. Event/callback
    // handling never takes this gate.
    const auto deadline = steady_now() +
                          std::chrono::seconds(25);
    if (!acquire_control_gate(deadline, nullptr, true)) fatal_drain_exit();
    std::unique_lock<std::recursive_timed_mutex> gate(control_gate_, std::adopt_lock);
    AtomicFlagScope cleanup_scope(cleanup_io_active_);
    DrainAdapter ops(*this);
    const CaptureRunResult result = drain_capture_callbacks(
        ops, dsc_was_attempted, dsc_stopped_, kDrainLimit);
    if (result != CaptureRunResult::cancelled) mark_cleanup_failed(Error::USB_IO);
    return result;
}

CaptureRunResult LibusbW3u3Hardware::cleanup_after_drain(
    bool dsc_ok, bool dsc_was_attempted) noexcept {
    const auto gate_deadline = steady_now() +
                               std::chrono::seconds(5);
    if (!acquire_control_gate(gate_deadline, nullptr, true)) {
        mark_cleanup_failed(Error::TIMEOUT);
        return CaptureRunResult::usb_error;
    }
    std::unique_lock<std::recursive_timed_mutex> gate(control_gate_, std::adopt_lock);
    AtomicFlagScope cleanup_scope(cleanup_io_active_);
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
        source_receiver_ = kNoActiveReceiver;
        dsc_attempted_ = false;
        dsc_stopped_ = true;
    } else {
        mark_cleanup_failed(Error::USB_IO);
    }
    return result;
}

bool LibusbW3u3Hardware::handle_events(unsigned timeout_ms) noexcept {
    if (capture_usb_hooks_ != nullptr && capture_usb_hooks_->pump_events != nullptr) {
        const int rc = capture_usb_hooks_->pump_events(capture_usb_hooks_->context,
                                                       timeout_ms);
        if (rc == LIBUSB_ERROR_NO_DEVICE) disconnected_.store(true);
        return rc >= 0;
    }
    if (context_ == nullptr) return false;
    timeval timeout{};
    timeout.tv_sec = static_cast<long>(timeout_ms / 1000U);
    timeout.tv_usec = static_cast<long>((timeout_ms % 1000U) * 1000U);
    const int rc = libusb_handle_events_timeout_completed(context_, &timeout, nullptr);
    if (rc == LIBUSB_ERROR_NO_DEVICE) disconnected_.store(true);
    return rc >= 0;
}

libusb_transfer* LibusbW3u3Hardware::allocate_transfer() noexcept {
    return capture_usb_hooks_ != nullptr && capture_usb_hooks_->allocate != nullptr
               ? capture_usb_hooks_->allocate(capture_usb_hooks_->context)
               : libusb_alloc_transfer(0);
}

int LibusbW3u3Hardware::submit_transfer(libusb_transfer* transfer) noexcept {
    return capture_usb_hooks_ != nullptr && capture_usb_hooks_->submit != nullptr
               ? capture_usb_hooks_->submit(capture_usb_hooks_->context, transfer)
               : libusb_submit_transfer(transfer);
}

int LibusbW3u3Hardware::cancel_transfer(libusb_transfer* transfer) noexcept {
    return capture_usb_hooks_ != nullptr && capture_usb_hooks_->cancel != nullptr
               ? capture_usb_hooks_->cancel(capture_usb_hooks_->context, transfer)
               : libusb_cancel_transfer(transfer);
}

void LibusbW3u3Hardware::free_transfer(libusb_transfer* transfer) noexcept {
    if (capture_usb_hooks_ != nullptr && capture_usb_hooks_->free != nullptr) {
        capture_usb_hooks_->free(capture_usb_hooks_->context, transfer);
    } else {
        libusb_free_transfer(transfer);
    }
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
    if (data == nullptr || local > 1U || size != 0x45U) return false;
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
    if (data == nullptr || local > 1U || size != 0x45U) return false;
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
    if (!receiver_supports(local, System::ISDB_T) || locked == nullptr ||
        steady_now() >= deadline)
        return false;
    if (uses_v2_frontend())
        return read_v2_frontend_lock(this, {0U}, locked, nullptr, 500U) ==
               V2FrontendResult::Completed;
    FrontendRunReport report{};
    const auto plan = uses_legacy_frontend()
        ? plan_legacy_terrestrial_lock(legacy_frontend(), tuned_frequency_khz_)
        : plan_terrestrial_lock_read(tuned_frequency_khz_);
    if (!run_plan(plan, 500U, &report) || !report.have_last_read)
        return false;
    *locked = (report.last_read & 0x0fU) == 0x09U;
    return true;
}
bool LibusbW3u3Hardware::set_cf_bit(
    std::uint8_t local, std::uint8_t mask, bool value) noexcept {
    std::uint8_t current = 0;
    if (!read_cf40(local, &current)) return false;
    const std::uint8_t next = value ? static_cast<std::uint8_t>(current | mask)
                                    : static_cast<std::uint8_t>(current & ~mask);
    return next == current || write_cf40(local, next);
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
