// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/libusb_hardware_backend.h"

#include "asicen/device_profile.h"
#include "asicen/enclosure.h"
#include "asicen/hardware_gpio_guard.h"
#include "asicen/powered_controller_type.h"
#include "asicen/transport_transform.h"
#include "asicen/write_protocol.h"
#include "dtv/userland/detail/diagnostic_log.h"
#if defined(_WIN32)
#include "windows/libusb_stream_policy.h"
#include "px4/windows_tuner_nonce.h"
#else
#include "px4/posix_tuner_nonce.h"
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <condition_variable>
#include <cstring>
#include <new>
#if !defined(_WIN32)
#include <sys/resource.h>
#include <sys/time.h>
#endif
#include <thread>


namespace asicen {
namespace {
using px4::userland::Error;
using px4::userland::Result;
using px4::userland::ipc::System;

constexpr std::size_t kQueueDepth = 4U;
constexpr std::size_t kChunkSize = 65536U;
constexpr std::chrono::milliseconds kDrainLimit{8000};
#if defined(DTV_USERLAND_DIAGNOSTICS)
thread_local bool prepare_runtime_diagnostic_active = false;
#endif
// TC_SetLNB for original W3U2/W3U3 is active-low GPIO20. The V2
// Windows LNBONOFF path is active-high on that same bit; its verified
// primary role owns the control. S3U/S3U2 vendor setters are no-ops until
// an independent, model-specific software control is established.
struct LnbGpioControl {
    bool supported = false;
    std::uint8_t mask = 0U;
    std::uint8_t on_value = 0U;
    std::uint8_t off_value = 0U;
};
LnbGpioControl lnb_gpio_control(const DeviceProfile* profile) noexcept
{
    if (profile == nullptr || !profile_runtime_supported(*profile)) {
        return {};
    }
    switch (profile->model_id) {
    case ModelId::W3u2:
    case ModelId::W3u3:
        return {true, 0x20U, 0U, 0x20U};
    case ModelId::W3u3V2:
        return {true, 0x20U, 0x20U, 0U};
    case ModelId::S3u:
    case ModelId::S3u2:
        return {};
    }
    return {};
}

class DeadlineScope {
public:
    DeadlineScope(std::chrono::steady_clock::time_point& deadline, bool& active,
                  std::chrono::steady_clock::time_point until) noexcept
        : deadline_(deadline), active_(active), previous_(deadline), previously_active_(active)
    {
        deadline_ = until;
        active_ = true;
    }
    ~DeadlineScope()
    {
        deadline_ = previous_;
        active_ = previously_active_;
    }

private:
    std::chrono::steady_clock::time_point& deadline_;
    bool& active_;
    std::chrono::steady_clock::time_point previous_;
    bool previously_active_;
};

class ReceiverRouteScope {
public:
    ReceiverRouteScope(std::atomic<std::uint8_t>& route, std::uint8_t receiver) noexcept
        : route_(route), previous_(route.exchange(receiver, std::memory_order_acq_rel))
    {
    }
    ~ReceiverRouteScope() { route_.store(previous_, std::memory_order_release); }

private:
    std::atomic<std::uint8_t>& route_;
    std::uint8_t previous_;
};

SatelliteOperationResult v2_satellite_result(V2FrontendResult result) noexcept
{
    switch (result) {
    case V2FrontendResult::Completed:
        return SatelliteOperationResult::Completed;
    case V2FrontendResult::InvalidArgument:
    case V2FrontendResult::UnsupportedChip:
        return SatelliteOperationResult::InvalidArgument;
    case V2FrontendResult::ShortTransfer:
        return SatelliteOperationResult::ShortTransfer;
    case V2FrontendResult::Cancelled:
        return SatelliteOperationResult::Cancelled;
    case V2FrontendResult::DeadlineExceeded:
    case V2FrontendResult::NotLocked:
        return SatelliteOperationResult::DeadlineExceeded;
    case V2FrontendResult::FailedTransfer:
        return SatelliteOperationResult::FailedTransfer;
    }
    return SatelliteOperationResult::FailedTransfer;
}

Error satellite_error(SatelliteOperationResult result, int transport_error) noexcept
{
    switch (result) {
    case SatelliteOperationResult::Completed:
        return Error::OK;
    case SatelliteOperationResult::InvalidArgument:
        return Error::INVALID_ARGUMENT;
    case SatelliteOperationResult::Cancelled:
        return Error::NOT_READY;
    case SatelliteOperationResult::DeadlineExceeded:
        return Error::TIMEOUT;
    case SatelliteOperationResult::FailedTransfer:
        return transport_error < 0   ? map_libusb_error(transport_error)
               : transport_error > 0 ? Error::PROTOCOL_ERROR
                                     : Error::USB_IO;
    case SatelliteOperationResult::ShortTransfer:
        return Error::PROTOCOL_ERROR;
    case SatelliteOperationResult::VerificationFailed:
        return Error::PROTOCOL_ERROR;
    }
    return Error::USB_IO;
}

std::uint8_t receiver_local(std::uint8_t receiver) noexcept
{
    return static_cast<std::uint8_t>(receiver % 2U);
}

int device_port_path(const LibusbDevice& device, std::vector<std::uint8_t>* path)
{
    std::uint8_t ports[8]{};
    const int count = device.port_numbers(ports, static_cast<int>(sizeof(ports)));
    if (count < 0) {
        return count;
    }
    if (count > 0) {
        path->assign(ports, ports + count);
        return 0;
    }
    std::uint8_t bus = 0U;
    const int connected = device.connection_info(&bus, ports, static_cast<int>(sizeof(ports)));
    if (connected <= 0 || bus != libusb_get_bus_number(device.device())) return 0;
    path->assign(ports, ports + connected);
    return 0;
}

class AtomicFlagScope {
public:
    explicit AtomicFlagScope(std::atomic<bool>& flag) noexcept
        : flag_(flag), previous_(flag.exchange(true, std::memory_order_acq_rel))
    {
    }
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
        std::uint64_t submission_order = 0U;
    };
    std::array<Slot, kQueueDepth> slots{};
    std::uint64_t next_submission_order = 0U;
    std::uint64_t next_delivery_order = 0U;
    bool prepared = false;
    bool drain_failed = false;
    // Unlike drain_failed, this capture-result latch survives transfer release.
    // A cancellation/sink stop must not hide a malformed concurrent callback.
    bool split_completion_failed = false;
    bool split_draining = false;
    CaptureRunResult split_callback_result = CaptureRunResult::cancelled;
    bool raw_io_enabled = false;
    std::uint8_t endpoint = 0U;
    mutable std::mutex mutex;

    static void LIBUSB_CALL completed(libusb_transfer* transfer)
    {
        auto* slot = static_cast<Slot*>(transfer->user_data);
        if (slot == nullptr || slot->owner == nullptr) {
            return;
        }
        AsyncState& state = *slot->owner;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            if (!slot->pending || slot->transfer != transfer) {
                state.drain_failed = true;
                state.split_completion_failed = true;
                return;
            }
            slot->pending = false;
            slot->status = static_cast<int>(transfer->status);
            slot->actual = transfer->actual_length;
            slot->ready = true;
            // Preserve errors reported before the drain boundary even if a
            // simultaneous sink rejection/cancel skips the producer scan.
            if (!state.split_draining && slot->status != LIBUSB_TRANSFER_COMPLETED &&
                slot->status != LIBUSB_TRANSFER_TIMED_OUT) {
                if (slot->status == LIBUSB_TRANSFER_NO_DEVICE)
                    state.split_callback_result = CaptureRunResult::disconnected;
                else if (state.split_callback_result == CaptureRunResult::cancelled)
                    state.split_callback_result = CaptureRunResult::usb_error;
            }
            if (slot->actual < 0 || static_cast<std::size_t>(slot->actual) > slot->buffer.size() ||
                slot->actual > transfer->length) {
                state.drain_failed = true;
                state.split_completion_failed = true;
                return;
            }
        }
    }
};

LibusbW3u3Hardware::AsyncState*
LibusbW3u3Hardware::capture_async(std::uint8_t receiver) noexcept
{
    return receiver < async_states_.size() ? async_states_[receiver] : nullptr;
}

const LibusbW3u3Hardware::AsyncState*
LibusbW3u3Hardware::capture_async(std::uint8_t receiver) const noexcept
{
    return receiver < async_states_.size() ? async_states_[receiver] : nullptr;
}

TransportCaptureDecoderV7*
LibusbW3u3Hardware::capture_decoder(std::uint8_t receiver) noexcept
{
    return receiver < decoders_.size() ? decoders_[receiver].get() : nullptr;
}

const TransportCaptureDecoderV7*
LibusbW3u3Hardware::capture_decoder(std::uint8_t receiver) const noexcept
{
    return receiver < decoders_.size() ? decoders_[receiver].get() : nullptr;
}

bool LibusbW3u3Hardware::any_source_prepared() const noexcept
{
    return std::any_of(source_prepared_.begin(), source_prepared_.end(),
                       [](bool prepared) { return prepared; });
}

// One run-owned heap allocation. A reserved or consumer-held block counts
// against the same bounded ingress limit as a queued block. USB slots are separate.
struct LibusbW3u3Hardware::SplitPump {
    static constexpr std::size_t kIngressDepth = 32U;
    struct Block {
        std::array<std::uint8_t, kChunkSize> bytes{};
        std::uint64_t submission = 0;
        std::size_t size = 0;
    };
    LibusbW3u3Hardware& hardware;
    std::uint8_t receiver;
    const std::atomic<bool>& cancelled;
    bool (*emit)(void*, const std::uint8_t*, std::size_t);
    void* context;
    std::array<Block, kIngressDepth> blocks{};
    std::array<bool, kIngressDepth> used{};
    std::array<std::size_t, kIngressDepth> fifo{};
    std::size_t head = 0, count = 0, occupied = 0, high_water = 0;
    std::mutex mutex;
    std::condition_variable changed;
    bool closed = false;
    std::atomic<bool> abort{false}, done{false};
    NativeThread consumer;
    CaptureRunResult consumer_result = CaptureRunResult::cancelled;
    const char* terminal = "cancelled";
    std::uint64_t copied_chunks = 0, copied_bytes = 0;
    std::uint64_t decoder_input_chunks = 0, decoder_input_bytes = 0;
    std::uint64_t discarded_chunks = 0, discarded_bytes = 0;
    std::uint64_t overflow_submission = 0;
    unsigned ingress_overflow = 0;

    SplitPump(LibusbW3u3Hardware& owner, std::uint8_t source_receiver,
              const std::atomic<bool>& stop,
              bool (*sink)(void*, const std::uint8_t*, std::size_t), void* opaque) noexcept
        : hardware(owner), receiver(source_receiver), cancelled(stop), emit(sink), context(opaque) {}

    bool stopping() const noexcept
    {
        return abort.load(std::memory_order_acquire) || cancelled.load() ||
            hardware.stop_requested_.load() ||
            (receiver < hardware.capture_interrupted_.size() &&
             hardware.capture_interrupted_[receiver].load()) ||
            hardware.cleanup_failed_.load(std::memory_order_acquire);
    }

    std::size_t reserve() noexcept
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (std::size_t i = 0; i < used.size(); ++i) {
            if (!used[i]) {
                used[i] = true;
                high_water = std::max(high_water, ++occupied);
                return i;
            }
        }
        return kIngressDepth; // Never wait for consumer capacity.
    }

    void release(std::size_t index) noexcept
    {
        used[index] = false;
        --occupied;
    }

    bool publish(std::size_t index) noexcept
    {
        std::lock_guard<std::mutex> lock(mutex);
        ++copied_chunks;
        copied_bytes += blocks[index].size;
        if (closed || done.load(std::memory_order_acquire) || stopping()) {
            ++discarded_chunks;
            discarded_bytes += blocks[index].size;
            release(index);
            return false;
        }
        fifo[(head + count) % fifo.size()] = index;
        ++count;
        changed.notify_one();
        return true;
    }

    void close(bool discard) noexcept
    {
        if (discard) abort.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(mutex);
            closed = true;
        }
        changed.notify_all();
    }

    static void* entry(void* opaque) noexcept
    {
        static_cast<SplitPump*>(opaque)->consume();
        return nullptr;
    }

    void consume() noexcept
    {
        for (;;) {
            std::size_t index = kIngressDepth;
            {
                std::unique_lock<std::mutex> lock(mutex);
                changed.wait_for(lock, std::chrono::milliseconds(100),
                    [&] { return count != 0 || closed || stopping(); });
                if (stopping()) break;
                if (count == 0) {
                    if (closed) break;
                    continue;
                }
                index = fifo[head];
                head = (head + 1) % fifo.size();
                --count;
            }
            auto& block = blocks[index];
            // No queue or state lock is held through decode/emit.
            auto* decoder = hardware.capture_decoder(receiver);
            if (decoder == nullptr) {
                consumer_result = CaptureRunResult::usb_error;
                break;
            }
            const auto before = decoder->sync_loss_events();
            ++decoder_input_chunks;
            decoder_input_bytes += block.size;
            const auto delivered = decoder->push({block.bytes.data(), block.size},
                [](void* opaque, px4::userland::ByteView packet) noexcept {
                    auto& self = *static_cast<SplitPump*>(opaque);
                    // A stop between packets must not drain a whole queued run.
                    if (self.stopping()) return Result<void>::failure(Error::SLOW_CONSUMER);
                    return self.emit(self.context, packet.data, packet.size)
                        ? Result<void>::success() : Result<void>::failure(Error::SLOW_CONSUMER);
                }, this);
            hardware.capture_sync_errors_[receiver].fetch_add(decoder->sync_loss_events() - before);
            if (block.submission < kQueueDepth) {
                const auto framing = decoder->counters();
                hardware.record_startup("framing_emitted",
                    static_cast<std::int64_t>(framing.emitted_packets),
                    static_cast<std::int64_t>(framing.discarded_sync_search_bytes));
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                release(index);
            }
            if (!delivered || hardware.cleanup_failed_.load(std::memory_order_acquire)) {
                consumer_result = hardware.cleanup_failed_.load(std::memory_order_acquire) ||
                    (!delivered && delivered.error() != Error::SLOW_CONSUMER)
                    ? CaptureRunResult::usb_error : CaptureRunResult::cancelled;
                break;
            }
        }
        bool wake_producer = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            while (count != 0) {
                const auto index = fifo[head];
                head = (head + 1) % fifo.size();
                --count;
                ++discarded_chunks;
                discarded_bytes += blocks[index].size;
                release(index);
            }
            done.store(true, std::memory_order_release);
            wake_producer = !closed;
        }
        // Reuse the existing race-safe event-wait terminal predicate and wake.
        // This never takes the queue/state/control gate and never resubmits.
        if (wake_producer) hardware.interrupt();
    }

    void dump() const noexcept
    {
        const bool reconciled = copied_bytes == decoder_input_bytes + discarded_bytes &&
            copied_chunks == decoder_input_chunks + discarded_chunks && occupied == 0;
        std::fprintf(stderr, "asicen-split-usb-pump version=1 terminal=%s ingress_overflow=%u overflow_submission=%llu "
            "pool_blocks=32 block_bytes=65536"
            " high_water=%zu copied_chunks=%llu copied_bytes=%llu decoder_input_chunks=%llu decoder_input_bytes=%llu discarded_chunks=%llu discarded_bytes=%llu accounting=%s\n",
            terminal, ingress_overflow, static_cast<unsigned long long>(overflow_submission), high_water,
            static_cast<unsigned long long>(copied_chunks), static_cast<unsigned long long>(copied_bytes),
            static_cast<unsigned long long>(decoder_input_chunks), static_cast<unsigned long long>(decoder_input_bytes),
            static_cast<unsigned long long>(discarded_chunks), static_cast<unsigned long long>(discarded_bytes),
            reconciled ? "reconciled" : "inconclusive");
    }
};

struct LibusbW3u3Hardware::DrainAdapter final : CaptureDrainOps, CaptureCleanupOps {
    DrainAdapter(LibusbW3u3Hardware& owner, std::uint8_t receiver)
        : owner_(owner), receiver_(receiver)
    {
    }
    bool stop_dsc() noexcept override
    {
        if (receiver_ >= owner_.receiver_count()) {
            return false;
        }
        const std::uint8_t local = receiver_local(receiver_);
        const bool ok = owner_.dsc_stop(local);
        owner_.receiver_capture_states_[receiver_].dsc_stopped = ok;
        return ok;
    }
    void cancel_pending() noexcept override
    {
        AsyncState* const async = owner_.capture_async(receiver_);
        if (async == nullptr) {
            return;
        }
        std::array<libusb_transfer*, kQueueDepth> transfers{};
        {
            std::lock_guard<std::mutex> lock(async->mutex);
            for (std::size_t i = 0; i < async->slots.size(); ++i) {
                const auto& slot = async->slots[i];
                if (slot.transfer != nullptr && slot.pending) {
                    transfers[i] = slot.transfer;
                }
            }
        }
        for (auto* transfer : transfers) {
            if (transfer != nullptr) {
                (void)owner_.cancel_transfer(transfer);
            }
        }
    }
    bool has_pending() const noexcept override
    {
        const AsyncState* const async = owner_.capture_async(receiver_);
        if (async == nullptr) {
            return false;
        }
        std::lock_guard<std::mutex> lock(async->mutex);
        for (const auto& slot : async->slots) {
            if (slot.pending) {
                return true;
            }
        }
        return false;
    }
    void pump_events(unsigned timeout_ms) noexcept override
    {
        (void)owner_.handle_events(timeout_ms);
    }
    void release_transfers() noexcept override
    {
        AsyncState* const async = owner_.capture_async(receiver_);
        if (async == nullptr) {
            return;
        }
        std::array<libusb_transfer*, kQueueDepth> transfers{};
        {
            std::lock_guard<std::mutex> lock(async->mutex);
            for (std::size_t i = 0; i < async->slots.size(); ++i) {
                auto& slot = async->slots[i];
                transfers[i] = slot.transfer;
                slot.transfer = nullptr;
                slot.pending = false;
            }
            async->prepared = false;
            async->next_submission_order = 0U;
            async->next_delivery_order = 0U;
            async->drain_failed = false;
        }
        for (auto* transfer : transfers) {
            if (transfer != nullptr) {
                owner_.free_transfer(transfer);
            }
        }
#if defined(_WIN32)
        if (async->raw_io_enabled) {
            const int result = finish_libusb_stream(
                owner_.capture_handle(receiver_), async->endpoint);
            async->raw_io_enabled = false;
            if (result != 0) {
                owner_.mark_cleanup_failed(map_libusb_error(result));
            }
        }
#endif
    }
    CaptureRunResult cleanup_after_drain(bool dsc_stopped, bool dsc_attempted) noexcept override
    {
        return owner_.cleanup_after_drain(receiver_, dsc_stopped, dsc_attempted);
    }
    bool disconnected() const noexcept override { return owner_.disconnected_.load(); }
    bool clear_seed_and_verify_output() noexcept override
    {
        return owner_.clear_link_seed_and_verify_controller();
    }
    bool disable_output_and_verify() noexcept override
    {
        if (auto* const coordinator = owner_.capture_coordinator(receiver_);
            coordinator != nullptr && coordinator->active(receiver_)) {
            coordinator->quarantine();
            return false;
        }
        return owner_.write_controller05(0U) && owner_.read_controller05_value_zero();
    }
    bool restore_cf_and_verify() noexcept override
    {
        const auto& source = owner_.receiver_capture_states_[receiver_];
        if (!source.cf_snapshot_valid) {
            return true;
        }
        if (receiver_ >= owner_.receiver_count()) {
            return false;
        }
        const std::uint8_t local = receiver_local(receiver_);
        const bool written =
            owner_.write_cf_block(local, source.cf_snapshot.data(), source.cf_snapshot.size());
        std::array<std::uint8_t, 0x45> verify{};
        return written && owner_.read_cf_block(local, verify.data(), verify.size()) &&
               verify == source.cf_snapshot;
    }

private:
    LibusbW3u3Hardware& owner_;
    std::uint8_t receiver_;
};

LibusbW3u3Hardware::LibusbW3u3Hardware(libusb_context* context, UsbLocation primary,
                                       UsbLocation sibling, std::vector<std::uint8_t> primary_path,
                                       std::vector<std::uint8_t> sibling_path,
                                       const DeviceProfile* expected_profile,
                                       bool sibling_runtime_required)
    : context_(context), primary_claim_(primary_, "primary"),
      sibling_claim_(sibling_, "sibling"),
      primary_location_(primary), sibling_location_(sibling),
      primary_path_(std::move(primary_path)), sibling_path_(std::move(sibling_path)),
      sibling_runtime_required_(sibling_runtime_required)
{
    if (expected_profile != nullptr) {
        const auto* known = find_profile(expected_profile->vid, expected_profile->pid);
        if (known != nullptr && known->model_id == expected_profile->model_id &&
            known->frontend_family == expected_profile->frontend_family &&
            known->source_supported == expected_profile->source_supported) {
            profile_ = known;
        }
    }
    if (context_ == nullptr) {
        return;
    }
    open_error_ = primary_.open(context_, primary);
    if (open_error_ == 0) {
        if (expected_profile == nullptr) {
            libusb_device_descriptor descriptor{};
            open_error_ = primary_.descriptor(&descriptor);
            if (open_error_ == 0) {
                profile_ = find_profile(descriptor.idVendor, descriptor.idProduct);
            }
        }
        if (profile_ != nullptr && profile_->expected_runtime_functions == 2U) {
            open_error_ = sibling_.open(context_, sibling);
        }
        // Handles remain open until release(), including after stream cleanup.
    }
}

LibusbW3u3Hardware::LibusbW3u3Hardware(libusb_context* context, int primary_fd, int sibling_fd,
                                       const DeviceProfile* expected_profile,
                                       LibusbAcquisitionApi& api,
                                       bool sibling_runtime_required)
    : context_(context), primary_(api), sibling_(api),
      primary_claim_(primary_, "primary"), sibling_claim_(sibling_, "sibling"),
      sibling_runtime_required_(sibling_runtime_required)
{
    if (expected_profile != nullptr) {
        const auto* known = find_profile(expected_profile->vid, expected_profile->pid);
        if (known != nullptr && known->model_id == expected_profile->model_id &&
            known->frontend_family == expected_profile->frontend_family &&
            known->source_supported == expected_profile->source_supported) {
            profile_ = known;
        }
    }
    if (context_ == nullptr) {
        return;
    }
    open_error_ = primary_.open(context_, primary_fd);
    if (open_error_ != 0) {
        return;
    }
    if (expected_profile == nullptr) {
        libusb_device_descriptor descriptor{};
        open_error_ = primary_.descriptor(&descriptor);
        if (open_error_ != 0) {
            return;
        }
        profile_ = find_profile(descriptor.idVendor, descriptor.idProduct);
    }
    // The wrapped handle's port path is the caller-selected topology; it is
    // passed to the existing path-based claim unchanged.
    open_error_ = device_port_path(primary_, &primary_path_);
    if (open_error_ != 0) {
        return;
    }
    if (profile_ == nullptr) {
        return;
    }
    const bool paired = profile_->expected_runtime_functions == 2U;
    if (paired != (sibling_fd >= 0)) {
        fd_function_mismatch_ = true;
        return;
    }
    if (paired) {
        open_error_ = sibling_.open(context_, sibling_fd);
        if (open_error_ == 0) {
            open_error_ = device_port_path(sibling_, &sibling_path_);
        }
    }
}

LibusbW3u3Hardware::~LibusbW3u3Hardware() noexcept
{
    // Final cleanup only uses the already-owned handles; a disconnected
    // bridge is terminal and is never reopened by release().
    stop_lnb_monitor();
    (void)release();
    flush_startup_trace();
    for (auto* async : async_states_) {
        delete async;
    }
}

Result<void> LibusbW3u3Hardware::claim()
{
    if (!startup_config_.valid) {
        std::fprintf(stderr, "ASICEN_DIAGNOSTIC_POST_GAIN_MS must be 0 or 340\n");
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    }
    // Reject unsupported or mismatched families before claims, GPIO reads,
    // revision/controller requests, or any other vendor transfer.
    if (fd_function_mismatch_) {
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    }
    if (open_error_ != 0) {
        return Result<void>::failure(map_libusb_acquisition_error(open_error_));
    }
    if (!runtime_model_supported()) {
        return Result<void>::failure(Error::UNSUPPORTED);
    }
    if (context_ == nullptr || !primary_.is_open() ||
        (profile_->expected_runtime_functions == 2U && !sibling_.is_open())) {
        return Result<void>::failure(Error::DISCONNECTED);
    }
    const auto result = ownership_.claim_profile(
        *profile_, primary_claim_,
        profile_->expected_runtime_functions == 2U ? &sibling_claim_ : nullptr, primary_path_,
        sibling_path_, sibling_runtime_required_ ? SiblingIdentityPolicy::runtime_required
                                                 : SiblingIdentityPolicy::reservation_allowed);
    primary_claim_.log_claim_diagnostic();
    if (profile_->expected_runtime_functions == 2U) {
        sibling_claim_.log_claim_diagnostic();
    }
    if (result != OwnershipError::none) {
        return Result<void>::failure(
            result == OwnershipError::primary_claim_failed ||
                    result == OwnershipError::sibling_claim_failed ||
                    result == OwnershipError::snapshot_failed
                ? map_libusb_acquisition_error(ownership_.last_transport_error())
            : result == OwnershipError::release_failed ? Error::USB_IO
                                                       : Error::INVALID_ARGUMENT);
    }
    claimed_ = true;
    first_transport_error_.store(0);
    if (!verify_device_revision()) {
        std::fprintf(stderr, "asicend: bridge revision unavailable or unsupported (requires 11/52; "
                             "16/52 is not implemented)\n");
        const Error error = transport_failure(Error::UNSUPPORTED);
        (void)release();
        return Result<void>::failure(error);
    }
    if (uses_v2_frontend() && !verify_v2_pair_roles()) {
        std::fprintf(stderr,
                     "asicend: V2 requires role0/role1 runtime functions, both revision 11/52\n");
        const Error error = transport_failure(Error::UNSUPPORTED);
        (void)release();
        return Result<void>::failure(error);
    }
    if (!snapshot_gpio_state(0U)) {
        const Error error = transport_failure(Error::PROTOCOL_ERROR);
        (void)release();
        return Result<void>::failure(error);
    }
    return Result<void>::success();
}

Result<void> LibusbW3u3Hardware::release() noexcept
{
    stop_lnb_monitor();
    if (abandoned_) {
        return Result<void>::failure(Error::USB_IO);
    }
    if (claimed_ && (lnb_cleanup_required_ || lnb_on_ || lnb_transaction_active_)) {
        const auto stopped = shutdown();
        if (!stopped && !disconnected_.load()) {
            return stopped;
        }
    }
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

SatelliteProbeSummary
LibusbW3u3Hardware::probe_satellite(std::uint32_t rf_khz, bool select_slot, std::size_t slot,
                                    const volatile std::sig_atomic_t* stop_flag,
                                    bool (*stop_callback)() noexcept) noexcept
{
    SatelliteProbeSummary summary{};
    if (!runtime_model_supported() || !claimed_ || cleanup_failed_.load() ||
        !is_w3u3_satellite_rf_khz(rf_khz) || (select_slot && slot >= kW3u3SatelliteTsidSlots)) {
        summary.result = SatelliteOperationResult::InvalidArgument;
        return summary;
    }
    if ((stop_flag != nullptr && *stop_flag != 0) ||
        (stop_callback != nullptr && stop_callback())) {
        summary.result = SatelliteOperationResult::Cancelled;
        return summary;
    }

    const auto absolute_deadline = steady_now() + std::chrono::seconds(60);
    diagnostic_stop_flag_ = stop_flag;
    diagnostic_stop_callback_ = stop_callback;
    deadline_ = absolute_deadline;
    deadline_active_ = true;
    const auto opened = open_receiver(0U);
    if (!opened) {
        if (cancelled()) {
            summary.result = SatelliteOperationResult::Cancelled;
        } else if (steady_now() >= absolute_deadline) {
            summary.result = SatelliteOperationResult::DeadlineExceeded;
        } else {
            summary.result = SatelliteOperationResult::FailedTransfer;
        }
        deadline_active_ = false;
        diagnostic_stop_flag_ = nullptr;
        diagnostic_stop_callback_ = nullptr;
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
        diagnostic_stop_callback_ = nullptr;
        return summary;
    }

    const auto lock_deadline = std::min(absolute_deadline, steady_now() + std::chrono::seconds(5));
    deadline_ = lock_deadline;
    const SatelliteLockResult lock = poll_model_satellite_lock();
    summary.result = lock.result;
    summary.locked = lock.locked;

    if (summary.result == SatelliteOperationResult::Completed && summary.locked) {
        deadline_ = absolute_deadline;
        const auto list =
            wait_model_satellite_tsid(select_slot ? slot : kW3u3SatelliteTsidSlots, 0U, false);
        summary.result = list.result;
        if (list.result == SatelliteOperationResult::Completed) {
            for (const std::uint16_t tsid : list.tsids) {
                if (tsid != 0U && tsid != kW3u3SatelliteNoTsid) {
                    ++summary.nonempty_tsid_slots;
                }
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
    diagnostic_stop_callback_ = nullptr;
    return summary;
}

CardProbeSummary LibusbW3u3Hardware::probe_card(const volatile std::sig_atomic_t* stop_flag,
                                                bool (*stop_callback)() noexcept) noexcept
{
    const std::array<std::uint8_t, 5U> apdu{{0x90U, 0x30U, 0x00U, 0x00U, 0x00U}};
    const CardProbeRequest request{CardProbeMode::apdu, {apdu.data(), apdu.size()}, 1U};
    auto summary = probe_card(request, stop_flag, stop_callback);
    if (summary.error == Error::OK &&
        (summary.response_length < 57U || summary.status_word != 0x9000U)) {
        summary.error = Error::PROTOCOL_ERROR;
    }
    return summary;
}

CardProbeSummary LibusbW3u3Hardware::probe_card(const CardProbeRequest& request,
                                                const volatile std::sig_atomic_t* stop_flag,
                                                bool (*stop_callback)() noexcept) noexcept
{
    CardProbeSummary summary{};
    if ((request.mode != CardProbeMode::detect && request.mode != CardProbeMode::atr &&
         request.mode != CardProbeMode::apdu) ||
        request.repeat == 0U || request.repeat > 100000U ||
        (request.mode != CardProbeMode::apdu &&
         (request.repeat != 1U || request.apdu.size != 0U)) ||
        (request.mode == CardProbeMode::apdu &&
         (request.apdu.data == nullptr || request.apdu.size == 0U || request.apdu.size > 65535U))) {
        summary.error = Error::INVALID_ARGUMENT;
        summary.failure_status = 2;
        return summary;
    }
    summary.error = Error::UNSUPPORTED;
    if (!runtime_model_supported() || !claimed_ || cleanup_failed_.load()) {
        summary.failure_status = 4;
        return summary;
    }
    if ((stop_flag != nullptr && *stop_flag != 0) ||
        (stop_callback != nullptr && stop_callback())) {
        summary.error = Error::TIMEOUT;
        summary.failure_status = 9;
        return summary;
    }

    const auto absolute_deadline = steady_now() + std::chrono::seconds(30);
    diagnostic_stop_flag_ = stop_flag;
    diagnostic_stop_callback_ = stop_callback;
    deadline_ = absolute_deadline;
    deadline_active_ = true;
    const auto opened = open_receiver(profile_->combined_isdb_ts ? 0U : 1U);
    if (!opened) {
        summary.error = opened.error();
        summary.failure_status = 6;
        deadline_active_ = false;
        diagnostic_stop_flag_ = nullptr;
        diagnostic_stop_callback_ = nullptr;
        return summary;
    }
    deadline_ = absolute_deadline;
    deadline_active_ = true;

    W3u3CardMailboxHardware mailbox(*this, profile_->model_id);
    px4::userland::CardSession session(mailbox, mailbox);
    if (request.mode == CardProbeMode::detect) {
        const auto detected = mailbox.detect_card();
        summary.error = detected ? Error::OK : detected.error();
        summary.detected = detected && detected.value();
    } else {
        const auto atr = px4::userland::reset_and_read_card_atr(mailbox, mailbox);
        if (!atr) {
            summary.error = atr.error();
        } else {
            summary.detected = true;
            summary.atr_valid = true;
            summary.atr = atr.value();
            summary.atr_length = atr.value().length;
            summary.error = Error::OK;
            if (request.mode == CardProbeMode::apdu) {
                const auto initialized = session.initialize_with_atr(atr.value());
                if (!initialized) {
                    summary.error = initialized.error();
                } else {
                    std::array<std::uint8_t, 4096U> response{};
                    for (std::uint32_t iteration = 0U; iteration < request.repeat; ++iteration) {
                        if (cancelled()) {
                            summary.error = Error::TIMEOUT;
                            summary.failure_status = 9;
                            break;
                        }
                        deadline_ = steady_now() + std::chrono::seconds(15);
                        const auto transmitted = session.transmit(
                            request.apdu,
                            px4::userland::MutableByteView{response.data(), response.size()});
                        if (!transmitted) {
                            summary.error = transmitted.error();
                            break;
                        }
                        summary.response_length = transmitted.value();
                        ++summary.transmit_count;
                    }
                    if (summary.error == Error::OK) {
                        summary.response.assign(response.begin(),
                                                response.begin() + summary.response_length);
                        if (summary.response_length >= 2U) {
                            summary.status_word = static_cast<std::uint16_t>(
                                (static_cast<std::uint16_t>(response[summary.response_length - 2U])
                                 << 8U) |
                                response[summary.response_length - 1U]);
                        }
                    }
                    volatile std::uint8_t* response_bytes = response.data();
                    for (std::size_t index = 0U; index < response.size(); ++index) {
                        response_bytes[index] = 0U;
                    }
                }
            }
        }
    }

    // Cleanup gets a fresh finite budget and ignores the signal cancellation,
    // while the first operation error remains the reported result.
    diagnostic_stop_flag_ = nullptr;
    diagnostic_stop_callback_ = nullptr;
    deadline_ = steady_now() + std::chrono::seconds(2);
    deadline_active_ = true;
    const auto card_cleanup = mailbox.shutdown_controller();
    if (!card_cleanup) {
        mark_cleanup_failed(Error::USB_IO);
        if (summary.error == Error::OK) {
            summary.error = card_cleanup.error();
            summary.failure_status = 8;
        }
    }
    deadline_active_ = false;
    diagnostic_stop_flag_ = nullptr;
    return summary;
}

bool LibusbW3u3Hardware::runtime_model_supported() const noexcept
{
    return profile_ != nullptr && profile_runtime_supported(*profile_) &&
           (profile_->frontend_family == FrontendFamily::W3u3 ||
            profile_->frontend_family == FrontendFamily::S3u ||
            profile_->frontend_family == FrontendFamily::S3u2 ||
            profile_->frontend_family == FrontendFamily::Nmi);
}

bool LibusbW3u3Hardware::receiver_initialized(std::uint8_t receiver) const noexcept
{
    if (profile_ == nullptr || receiver >= receiver_count()) {
        return false;
    }
    const auto route = receiver_address(*profile_, receiver);
    return route && route.value().rf_function_index < initialized_functions_.size() &&
           initialized_functions_[route.value().rf_function_index];
}

std::uint8_t LibusbW3u3Hardware::current_receiver() const noexcept
{
    const auto routed = cleanup_route_receiver_.load(std::memory_order_acquire);
    return routed != kNoActiveReceiver ? routed : active_receiver_.owner();
}

std::uint8_t LibusbW3u3Hardware::receiver_count() const noexcept
{
    if (!runtime_model_supported()) {
        return 0U;
    }
    if (profile_->model_id == ModelId::W3u2 || profile_->model_id == ModelId::W3u3) {
        return profile_->enclosure_receiver_count;
    }
    return profile_->combined_isdb_ts ? 1U : 2U;
}

Result<void> LibusbW3u3Hardware::prepare_runtime_functions() noexcept
{
#if defined(DTV_USERLAND_DIAGNOSTICS)
    struct DiagnosticScope final {
        bool& active;
        explicit DiagnosticScope(bool& value) noexcept : active(value)
        {
            active = true;
        }
        ~DiagnosticScope() { active = false; }
    } diagnostic_scope(prepare_runtime_diagnostic_active);
#endif
    if (!runtime_model_supported()) {
        return Result<void>::failure(Error::UNSUPPORTED);
    }
    std::array<bool, 2U> prepared{};
    const std::uint8_t initial_receiver = profile_->combined_isdb_ts ? 0U : 1U;
    for (std::uint8_t offset = 0U; offset < receiver_count(); ++offset) {
        const std::uint8_t receiver = offset == 0U
                                          ? initial_receiver
                                          : offset <= initial_receiver ? offset - 1U : offset;
        if (receiver >= receiver_count()) continue;
        const auto route = receiver_address(*profile_, receiver);
        if (!route || route.value().rf_function_index >= prepared.size()) {
#if defined(DTV_USERLAND_DIAGNOSTICS)
            dtv::userland::detail::diagnostic_log(
                "asicen_prepare_result stage=route receiver=%u error=%u",
                static_cast<unsigned>(receiver), static_cast<unsigned>(Error::UNSUPPORTED));
#endif
            return Result<void>::failure(Error::UNSUPPORTED);
        }
        const std::uint8_t function = route.value().rf_function_index;
        if (prepared[function]) continue;
        const auto opened = open_receiver(receiver);
#if defined(DTV_USERLAND_DIAGNOSTICS)
        dtv::userland::detail::diagnostic_log(
            "asicen_prepare_result stage=open receiver=%u function=%u error=%u",
            static_cast<unsigned>(receiver), static_cast<unsigned>(function),
            static_cast<unsigned>(opened ? Error::OK : opened.error()));
#endif
        if (!opened) return opened;
        const auto closed = close_receiver(receiver);
#if defined(DTV_USERLAND_DIAGNOSTICS)
        dtv::userland::detail::diagnostic_log(
            "asicen_prepare_result stage=close receiver=%u function=%u error=%u",
            static_cast<unsigned>(receiver), static_cast<unsigned>(function),
            static_cast<unsigned>(closed ? Error::OK : closed.error()));
#endif
        if (!closed) return closed;
        prepared[function] = true;
    }
    return Result<void>::success();
}

V2FrontendTarget LibusbW3u3Hardware::frontend_target(std::uint8_t receiver) const noexcept
{
    if (profile_ == nullptr) {
        return {};
    }
    const auto route = receiver_address(*profile_, receiver);
    return route ? V2FrontendTarget{route.value().frontend_source} : V2FrontendTarget{};
}
bool LibusbW3u3Hardware::receiver_supports(std::uint8_t receiver, System system) const noexcept
{
    if (!runtime_model_supported() || receiver >= receiver_count()) {
        return false;
    }
    if (profile_->combined_isdb_ts) {
        return system == System::ISDB_S || system == System::ISDB_T;
    }
    if (profile_->model_id != ModelId::W3u2 && profile_->model_id != ModelId::W3u3) {
        return (receiver == 0U && system == System::ISDB_S) ||
               (receiver == 1U && system == System::ISDB_T);
    }
    const auto route = receiver_address(*profile_, receiver);
    return route && system == (route.value().system == BroadcastSystem::IsdbS
                                   ? System::ISDB_S
                                   : System::ISDB_T);
}
bool LibusbW3u3Hardware::selects_satellite_stream_before_tune() const noexcept
{
    return false;
}
bool LibusbW3u3Hardware::requires_terrestrial_lock_settle() const noexcept
{
    return runtime_model_supported() && !uses_legacy_frontend() && !uses_v2_frontend();
}

SatelliteOperationResult LibusbW3u3Hardware::run_model_satellite_tune(std::uint32_t rf) noexcept
{
    if (uses_v2_frontend()) {
        if (!v2_identity_verified_) {
            return SatelliteOperationResult::InvalidArgument;
        }
        return v2_satellite_result(
            tune_v2_frontend(this, frontend_target(current_receiver()), rf));
    }
    return uses_legacy_frontend() ? run_legacy_satellite_tune(legacy_frontend(), this, rf)
                                  : run_w3u3_satellite_tune(this, rf);
}

SatelliteLockResult LibusbW3u3Hardware::read_model_satellite_lock() noexcept
{
    if (uses_v2_frontend()) {
        SatelliteLockResult result{};
        result.result = v2_satellite_result(
            read_v2_frontend_lock(this, frontend_target(current_receiver()), &result.locked));
        return result;
    }
    return uses_legacy_frontend() ? read_legacy_satellite_lock(legacy_frontend(), this)
                                  : read_w3u3_satellite_lock(this);
}

SatelliteLockResult LibusbW3u3Hardware::poll_model_satellite_lock() noexcept
{
    if (!uses_v2_frontend()) {
        return uses_legacy_frontend() ? poll_legacy_satellite_lock(legacy_frontend(), this)
                                      : poll_w3u3_satellite_lock(this);
    }
    SatelliteLockResult result{};
    for (unsigned attempt = 0; attempt < 50U; ++attempt) {
        if (cancelled()) {
            result.result = SatelliteOperationResult::Cancelled;
            break;
        }
        if (expired()) {
            result.result = SatelliteOperationResult::DeadlineExceeded;
            break;
        }
        result = read_model_satellite_lock();
        if (result.result != SatelliteOperationResult::Completed || result.locked) {
            break;
        }
        delay_ms(100U);
    }
    return result;
}

SatelliteTsidReadyResult LibusbW3u3Hardware::wait_model_satellite_tsid(std::size_t slot,
                                                                       std::uint16_t tsid,
                                                                       bool by_value) noexcept
{
    SatelliteTsidReadyResult result{};
    const auto remaining =
        deadline_active_
            ? std::max<long long>(
                  0, std::chrono::duration_cast<std::chrono::milliseconds>(deadline_ - steady_now())
                         .count())
            : 10000LL;
    const auto attempts = static_cast<std::size_t>(remaining / 10) + 1U;
    for (std::size_t attempt = 0U; attempt < attempts; ++attempt) {
        if (cancelled()) {
            result.result = SatelliteOperationResult::Cancelled;
            return result;
        }
        const bool final_poll = expired() || attempt + 1U == attempts;
        {
            // The reference permits one final readiness read at the exhausted
            // poll/sleep budget. Keep this one transaction independently finite;
            // a missing result cannot restart the readiness loop.
            DeadlineScope transaction(deadline_, deadline_active_,
                                      final_poll ? steady_now() + std::chrono::seconds(1)
                                                 : deadline_);
            if (uses_v2_frontend()) {
                result.result = v2_satellite_result(read_v2_frontend_tsids(
                    this, frontend_target(current_receiver()), &result.tsids));
            } else {
                const auto list = uses_legacy_frontend()
                                      ? read_legacy_satellite_tsids(legacy_frontend(), this)
                                      : read_w3u3_satellite_tsids(this);
                result.result = list.result;
                result.tsids = list.tsids;
            }
        }
        if (result.result != SatelliteOperationResult::Completed) {
            return result;
        }
        for (std::size_t i = 0; i < result.tsids.size(); ++i) {
            const auto value = result.tsids[i];
            if ((by_value || value != 0U) && value != kW3u3SatelliteNoTsid &&
                (by_value ? value == tsid : slot >= kW3u3SatelliteTsidSlots || i == slot)) {
                result.slot = i;
                return result;
            }
        }
        if (final_poll) {
            break;
        }
        delay_ms(10U);
    }
    result.result = SatelliteOperationResult::DeadlineExceeded;
    return result;
}

SatelliteTsidSelectResult LibusbW3u3Hardware::select_model_satellite_tsid(
    std::size_t slot, const std::array<std::uint16_t, kW3u3SatelliteTsidSlots>& tsids) noexcept
{
    if (!uses_v2_frontend()) {
        return uses_legacy_frontend()
                   ? select_legacy_satellite_tsid(legacy_frontend(), this, slot, tsids)
                   : select_w3u3_satellite_tsid(this, slot, tsids);
    }
    SatelliteTsidSelectResult result{};
    if (slot >= tsids.size() || tsids[slot] == kW3u3SatelliteNoTsid) {
        return result;
    }
    result.result = v2_satellite_result(
        select_v2_frontend_tsid(this, frontend_target(current_receiver()), tsids[slot]));
    if (result.result == SatelliteOperationResult::Completed) {
        result.selected_tsid = tsids[slot];
    }
    return result;
}

bool LibusbW3u3Hardware::uses_legacy_frontend() const noexcept
{
    return profile_ != nullptr && (profile_->frontend_family == FrontendFamily::S3u ||
                                   profile_->frontend_family == FrontendFamily::S3u2);
}

bool LibusbW3u3Hardware::uses_v2_frontend() const noexcept
{
    return profile_ != nullptr && profile_->frontend_family == FrontendFamily::Nmi;
}

bool LibusbW3u3Hardware::verify_v2_pair_roles() noexcept
{
    v2_roles_verified_ = false;
    const ControlTransfer info{
        0U, Request::CustomerInfo, 0U, 0U, kCustomerInfoSize, Direction::In, 1000U};
    for (std::uint8_t function = 0; function < 2U; ++function) {
        // Revision16 follows a different identity/link branch on either
        // function. Verify both before any shared-board startup write.
        const ControlTransfer revision{0U, Request::SysCtrlRead, 2U, 0U, 3U, Direction::In, 1000U};
        std::array<std::uint8_t, 3> revision_response{};
        if (control_function(function, revision, revision_response.data()) != revision.length ||
            revision_response[0] != 1U || revision_response[1] != 0x11U ||
            revision_response[2] != 0x52U) {
            return false;
        }
        std::array<std::uint8_t, kCustomerInfoSize> response{};
        if (control_function(function, info, response.data()) != info.length) {
            return false;
        }
        const auto route = v2_source_route(response.data(), response.size(), 0U);
        if (!route.valid || route.device_role != function || route.rf_master_role != 0U) {
            return false;
        }
    }
    v2_roles_verified_ = true;
    return true;
}

bool LibusbW3u3Hardware::verify_v2_pair_identity() noexcept
{
    v2_identity_verified_ = false;
    if (!v2_roles_verified_) {
        return false;
    }
    std::array<std::array<std::uint8_t, 17>, 2> identities{};
    bool read_ok = true;
    for (std::uint8_t function = 0; function < 2U; ++function) {
        // The source first probes one byte, then waits before reading the
        // opaque board identity. These are post-power, read-only operations.
        const auto probe = make_i2c_read(0xa8U, 0xb0U, 1U, 0U, 1000U);
        std::array<std::uint8_t, 2> response{};
        if (control_function(function, probe, response.data()) != probe.length ||
            response[0] != 1U) {
            read_ok = false;
            break;
        }
        delay_ms(10U);
        const auto read = make_i2c_read(0xa8U, 0xb0U, 16U, 0U, 1000U);
        if (control_function(function, read, identities[function].data()) != read.length ||
            identities[function][0] != 1U) {
            read_ok = false;
            break;
        }
    }
    bool same = read_ok;
    for (std::size_t i = 1U; i < 17U; ++i) {
        same = (identities[0][i] == identities[1][i]) && same;
    }
    // Never retain or log the private board identifier.
    for (auto& identity : identities) {
        volatile std::uint8_t* bytes = identity.data();
        for (std::size_t i = 0; i < identity.size(); ++i) {
            bytes[i] = 0U;
        }
    }
    v2_identity_verified_ = same;
    return same;
}

LegacyFrontendProfile LibusbW3u3Hardware::legacy_frontend() const noexcept
{
    return profile_->frontend_family == FrontendFamily::S3u ? LegacyFrontendProfile::S3u
                                                            : LegacyFrontendProfile::S3u2;
}

Result<void> LibusbW3u3Hardware::open_receiver(std::uint8_t receiver) noexcept
{
    if (!runtime_model_supported() || receiver >= receiver_count() ||
        receiver >= source_prepared_.size()) {
        return Result<void>::failure(Error::UNSUPPORTED);
    }
    if (cleanup_io_active_.load(std::memory_order_acquire)) {
        dtv::userland::detail::diagnostic_log(
            "asicen_open_busy receiver=%u reason=cleanup_io_active stage=entry",
            static_cast<unsigned>(receiver));
        return Result<void>::failure(Error::BUSY);
    }
    const ReceiverReservationResult reservation = active_receiver_.reserve(receiver);
    const bool newly_reserved = reservation == ReceiverReservationResult::reserved;
    if (reservation == ReceiverReservationResult::busy ||
        reservation == ReceiverReservationResult::invalid) {
        dtv::userland::detail::diagnostic_log(
            "asicen_open_busy receiver=%u reason=receiver_reservation result=%u",
            static_cast<unsigned>(receiver), static_cast<unsigned>(reservation));
        return Result<void>::failure(Error::BUSY);
    }
    const auto fail_open = [&](Error error) {
        if (newly_reserved) {
            (void)active_receiver_.release(receiver);
        }
        return Result<void>::failure(error);
    };
    const auto gate_deadline = steady_now() + std::chrono::seconds(20);
    if (!acquire_control_gate(gate_deadline)) {
        return fail_open(Error::TIMEOUT);
    }
    std::unique_lock<std::recursive_timed_mutex> gate(control_gate_, std::adopt_lock);
    ReceiverRouteScope route_scope(cleanup_route_receiver_, receiver);
    first_transport_error_.store(0);
    if (cleanup_io_active_.load(std::memory_order_acquire)) {
        dtv::userland::detail::diagnostic_log(
            "asicen_open_busy receiver=%u reason=cleanup_io_active stage=control_gate",
            static_cast<unsigned>(receiver));
        return fail_open(Error::BUSY);
    }
    // Reserve before reading mutable session/cleanup state so a competing
    // receiver cannot race a capture teardown and start frontend I/O.
    if (!claimed_) {
        return fail_open(Error::NOT_READY);
    }
    if (uses_v2_frontend() && !v2_roles_verified_) {
        return fail_open(Error::UNSUPPORTED);
    }
    if (!frontend_io_allowed_after_cleanup(cleanup_failed_.load())) {
        return fail_open(Error::USB_IO);
    }
    const auto route = receiver_address(*profile_, receiver);
    if (!route || !ownership_.function_supports_bulk_endpoint(
                      route.value().function_index,
                      bulk_endpoint_for_local(route.value().local_lane))) {
        return fail_open(Error::UNSUPPORTED);
    }
    for (std::uint8_t active = 0U; active < receiver_count(); ++active) {
        if (active == receiver || !active_receiver_.owns(active)) {
            continue;
        }
        const auto active_route = receiver_address(*profile_, active);
        if (active_route && active_route.value().rf_function_index ==
                                route.value().rf_function_index &&
            active_route.value().local_lane == route.value().local_lane) {
            dtv::userland::detail::diagnostic_log(
                "asicen_open_busy receiver=%u reason=rf_lane_owned active_receiver=%u function=%u lane=%u",
                static_cast<unsigned>(receiver), static_cast<unsigned>(active),
                static_cast<unsigned>(route.value().rf_function_index),
                static_cast<unsigned>(route.value().local_lane));
            return fail_open(Error::BUSY);
        }
    }
    if (route.value().rf_function_index >= initialized_functions_.size()) {
        return fail_open(Error::UNSUPPORTED);
    }
    if (initialized_functions_[route.value().rf_function_index]) {
        return Result<void>::success();
    }
    if (any_source_prepared()) {
        if (startup_config_.enabled) {
            unsigned prepared_mask = 0U;
            for (std::size_t index = 0U; index < source_prepared_.size(); ++index) {
                if (source_prepared_[index]) prepared_mask |= 1U << index;
            }
            std::fprintf(stderr,
                         "asicen-open-receiver-rejected receiver=%u function=%u "
                         "reason=uninitialized_function_while_capture_active "
                         "prepared_mask=%u\n",
                         static_cast<unsigned>(receiver),
                         static_cast<unsigned>(route.value().rf_function_index), prepared_mask);
        }
        dtv::userland::detail::diagnostic_log(
            "asicen_open_busy receiver=%u reason=uninitialized_function_while_capture_active function=%u",
            static_cast<unsigned>(receiver),
            static_cast<unsigned>(route.value().rf_function_index));
        return fail_open(Error::BUSY);
    }
    if (!verify_device_revision()) {
        std::fprintf(stderr, "asicend: bridge revision unavailable or unsupported (requires 11/52; "
                             "16/52 is not implemented)\n");
        return fail_open(transport_failure(Error::UNSUPPORTED));
    }
    if (!snapshot_gpio_state(receiver)) {
        return fail_open(transport_failure(Error::PROTOCOL_ERROR));
    }
    // Establish OFF before startup. In particular V2's source default GPIO
    // pattern contains its active-high LNB bit. Generic plans are masked
    // below so neither startup nor later snapshot restoration can enable it.
    if (supports_lnb_control()) {
        const auto off = set_lnb_power(false, false);
        if (!off) {
            (void)clear_lnb_power();
            return fail_open(off.error());
        }
    }
    FrontendPlan power_plan = uses_legacy_frontend()
                                  ? plan_legacy_frontend_startup(legacy_frontend())
                              : uses_v2_frontend() ? plan_v2_revision11_startup_prefix()
                                                   : plan_startup_subset();
    if (uses_legacy_frontend()) {
        const auto startup_off = plan_legacy_frontend_startup_off(legacy_frontend());
        power_plan.insert(power_plan.end(), startup_off.begin(), startup_off.end());
    }
    const auto power = uses_legacy_frontend() ? plan_legacy_frontend_power(legacy_frontend(), true)
                       : uses_v2_frontend()   ? plan_v2_shared_power_on()
                                              : plan_safe_power_on();
    if (!uses_v2_frontend() && !uses_legacy_frontend()) {
        power_plan.insert(power_plan.end(), power.begin(), power.end());
    }
    const FrontendPlan init_plan =
        uses_legacy_frontend() ? plan_legacy_frontend_init(legacy_frontend())
        : uses_v2_frontend()   ? FrontendPlan{}
                               : plan_terrestrial_init_with_satellite_demod();

    // One deadline covers startup, the now-powered controller guard, and
    // demod initialization. Never issue controller I2C before this power
    // sequence: the device NACKs those accesses while its controller is off.
    deadline_ = steady_now() + std::chrono::seconds(20);
    deadline_active_ = true;
    PoweredControllerCheck controller_check = PoweredControllerCheck::ready;
    const PoweredInitResult result = execute_powered_init_sequence(
        [&] {
            const auto route = receiver_address(*profile_, receiver);
            if (!route || route.value().rf_function_index >=
                              board_power_attempted_functions_.size()) {
                return false;
            }
            board_power_attempted_functions_[route.value().rf_function_index] = true;
            if (run_frontend_plan(power_plan, this) != FrontendRunResult::Completed) {
                return false;
            }
            if (uses_legacy_frontend()) {
                // DTV_Init rechecks the silicon after DTV_Start's off phase.
                return verify_device_revision() &&
                       run_frontend_plan(plan_legacy_frontend_init_prelude(legacy_frontend()),
                                         this) == FrontendRunResult::Completed &&
                       run_frontend_plan(power, this) == FrontendRunResult::Completed;
            }
            if (!uses_v2_frontend()) {
                return true;
            }
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
            if (!uses_v2_frontend()) {
                return run_frontend_plan(init_plan, this) == FrontendRunResult::Completed;
            }
            // Role0 is the shared RF master for both role0/role1 frontends.
            // Initialize T0,S0,T1,S1, while exposing only primary USB lanes.
            for (std::uint8_t source = 0U; source < 4U; ++source) {
                if (initialize_v2_frontend(this, {source}, nullptr, 4000U) !=
                    V2FrontendResult::Completed) {
                    return false;
                }
            }
            return true;
        });
    const Error init_error = transport_failure(cancelled() ? Error::NOT_READY
                                               : expired() ? Error::TIMEOUT
                                                           : Error::USB_IO);
    deadline_active_ = false;
    if (result != PoweredInitResult::completed) {
        if (result == PoweredInitResult::controller_guard_failed) {
            const char* reason = "controller guard failed";
            switch (controller_check) {
            case PoweredControllerCheck::type_read_failed:
                reason = "powered controller type read failed";
                break;
            case PoweredControllerCheck::unsupported_type:
                reason = "powered controller type unsupported";
                break;
            case PoweredControllerCheck::output_state_read_failed:
                reason = "controller output-idle read failed";
                break;
            case PoweredControllerCheck::output_busy:
                reason = "controller output is busy";
                break;
            case PoweredControllerCheck::ready:
                break;
            }
            std::fprintf(stderr, "asicend: %s\n", reason);
            if (controller_check == PoweredControllerCheck::output_busy) {
                dtv::userland::detail::diagnostic_log(
                    "asicen_open_busy receiver=%u reason=powered_controller_output_busy stage=initialize_function",
                    static_cast<unsigned>(receiver));
            }
        }
        const bool restored = restore_gpio_snapshot_for(receiver);
        if (!restored) {
            mark_cleanup_failed(Error::USB_IO);
        }
        Error error = init_error;
        if (result == PoweredInitResult::controller_guard_failed && restored) {
            if (controller_check == PoweredControllerCheck::unsupported_type) {
                error = Error::UNSUPPORTED;
            } else if (controller_check == PoweredControllerCheck::output_busy) {
                error = Error::BUSY;
            }
        }
        return fail_open(error);
    }
    initialized_ = true;
    initialized_functions_[route.value().rf_function_index] = true;
    return Result<void>::success();
}

Result<void> LibusbW3u3Hardware::tune_terrestrial(std::uint8_t receiver,
                                                  std::uint32_t frequency_khz,
                                                  std::uint32_t timeout_ms) noexcept
{
    if (!frontend_io_allowed_after_cleanup(cleanup_failed_.load())) {
        return Result<void>::failure(Error::USB_IO);
    }
    if (!claimed_ || !receiver_supports(receiver, System::ISDB_T) ||
        !active_receiver_.owns(receiver) || !receiver_initialized(receiver)) {
        return Result<void>::failure(Error::UNSUPPORTED);
    }
    if (timeout_ms == 0U) {
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    }
    if (uses_v2_frontend()) {
        if (!v2_identity_verified_ ||
            v2_tune_frequency_hz(frontend_target(receiver), frequency_khz) == 0U) {
            return Result<void>::failure(Error::INVALID_ARGUMENT);
        }
        const auto until = steady_now() + std::chrono::milliseconds(timeout_ms);
        if (!acquire_control_gate(until)) {
            return Result<void>::failure(Error::TIMEOUT);
        }
        std::unique_lock<std::recursive_timed_mutex> gate(control_gate_, std::adopt_lock);
        ReceiverRouteScope route_scope(cleanup_route_receiver_, receiver);
        first_transport_error_.store(0);
        const auto previous_deadline = deadline_;
        const bool previous_active = deadline_active_;
        deadline_ = previous_active ? std::min(previous_deadline, until) : until;
        deadline_active_ = true;
        const auto result =
            tune_v2_frontend(this, frontend_target(receiver), frequency_khz, nullptr, timeout_ms);
        deadline_ = previous_deadline;
        deadline_active_ = previous_active;
        if (result != V2FrontendResult::Completed) {
            return Result<void>::failure(
                satellite_error(v2_satellite_result(result), first_transport_error_.load()));
        }
        auto& state = receiver_tune_states_[receiver];
        state.tuned = true;
        state.system = System::ISDB_T;
        state.frequency_khz = frequency_khz;
        state.gain_applied = false;
        state.readiness.reset();
        return Result<void>::success();
    }
    reset_startup_diagnostic(receiver);
    startup_trace_active_.store(startup_config_.enabled);
    record_startup("tune_begin", frequency_khz, timeout_ms);
    const auto plan = uses_legacy_frontend()
                          ? plan_legacy_terrestrial_tune(legacy_frontend(), frequency_khz, 6U)
                          : plan_terrestrial_tune_full(frequency_khz, 6U);
    if (plan.empty()) {
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    }
    if (!run_plan(plan, timeout_ms, nullptr, receiver)) {
        return Result<void>::failure(plan_error_);
    }
    auto& state = receiver_tune_states_[receiver];
    state.tuned = true;
    state.system = System::ISDB_T;
    state.frequency_khz = frequency_khz;
    state.gain_applied = false;
    state.readiness.reset();
    record_startup("tune_complete");
    return Result<void>::success();
}

Result<void> LibusbW3u3Hardware::tune_satellite(std::uint8_t receiver, std::uint32_t frequency_khz,
                                                std::uint32_t timeout_ms) noexcept
{
    if (!frontend_io_allowed_after_cleanup(cleanup_failed_.load())) {
        return Result<void>::failure(Error::USB_IO);
    }
    std::uint32_t rf_khz = 0U;
    if (!claimed_ || !receiver_supports(receiver, System::ISDB_S) ||
        !active_receiver_.owns(receiver) || !receiver_initialized(receiver) || timeout_ms == 0U ||
        !w3u3_satellite_if_to_rf_khz(frequency_khz, &rf_khz)) {
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    }
    const auto gate_deadline = steady_now() + std::chrono::milliseconds(timeout_ms);
    if (!acquire_control_gate(gate_deadline)) {
        return Result<void>::failure(Error::TIMEOUT);
    }
    std::unique_lock<std::recursive_timed_mutex> gate(control_gate_, std::adopt_lock);
    ReceiverRouteScope route_scope(cleanup_route_receiver_, receiver);
    first_transport_error_.store(0);
    const auto previous_deadline = deadline_;
    const bool previous_deadline_active = deadline_active_;
    const auto selection_deadline = gate_deadline;
    deadline_ = previous_deadline_active ? std::min(previous_deadline, selection_deadline)
                                         : selection_deadline;
    deadline_active_ = true;
    reset_startup_diagnostic(receiver);
    startup_trace_active_.store(startup_config_.enabled);
    const SatelliteOperationResult tuned = run_model_satellite_tune(rf_khz);
    record_startup("satellite_tune_result", static_cast<int>(tuned),
                   first_transport_error_.load(), frequency_khz);
    deadline_ = previous_deadline;
    deadline_active_ = previous_deadline_active;
    const Error error = satellite_error(tuned, first_transport_error_.load());
    if (error != Error::OK) {
        return Result<void>::failure(error);
    }
    auto& state = receiver_tune_states_[receiver];
    state.tuned = true;
    state.system = System::ISDB_S;
    state.frequency_khz = frequency_khz;
    state.gain_applied = false;
    state.readiness.reset();
    return Result<void>::success();
}

Result<bool> LibusbW3u3Hardware::is_locked(std::uint8_t receiver, System system) noexcept
{
    if (!frontend_io_allowed_after_cleanup(cleanup_failed_.load())) {
        return Result<bool>::failure(Error::USB_IO);
    }
    if (!active_receiver_.owns(receiver) || !receiver_initialized(receiver) ||
        !receiver_tune_states_[receiver].tuned ||
        system != receiver_tune_states_[receiver].system) {
        return Result<bool>::failure(Error::UNSUPPORTED);
    }
    const auto gate_deadline = steady_now() + std::chrono::seconds(2);
    if (!acquire_control_gate(gate_deadline)) {
        return Result<bool>::failure(Error::TIMEOUT);
    }
    std::unique_lock<std::recursive_timed_mutex> gate(control_gate_, std::adopt_lock);
    ReceiverRouteScope route_scope(cleanup_route_receiver_, receiver);
    first_transport_error_.store(0);
    if (uses_v2_frontend()) {
        bool locked = false;
        const auto result = read_v2_frontend_lock(this, frontend_target(receiver), &locked);
        return result == V2FrontendResult::Completed
                   ? Result<bool>::success(locked)
                   : Result<bool>::failure(satellite_error(v2_satellite_result(result),
                                                           first_transport_error_.load()));
    }
    if (system == System::ISDB_S) {
        const SatelliteLockResult result = read_model_satellite_lock();
        if (result.result != SatelliteOperationResult::Completed) {
            return Result<bool>::failure(
                satellite_error(result.result, first_transport_error_.load()));
        }
        return Result<bool>::success(result.locked);
    }
    std::uint8_t lock = 0;
    bool have = false;
    const auto plan = uses_legacy_frontend()
                          ? plan_legacy_terrestrial_lock(
                                legacy_frontend(), receiver_tune_states_[receiver].frequency_khz)
                          : plan_terrestrial_lock_read(
                                receiver_tune_states_[receiver].frequency_khz);
    FrontendRunReport report{};
    if (!run_plan(plan, 1000U, &report, receiver)) {
        return Result<bool>::failure(plan_error_);
    }
    if (!report.have_last_read) {
        return Result<bool>::failure(Error::PROTOCOL_ERROR);
    }
    lock = report.last_read;
    have = true;
    const bool locked = have && (lock & 0x0fU) == 0x09U;
    const bool w3_terrestrial = !uses_legacy_frontend() && !uses_v2_frontend();
    auto& tune_state = receiver_tune_states_[receiver];
    record_startup(tune_state.gain_applied ? "post_gain_lock" : "pre_gain_lock", lock, locked);
    if (locked && !tune_state.gain_applied && w3_terrestrial) {
        record_startup("gain_begin");
        if (!run_plan(plan_fc0012_gain_once(receiver_local(receiver)), 1000U, nullptr, receiver)) {
            record_startup("gain_error", static_cast<int>(plan_error_));
            return Result<bool>::failure(plan_error_);
        }
        tune_state.gain_applied = true;
        tune_state.readiness.reset();
        record_startup("gain_complete");
        // The normal path and diagnostic B require a fresh post-gain sample.
        // Diagnostic A=0 alone retains the original immediate return.
        if (startup_config_.stable_ms != 0U)
            return Result<bool>::success(false);
    }
    if (w3_terrestrial && tune_state.gain_applied && startup_config_.stable_ms != 0U) {
        const bool ready = tune_state.readiness.observe(
            locked, steady_now(), startup_config_.stable_ms);
        record_startup("readiness", ready, startup_config_.stable_ms);
        return Result<bool>::success(ready);
    }
    return Result<bool>::success(locked);
}

Result<void> LibusbW3u3Hardware::select_satellite_slot(std::uint8_t receiver, std::uint8_t slot,
                                                       std::uint32_t timeout_ms) noexcept
{
    if (!frontend_io_allowed_after_cleanup(cleanup_failed_.load())) {
        return Result<void>::failure(Error::USB_IO);
    }
    if (!receiver_supports(receiver, System::ISDB_S) || !receiver_initialized(receiver) ||
        !active_receiver_.owns(receiver) ||
        !receiver_tune_states_[receiver].tuned ||
        receiver_tune_states_[receiver].system != System::ISDB_S ||
        slot >= kW3u3SatelliteTsidSlots ||
        timeout_ms == 0U) {
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    }
    const auto gate_deadline = steady_now() + std::chrono::milliseconds(timeout_ms);
    if (!acquire_control_gate(gate_deadline)) {
        return Result<void>::failure(Error::TIMEOUT);
    }
    std::unique_lock<std::recursive_timed_mutex> gate(control_gate_, std::adopt_lock);
    ReceiverRouteScope route_scope(cleanup_route_receiver_, receiver);
    first_transport_error_.store(0);
    const auto previous_deadline = deadline_;
    const bool previous_deadline_active = deadline_active_;
    const auto selection_deadline = gate_deadline;
    deadline_ = previous_deadline_active ? std::min(previous_deadline, selection_deadline)
                                         : selection_deadline;
    deadline_active_ = true;
    const auto list = wait_model_satellite_tsid(slot, 0U, false);
    SatelliteOperationResult result = list.result;
    if (result == SatelliteOperationResult::Completed) {
        // Readiness and the fixed select/readback transaction have separate
        // budgets in the reference. A successful final readiness read remains
        // selectable even when it consumed the last readiness millisecond.
        deadline_ = steady_now() + std::chrono::seconds(1);
        result = select_model_satellite_tsid(slot, list.tsids).result;
    }
    deadline_ = previous_deadline;
    deadline_active_ = previous_deadline_active;
    const Error error = satellite_error(result, first_transport_error_.load());
    return error == Error::OK ? Result<void>::success() : Result<void>::failure(error);
}
Result<void> LibusbW3u3Hardware::select_satellite_tsid(std::uint8_t receiver, std::uint16_t tsid,
                                                       std::uint32_t timeout_ms) noexcept
{
    if (!frontend_io_allowed_after_cleanup(cleanup_failed_.load())) {
        return Result<void>::failure(Error::USB_IO);
    }
    if (!receiver_supports(receiver, System::ISDB_S) || !receiver_initialized(receiver) ||
        !active_receiver_.owns(receiver) ||
        !receiver_tune_states_[receiver].tuned ||
        receiver_tune_states_[receiver].system != System::ISDB_S || timeout_ms == 0U ||
        tsid == kW3u3SatelliteNoTsid) {
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    }
    const auto gate_deadline = steady_now() + std::chrono::milliseconds(timeout_ms);
    if (!acquire_control_gate(gate_deadline)) {
        return Result<void>::failure(Error::TIMEOUT);
    }
    std::unique_lock<std::recursive_timed_mutex> gate(control_gate_, std::adopt_lock);
    ReceiverRouteScope route_scope(cleanup_route_receiver_, receiver);
    first_transport_error_.store(0);
    const auto previous_deadline = deadline_;
    const bool previous_deadline_active = deadline_active_;
    const auto selection_deadline = gate_deadline;
    deadline_ = previous_deadline_active ? std::min(previous_deadline, selection_deadline)
                                         : selection_deadline;
    deadline_active_ = true;
    const auto list = wait_model_satellite_tsid(0U, tsid, true);
    SatelliteOperationResult result = list.result;
    if (result == SatelliteOperationResult::Completed) {
        deadline_ = steady_now() + std::chrono::seconds(1);
        result = select_model_satellite_tsid(list.slot, list.tsids).result;
    }
    deadline_ = previous_deadline;
    deadline_active_ = previous_deadline_active;
    const Error error = satellite_error(result, first_transport_error_.load());
    return error == Error::OK ? Result<void>::success() : Result<void>::failure(error);
}
Result<void> LibusbW3u3Hardware::close_receiver(std::uint8_t receiver) noexcept
{
    if (!runtime_model_supported() || receiver >= receiver_count()) {
        return Result<void>::failure(Error::UNSUPPORTED);
    }
    const auto until = steady_now() + std::chrono::seconds(3);
    if (!acquire_control_gate(until, nullptr, true)) {
        mark_cleanup_failed(Error::TIMEOUT);
        return Result<void>::failure(Error::TIMEOUT);
    }
    std::unique_lock<std::recursive_timed_mutex> gate(control_gate_, std::adopt_lock);
    ReceiverRouteScope route_scope(cleanup_route_receiver_, receiver);
    if (!active_receiver_.owns(receiver)) {
        return Result<void>::failure(Error::NOT_FOUND);
    }
    if (source_prepared_[receiver] && !cleanup_failed_.load()) {
        return Result<void>::failure(Error::BUSY);
    }
    flush_startup_trace();
    receiver_tune_states_[receiver].readiness.reset();
    // Independent LNB cleanup is still attempted after another cleanup stage
    // quarantines the receiver. Never release its lease with uncertain power.
    // Only a verified, healthy independent (ctl) feed survives the close.
    const bool keep_independent = lnb_independent_on_ && lnb_state_known_ && lnb_on_ &&
                                  !lnb_fault_latched_ && !disconnected_.load() &&
                                  !cleanup_failed_.load();
    Result<void> off = Result<void>::success();
    if (keep_independent) {
        lnb_receiver_on_ = false;
        lnb_transaction_active_ = false;
        lnb_transaction_receiver_ = kNoActiveReceiver;
    } else {
        off = clear_lnb_power();
    }
    if (!off) {
        return off;
    }
    if (cleanup_failed_.load()) {
        return Result<void>::failure(Error::USB_IO);
    }
    receiver_tune_states_[receiver] = ReceiverTuneState{};
    (void)active_receiver_.release(receiver);
    return Result<void>::success();
}

bool LibusbW3u3Hardware::supports_lnb_control() const noexcept
{
    return lnb_gpio_control(profile_).supported;
}

Result<void> LibusbW3u3Hardware::set_lnb_power(bool on, bool cleanup) noexcept
{
    const auto spec = lnb_gpio_control(profile_);
    if (on && lnb_fault_latched_) {
        return Result<void>::failure(Error::PROTOCOL_ERROR);
    }
    if (!spec.supported) {
        return on ? Result<void>::failure(Error::UNSUPPORTED) : Result<void>::success();
    }
    auto until = steady_now() + std::chrono::seconds(2);
    if (!cleanup && deadline_active_) {
        until = std::min(until, deadline_);
    }
    if (!acquire_control_gate(until, nullptr, cleanup)) {
        return Result<void>::failure(disconnected_.load()                 ? Error::DISCONNECTED
                                     : stop_requested_.load() && !cleanup ? Error::NOT_READY
                                                                          : Error::TIMEOUT);
    }
    std::unique_lock<std::recursive_timed_mutex> gate(control_gate_, std::adopt_lock);
    first_transport_error_.store(0);
    if (!claimed_) {
        return Result<void>::failure(Error::NOT_READY);
    }
    if (disconnected_.load()) {
        return Result<void>::failure(Error::DISCONNECTED);
    }
    if (!cleanup && cleanup_failed_.load()) {
        return Result<void>::failure(Error::USB_IO);
    }
    if (uses_v2_frontend() && !v2_roles_verified_) {
        return Result<void>::failure(Error::UNSUPPORTED);
    }
    DeadlineScope deadline(deadline_, deadline_active_, until);
    // Cleanup may outlive the cancelled/expired tune and must get a fresh,
    // bounded budget. It is allowed even after capture/controller cleanup fails.
    const bool previous_cleanup = cleanup_io_active_.load();
    const bool previous_transition = lnb_power_transition_active_;
    if (cleanup) {
        cleanup_io_active_.store(true);
    }
    const auto finish = [&](Error error) {
        lnb_power_transition_active_ = previous_transition;
        if (cleanup) {
            cleanup_io_active_.store(previous_cleanup);
        }
        return error == Error::OK ? Result<void>::success() : Result<void>::failure(error);
    };
    if (on && !cleanup) {
        if (!initialized_) {
            return finish(Error::NOT_READY);
        }
        const auto ready = verify_powered_controller();
        if (ready != PoweredControllerCheck::ready) {
            return finish(
                disconnected_.load()                           ? Error::DISCONNECTED
                : ready == PoweredControllerCheck::output_busy ? Error::BUSY
                : ready == PoweredControllerCheck::unsupported_type
                    ? Error::UNSUPPORTED
                    : transport_failure(expired() ? Error::TIMEOUT : Error::PROTOCOL_ERROR));
        }
    }
    if (!cleanup && cancelled()) {
        return finish(Error::NOT_READY);
    }
    lnb_power_transition_active_ = true;
    const auto transfer = make_gpio_set(on ? spec.on_value : spec.off_value, spec.mask,
                                        on && uses_v2_frontend() ? 100U : 1000U);
    std::array<unsigned char, 1> response{};
    // A failed/short write can already have changed the physical pin.
    lnb_cleanup_required_ = true;
    lnb_state_known_ = false;
    publish_lnb_state();
    lnb_gpio_io_active_ = true;
    const int written = control(transfer, response.data());
    lnb_gpio_io_active_ = false;
    if (written != transfer.length) {
        return finish(transport_failure(expired() ? Error::TIMEOUT : Error::PROTOCOL_ERROR));
    }
    if (on && uses_v2_frontend()) {
        delay_ms(20U);
    }
    const auto readback = uses_v2_frontend()
                              ? ControlTransfer{0U,
                                                Request::GpioRead,
                                                0U,
                                                0U,
                                                1U,
                                                Direction::In,
                                                static_cast<std::uint16_t>(on ? 100U : 1000U)}
                              : make_gpio_set(0U, 0U, 1000U);
    if (control(readback, response.data()) != readback.length) {
        return finish(transport_failure(expired() ? Error::TIMEOUT : Error::PROTOCOL_ERROR));
    }
    const auto wanted = on ? spec.on_value : spec.off_value;
    if ((response[0] & spec.mask) != (wanted & spec.mask)) {
        return finish(Error::PROTOCOL_ERROR);
    }
    if (on && uses_v2_frontend() && (response[0] & 0x80U) == 0U) {
        // The vendor's digital protection condition is GPIO20 high and
        // GPIO80 low. Its electrical meaning is not inferred here.
        lnb_fault_latched_ = true;
        return finish(Error::PROTOCOL_ERROR);
    }
    lnb_on_ = on;
    lnb_state_known_ = true;
    publish_lnb_state();
    lnb_cleanup_required_ = on;
    lnb_feedback_checked_at_ = steady_now();
    if (!cleanup && cancelled()) {
        return finish(Error::NOT_READY);
    }
    if (on && uses_v2_frontend() && !start_lnb_monitor()) {
        return finish(Error::INTERNAL);
    }
    return finish(Error::OK);
}

Result<void> LibusbW3u3Hardware::check_v2_lnb_feedback() noexcept
{
    if (!uses_v2_frontend() || !lnb_on_) {
        return Result<void>::success();
    }
    // These GPIO-only controls do not alter the staged I2C buffer, so a
    // gate-owning frontend/card operation can cooperatively check feedback.
    lnb_feedback_active_ = true;
    DeadlineScope deadline(deadline_, deadline_active_, steady_now() + std::chrono::seconds(2));
    const auto read = ControlTransfer{0U, Request::GpioRead, 0U, 0U, 1U, Direction::In, 100U};
    std::array<unsigned char, 1> response{};
    const int result = control(read, response.data());
    lnb_feedback_checked_at_ = steady_now();
    if (result == read.length && (response[0] & 0xa0U) == 0xa0U) {
        lnb_feedback_active_ = false;
        return Result<void>::success();
    }
    const Error failure = result < 0 ? map_libusb_error(result) : Error::PROTOCOL_ERROR;
    // Fail closed on a protection indication or uncertain feedback. Latch
    // the fault so an in-flight retune can never rollback into ON again.
    lnb_fault_latched_ = true;
    (void)clear_lnb_power();
    lnb_feedback_active_ = false;
    mark_cleanup_failed(failure);
    return Result<void>::failure(failure);
}

bool LibusbW3u3Hardware::poll_v2_lnb_feedback_if_due() noexcept
{
    if (!uses_v2_frontend() || !lnb_on_ || lnb_feedback_active_ || lnb_power_transition_active_ ||
        disconnected_.load() ||
        steady_now() - lnb_feedback_checked_at_ < std::chrono::milliseconds(100)) {
        return true;
    }
    return check_v2_lnb_feedback().has_value();
}


bool LibusbW3u3Hardware::start_lnb_monitor() noexcept
{
    std::lock_guard<std::mutex> life(lnb_monitor_mutex_);
    if (lnb_monitor_stop_.load() || stop_requested_.load()) {
        return false;
    }
    if (lnb_monitor_.joinable()) {
        return true;
    }
    return lnb_monitor_.start(
        [](void* context) -> void* {
            static_cast<LibusbW3u3Hardware*>(context)->monitor_lnb_power();
            return nullptr;
        },
        this, capture_usb_hooks_ == nullptr ? nullptr : capture_usb_hooks_->launch_thread);
}

void LibusbW3u3Hardware::stop_lnb_monitor() noexcept
{
    std::lock_guard<std::mutex> life(lnb_monitor_mutex_);
    lnb_monitor_stop_.store(true);
    if (lnb_monitor_.joinable()) {
        lnb_monitor_.join();
    }
}

void LibusbW3u3Hardware::monitor_lnb_power() noexcept
{
    // Source V2 checks every 100 ms, including idle tuned leases. The same
    // gate serializes feedback with card, frontend and capture controller I/O.
    while (!lnb_monitor_stop_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (lnb_monitor_stop_.load()) {
            break;
        }
        while (!control_gate_.try_lock_for(std::chrono::milliseconds(10))) {
            if (lnb_monitor_stop_.load()) {
                return;
            }
        }
        std::unique_lock<std::recursive_timed_mutex> gate(control_gate_, std::adopt_lock);
        if (lnb_monitor_stop_.load()) {
            break;
        }
        if (!lnb_on_ || disconnected_.load()) {
            continue;
        }
        DeadlineScope deadline(deadline_, deadline_active_, steady_now() + std::chrono::seconds(2));
        (void)check_v2_lnb_feedback();
    }
}

Result<void> LibusbW3u3Hardware::clear_lnb_power() noexcept
{
    // Caller holds the control gate; a disconnected bridge is terminal and
    // must never receive a guessed recovery write.
    lnb_receiver_on_ = false;
    lnb_independent_on_ = false;
    if (disconnected_.load()) {
        lnb_on_ = false;
        lnb_state_known_ = false;
        publish_lnb_state();
        lnb_cleanup_required_ = false;
        lnb_transaction_active_ = false;
        lnb_transaction_receiver_ = kNoActiveReceiver;
        return Result<void>::success();
    }
    if (lnb_cleanup_required_ || lnb_on_) {
        const auto off = set_lnb_power(false, true);
        if (!off) {
            mark_cleanup_failed(off.error());
            return off;
        }
    }
    lnb_transaction_active_ = false;
    lnb_transaction_receiver_ = kNoActiveReceiver;
    return Result<void>::success();
}

Result<void> LibusbW3u3Hardware::set_independent_lnb_power(bool enabled) noexcept
{
    if (!supports_lnb_control() || (enabled && !allow_lnb_power_)) {
        return Result<void>::failure(Error::UNSUPPORTED);
    }
    const auto until = steady_now() + std::chrono::seconds(3);
    if (!acquire_control_gate(until)) {
        return Result<void>::failure(disconnected_.load()     ? Error::DISCONNECTED
                                     : cleanup_failed_.load() ? Error::USB_IO
                                     : stop_requested_.load() ? Error::NOT_READY
                                                              : Error::TIMEOUT);
    }
    std::unique_lock<std::recursive_timed_mutex> gate(control_gate_, std::adopt_lock);
    DeadlineScope deadline(deadline_, deadline_active_, until);
    // The independent request needs no receiver lease. It is aggregated with
    // the receiver's request (a pending tune counts), so neither a tune with
    // 0 V nor a receiver close drops it, and OFF never cuts a receiver's feed.
    const bool receiver_on = lnb_transaction_active_ ? lnb_requested_on_ : lnb_receiver_on_;
    const bool previous = lnb_independent_on_;
    lnb_independent_on_ = enabled;
    const auto applied = set_lnb_power(receiver_on || enabled, false);
    if (applied || !enabled) {
        // A failed OFF stays requested; the unknown state is reported as null
        // and the cleanup debt is retried by close/shutdown.
        return applied;
    }
    const Error primary = applied.error();
    lnb_independent_on_ = previous;
    if (lnb_cleanup_required_ && !disconnected_.load()) {
        const auto restored =
            set_lnb_power(lnb_fault_latched_ ? false : (receiver_on || previous), true);
        if (!restored) {
            mark_cleanup_failed(restored.error());
            (void)clear_lnb_power();
        }
    }
    if (lnb_fault_latched_) {
        mark_cleanup_failed(primary);
    }
    return Result<void>::failure(primary);
}

dtv::userland::wire::Optional<bool> LibusbW3u3Hardware::independent_lnb_power_state() noexcept
{
    dtv::userland::wire::Optional<bool> result{};
    const std::uint8_t state = lnb_published_.load(std::memory_order_acquire);
    if (state != 0U && !disconnected_.load()) {
        result.present = true;
        result.value = state == 2U;
    }
    return result;
}

Result<void> LibusbW3u3Hardware::begin_tune_power(std::uint8_t receiver, System system,
                                                  std::uint8_t lnb_voltage) noexcept
{
    if (lnb_voltage != 0U && lnb_voltage != 15U) {
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    }
    if (!receiver_supports(receiver, system) || (lnb_voltage != 0U && system != System::ISDB_S)) {
        return Result<void>::failure(Error::UNSUPPORTED);
    }
    if (lnb_voltage != 0U && !supports_lnb_control()) {
        return Result<void>::failure(Error::UNSUPPORTED);
    }
    if (lnb_voltage == 15U && !allow_lnb_power_) {
        return Result<void>::failure(Error::UNSUPPORTED);
    }
    const auto until = steady_now() + std::chrono::seconds(3);
    if (!acquire_control_gate(until)) {
        return Result<void>::failure(disconnected_.load()     ? Error::DISCONNECTED
                                     : cleanup_failed_.load() ? Error::USB_IO
                                     : stop_requested_.load() ? Error::NOT_READY
                                                              : Error::TIMEOUT);
    }
    std::unique_lock<std::recursive_timed_mutex> gate(control_gate_, std::adopt_lock);
    ReceiverRouteScope route_scope(cleanup_route_receiver_, receiver);
    if (!active_receiver_.owns(receiver)) {
        return Result<void>::failure(Error::UNSUPPORTED);
    }
    if (!initialized_ || !claimed_) {
        return Result<void>::failure(Error::NOT_READY);
    }
    const bool receiver_prepared = source_prepared_[receiver];
    const bool peer_satellite_powered = any_source_prepared() && (lnb_on_ || lnb_cleanup_required_);
    if (receiver_prepared || lnb_transaction_active_ || peer_satellite_powered) {
        if (startup_config_.enabled) {
            unsigned prepared_mask = 0U;
            for (std::size_t index = 0U; index < source_prepared_.size(); ++index) {
                if (source_prepared_[index]) prepared_mask |= 1U << index;
            }
            std::fprintf(stderr,
                         "asicen-tune-power-rejected receiver=%u reason=%s "
                         "prepared_mask=%u lnb_on=%u lnb_cleanup_required=%u "
                         "transaction_active=%u transaction_receiver=%u\n",
                         static_cast<unsigned>(receiver),
                         receiver_prepared ? "receiver_capture" :
                         lnb_transaction_active_ ? "power_transaction" : "peer_satellite_power",
                         prepared_mask, lnb_on_ ? 1U : 0U,
                         lnb_cleanup_required_ ? 1U : 0U,
                         lnb_transaction_active_ ? 1U : 0U,
                         static_cast<unsigned>(lnb_transaction_receiver_));
        }
        return Result<void>::failure(Error::BUSY);
    }
    DeadlineScope deadline(deadline_, deadline_active_, until);
    lnb_requested_on_ = lnb_voltage == 15U;
    lnb_transaction_active_ = true;
    lnb_transaction_receiver_ = receiver;
    // The pending request determines the feed throughout acquisition, just as
    // in PX4. A failed tune restores the previous committed request. An
    // independent (ctl) request keeps the feed on regardless of the tune.
    const Result<void> prepared = set_lnb_power(lnb_requested_on_ || lnb_independent_on_, false);
    if (!prepared) {
        const Error primary = prepared.error();
        if (lnb_cleanup_required_ && !disconnected_.load()) {
            const auto restored =
                set_lnb_power(lnb_fault_latched_ ? false : lnb_committed_request(), true);
            if (!restored) {
                mark_cleanup_failed(restored.error());
                (void)clear_lnb_power();
            }
        }
        if (lnb_fault_latched_) {
            mark_cleanup_failed(primary);
        }
        lnb_transaction_active_ = false;
        lnb_transaction_receiver_ = kNoActiveReceiver;
        return Result<void>::failure(primary);
    }
    return Result<void>::success();
}

Result<void> LibusbW3u3Hardware::commit_tune_power(std::uint8_t receiver) noexcept
{
    const auto until = steady_now() + std::chrono::seconds(3);
    if (!acquire_control_gate(until)) {
        return Result<void>::failure(disconnected_.load()     ? Error::DISCONNECTED
                                     : cleanup_failed_.load() ? Error::USB_IO
                                     : stop_requested_.load() ? Error::NOT_READY
                                                              : Error::TIMEOUT);
    }
    std::unique_lock<std::recursive_timed_mutex> gate(control_gate_, std::adopt_lock);
    ReceiverRouteScope route_scope(cleanup_route_receiver_, receiver);
    if (!active_receiver_.owns(receiver) || !lnb_transaction_active_ ||
        lnb_transaction_receiver_ != receiver) {
        return Result<void>::failure(Error::NOT_READY);
    }
    DeadlineScope deadline(deadline_, deadline_active_, until);
    if (!lnb_requested_on_ && !lnb_independent_on_ && lnb_on_) {
        const auto off = set_lnb_power(false, false);
        if (!off) {
            return off;  // The service will pair this with rollback.
        }
    }
    lnb_receiver_on_ = lnb_requested_on_;
    lnb_transaction_active_ = false;
    lnb_transaction_receiver_ = kNoActiveReceiver;
    return Result<void>::success();
}

Result<void> LibusbW3u3Hardware::rollback_tune_power(std::uint8_t receiver) noexcept
{
    const auto until = steady_now() + std::chrono::seconds(3);
    if (!acquire_control_gate(until, nullptr, true)) {
        mark_cleanup_failed(Error::TIMEOUT);
        return Result<void>::failure(Error::TIMEOUT);
    }
    std::unique_lock<std::recursive_timed_mutex> gate(control_gate_, std::adopt_lock);
    ReceiverRouteScope route_scope(cleanup_route_receiver_, receiver);
    if (!active_receiver_.owns(receiver)) {
        return Result<void>::failure(Error::UNSUPPORTED);
    }
    if (!lnb_transaction_active_) {
        return Result<void>::success();
    }
    if (lnb_transaction_receiver_ != receiver) {
        return Result<void>::failure(Error::UNSUPPORTED);
    }
    if (disconnected_.load()) {
        return clear_lnb_power();
    }
    if (!lnb_state_known_ || lnb_on_ != lnb_committed_request() || lnb_fault_latched_) {
        const auto restored =
            set_lnb_power(lnb_fault_latched_ ? false : lnb_committed_request(), true);
        if (!restored) {
            mark_cleanup_failed(restored.error());
            (void)clear_lnb_power();
            return restored;
        }
    }
    lnb_transaction_active_ = false;
    lnb_transaction_receiver_ = kNoActiveReceiver;
    return Result<void>::success();
}
void LibusbW3u3Hardware::mark_receiver_disconnected(std::uint8_t receiver) noexcept
{
    if (receiver <= 1U) {
        disconnected_.store(true);
        interrupt();
    }
}
void LibusbW3u3Hardware::request_stop() noexcept
{
    stop_requested_.store(true);
    interrupt();
}

std::chrono::steady_clock::time_point LibusbW3u3Hardware::steady_now() const noexcept
{
    if (capture_usb_hooks_ != nullptr && capture_usb_hooks_->now != nullptr) {
        return capture_usb_hooks_->now(capture_usb_hooks_->context);
    }
    return std::chrono::steady_clock::now();
}

bool LibusbW3u3Hardware::acquire_control_gate(std::chrono::steady_clock::time_point deadline,
                                              const volatile std::sig_atomic_t* stop_flag,
                                              bool cleanup) noexcept
{
    for (;;) {
        const auto now = steady_now();
        if (now >= deadline) {
            return false;
        }
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
                         stop_requested_.load() || (stop_flag != nullptr && *stop_flag != 0))) {
            return false;
        }
    }
}

void LibusbW3u3Hardware::mark_cleanup_failed(Error error) noexcept
{
    int expected = 0;
    (void)cleanup_failure_error_.compare_exchange_strong(expected, static_cast<int>(error),
                                                         std::memory_order_acq_rel);
    cleanup_failed_.store(true, std::memory_order_release);
    for (auto& interrupted : capture_interrupted_) {
        interrupted.store(true, std::memory_order_release);
    }
    if (capture_usb_hooks_ != nullptr && capture_usb_hooks_->interrupt_events != nullptr) {
        capture_usb_hooks_->interrupt_events(capture_usb_hooks_->context);
    } else if (context_ != nullptr) {
        libusb_interrupt_event_handler(context_);
    }
}

void LibusbW3u3Hardware::mark_capture_cleanup_failed(std::uint8_t receiver,
                                                     Error error) noexcept
{
    const auto* const coordinator = capture_coordinator(receiver);
    if (coordinator != nullptr && coordinator->quarantined()) return;
    mark_cleanup_failed(error);
}

Result<void>
LibusbW3u3Hardware::begin_card_operation(std::uint32_t timeout_ms,
                                         const volatile std::sig_atomic_t* stop_flag) noexcept
{
    if (!runtime_model_supported() || !claimed_ || !initialized_ || cleanup_failed_.load() ||
        disconnected_.load() || timeout_ms == 0U) {
        return Result<void>::failure(timeout_ms == 0U             ? Error::INVALID_ARGUMENT
                                     : disconnected_.load()       ? Error::DISCONNECTED
                                     : cleanup_failed_.load()     ? Error::USB_IO
                                     : !runtime_model_supported() ? Error::UNSUPPORTED
                                                                  : Error::NOT_READY);
    }
    const auto deadline = steady_now() + std::chrono::milliseconds(timeout_ms);
    if (!acquire_control_gate(deadline, stop_flag)) {
        return Result<void>::failure(
            disconnected_.load()     ? Error::DISCONNECTED
            : cleanup_failed_.load() ? Error::USB_IO
            : stop_requested_.load() || (stop_flag != nullptr && *stop_flag != 0) ? Error::NOT_READY
                                                                                  : Error::TIMEOUT);
    }
    if (cleanup_failed_.load() || disconnected_.load()) {
        control_gate_.unlock();
        return Result<void>::failure(disconnected_.load() ? Error::DISCONNECTED : Error::USB_IO);
    }
    diagnostic_stop_flag_ = stop_flag;
    deadline_ = deadline;
    deadline_active_ = true;
    return Result<void>::success();
}

void LibusbW3u3Hardware::end_card_operation() noexcept
{
    deadline_active_ = false;
    diagnostic_stop_flag_ = nullptr;
    control_gate_.unlock();
}

Result<void> LibusbW3u3Hardware::begin_card_cleanup(std::uint32_t timeout_ms) noexcept
{
    if (!runtime_model_supported() || !claimed_ || disconnected_.load() || timeout_ms == 0U) {
        const Error error = timeout_ms == 0U             ? Error::INVALID_ARGUMENT
                            : disconnected_.load()       ? Error::DISCONNECTED
                            : !runtime_model_supported() ? Error::UNSUPPORTED
                                                         : Error::NOT_READY;
        mark_cleanup_failed(error);
        return Result<void>::failure(error);
    }
    const auto deadline = steady_now() + std::chrono::milliseconds(timeout_ms);
    if (!acquire_control_gate(deadline, nullptr, true)) {
        mark_cleanup_failed(Error::TIMEOUT);
        return Result<void>::failure(Error::TIMEOUT);
    }
    diagnostic_stop_flag_ = nullptr;
    deadline_ = deadline;
    deadline_active_ = true;
    card_cleanup_active_ = true;
    return Result<void>::success();
}

void LibusbW3u3Hardware::end_card_cleanup(bool cleanup_succeeded) noexcept
{
    if (!cleanup_succeeded) {
        mark_cleanup_failed(Error::USB_IO);
    }
    card_cleanup_active_ = false;
    deadline_active_ = false;
    diagnostic_stop_flag_ = nullptr;
    control_gate_.unlock();
}

Result<void> LibusbW3u3Hardware::shutdown() noexcept
{
    stop_requested_.store(true);
    stop_lnb_monitor();
    if (abandoned_) {
        return Result<void>::failure(Error::USB_IO);
    }
    const auto gate_deadline = steady_now() + std::chrono::seconds(10);
    if (!acquire_control_gate(gate_deadline, nullptr, true)) {
        mark_cleanup_failed(Error::TIMEOUT);
        return Result<void>::failure(Error::TIMEOUT);
    }
    std::unique_lock<std::recursive_timed_mutex> gate(control_gate_, std::adopt_lock);
    AtomicFlagScope cleanup_scope(cleanup_io_active_);
    const bool gpio_snapshot_valid =
        std::any_of(gpio_snapshot_valid_functions_.begin(), gpio_snapshot_valid_functions_.end(),
                    [](bool valid) { return valid; });
    bool capture_cleanup_pending = false;
    for (std::uint8_t receiver = 0U; receiver < receiver_count(); ++receiver) {
        const auto& capture = receiver_capture_states_[receiver];
        const AsyncState* const async = capture_async(receiver);
        capture_cleanup_pending = capture_cleanup_pending || source_prepared_[receiver] ||
            capture.cf_snapshot_valid || capture.link_snapshot_valid ||
            (async != nullptr && async->prepared);
    }
    const bool needs_stop = capture_cleanup_pending || gpio_snapshot_valid;
    // OFF is independent of controller/CF restoration and receives its own
    // cleanup budget even when a previous step has quarantined the backend.
    (void)clear_lnb_power();
    if (!attempt_hardware_shutdown_cleanup(*this, needs_stop, gpio_snapshot_valid)) {
        mark_cleanup_failed(Error::USB_IO);
    }
    if (!cleanup_failed_.load(std::memory_order_acquire)) {
        return Result<void>::success();
    }
    const int stored = cleanup_failure_error_.load(std::memory_order_acquire);
    return Result<void>::failure(stored == 0 ? Error::USB_IO : static_cast<Error>(stored));
}

bool LibusbW3u3Hardware::stop_capture_safely() noexcept
{
    // Failed callback drains retain their callback graph in quarantine.
    return stop().has_value();
}

bool LibusbW3u3Hardware::snapshot_gpio_state(std::uint8_t receiver) noexcept
{
    if (profile_ == nullptr) {
        return false;
    }
    const auto route = receiver_address(*profile_, receiver);
    if (!route || route.value().rf_function_index >= gpio_snapshot_valid_functions_.size()) {
        return false;
    }
    const auto function = route.value().rf_function_index;
    if (!snapshot_gpio_if_needed(&gpio_snapshot_valid_functions_[function],
                                 &gpio_snapshots_[function],
                                 [this](std::uint8_t* value) {
                                     const auto transfer = make_gpio_set(0U, 0U, 1000U);
                                     std::array<unsigned char, 1> response{};
                                     if (control(transfer, response.data()) != transfer.length) {
                                         return false;
                                     }
                                     *value = response[0];
                                     return true;
                                 })) {
        return false;
    }
    return true;
}

bool LibusbW3u3Hardware::restore_gpio_snapshot_safely() noexcept
{
    if (abandoned_) {
        return false;
    }
    if (disconnected_.load()) {
        return true;  // USB writes are forbidden after loss.
    }
    std::array<bool, 2U> restored_functions{};
    for (std::uint8_t receiver = 0U; receiver < receiver_count(); ++receiver) {
        const auto route = receiver_address(*profile_, receiver);
        if (!route || route.value().rf_function_index >= restored_functions.size() ||
            restored_functions[route.value().rf_function_index]) {
            continue;
        }
        const auto function = route.value().rf_function_index;
        restored_functions[function] = true;
        if (gpio_snapshot_valid_functions_[function] ||
            board_power_attempted_functions_[function]) {
            if (!restore_gpio_snapshot_for(receiver)) {
                return false;
            }
        }
    }
    return true;
}

bool LibusbW3u3Hardware::restore_gpio_snapshot_for(std::uint8_t receiver) noexcept
{
    if (abandoned_) {
        return false;
    }
    if (disconnected_.load()) {
        return true;
    }
    if (profile_ == nullptr) {
        return false;
    }
    const auto route = receiver_address(*profile_, receiver);
    if (!route || route.value().rf_function_index >= gpio_snapshot_valid_functions_.size()) {
        return false;
    }
    const auto function = route.value().rf_function_index;
    const auto previous_route = cleanup_route_receiver_.exchange(receiver, std::memory_order_acq_rel);
    const auto finish = [this, previous_route](bool success) {
        cleanup_route_receiver_.store(previous_route, std::memory_order_release);
        return success;
    };
    if (uses_legacy_frontend() || uses_v2_frontend()) {
        // GPIOExGet samples physical pins, not the previous output latch.
        // A sampled-low released line must never be restored as driven-low.
        // Use the documented board-off sequence instead of claiming rollback
        // to unobservable GPIOEx latch state. V2 also has a source-defined
        // idle path which does not restore unproven GPIO20 electrical state.
        if (!board_power_attempted_functions_[function]) {
            gpio_snapshot_valid_functions_[function] = false;
            return finish(true);
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
            if (run_frontend_plan(FrontendPlan{operation}, this) != FrontendRunResult::Completed) {
                off = false;
            }
        }
        if (expired()) {
            off = false;
        }
        deadline_ = previous_deadline;
        deadline_active_ = previous_active;
        if (off) {
            board_power_attempted_functions_[function] = false;
            gpio_snapshot_valid_functions_[function] = false;
            initialized_functions_[function] = false;
            initialized_ = std::any_of(initialized_functions_.begin(),
                                       initialized_functions_.end(),
                                       [](bool initialized) { return initialized; });
            if (!initialized_) v2_identity_verified_ = false;
        }
        return finish(off);
    }
    const std::uint8_t mask = 0xdfU;
    const auto restore = make_gpio_set(gpio_snapshots_[function], mask, 1000U);
    std::array<unsigned char, 1> response{};
    bool restored = control(restore, response.data()) == restore.length;
    const auto readback = make_gpio_set(0U, 0U, 1000U);
    restored = control(readback, response.data()) == readback.length &&
               (response[0] & mask) == (gpio_snapshots_[function] & mask) && restored;
    if (restored) {
        gpio_snapshot_valid_functions_[function] = false;
        board_power_attempted_functions_[function] = false;
    }
    return finish(restored);
}

bool LibusbW3u3Hardware::run_plan(const FrontendPlan& plan, unsigned timeout_ms,
                                  FrontendRunReport* report, std::uint8_t receiver) noexcept
{
    plan_error_ = Error::OK;
    if (!claimed_ || disconnected_.load() || plan.empty()) {
        plan_error_ = disconnected_.load() ? Error::DISCONNECTED
                      : plan.empty()       ? Error::INVALID_ARGUMENT
                                           : Error::NOT_READY;
        return false;
    }
    const auto operation_deadline = steady_now() + std::chrono::milliseconds(timeout_ms);
    if (!acquire_control_gate(operation_deadline)) {
        plan_error_ = disconnected_.load()     ? Error::DISCONNECTED
                      : stop_requested_.load() ? Error::NOT_READY
                                               : Error::TIMEOUT;
        return false;
    }
    std::unique_lock<std::recursive_timed_mutex> gate(control_gate_, std::adopt_lock);
    ReceiverRouteScope route_scope(cleanup_route_receiver_, receiver);
    first_transport_error_.store(0);
    const auto previous_deadline = deadline_;
    const bool previous_active = deadline_active_;
    deadline_ =
        previous_active ? std::min(previous_deadline, operation_deadline) : operation_deadline;
    deadline_active_ = true;
    const auto result = run_frontend_plan(plan, this, report);
    switch (result) {
    case FrontendRunResult::Completed:
        break;
    case FrontendRunResult::Cancelled:
        plan_error_ = Error::NOT_READY;
        break;
    case FrontendRunResult::DeadlineExceeded:
        plan_error_ = Error::TIMEOUT;
        break;
    case FrontendRunResult::InvalidArgument:
        plan_error_ = Error::INVALID_ARGUMENT;
        break;
    case FrontendRunResult::ShortTransfer:
        plan_error_ = Error::PROTOCOL_ERROR;
        break;
    case FrontendRunResult::FailedTransfer:
        plan_error_ = transport_failure(cancelled() ? Error::NOT_READY
                                        : expired() ? Error::TIMEOUT
                                                    : Error::USB_IO);
        break;
    }
    deadline_ = previous_deadline;
    deadline_active_ = previous_active;
    return result == FrontendRunResult::Completed;
}

int LibusbW3u3Hardware::control(const ControlTransfer& original, unsigned char* data)
{
    std::uint8_t function = 0U;
    const auto cleanup_receiver = cleanup_route_receiver_.load(std::memory_order_acquire);
    const auto active_receiver = active_receiver_.owner();
    const auto route_receiver = cleanup_receiver != kNoActiveReceiver
                                    ? cleanup_receiver
                                    : active_receiver;
    if (profile_ != nullptr && route_receiver != kNoActiveReceiver) {
        const auto route = receiver_address(*profile_, route_receiver);
        if (!route) {
            return LIBUSB_ERROR_INVALID_PARAM;
        }
        function = route.value().rf_function_index;
    }
    return control_function(function, original, data);
}

int LibusbW3u3Hardware::control_function(std::uint8_t function, const ControlTransfer& original,
                                         unsigned char* data)
{
    const int result = control_function_impl(function, original, data);
    if (startup_trace_active_.load() &&
        (original.request == Request::I2cWrite ||
         original.request == Request::I2cWriteNoStop ||
         original.request == Request::DscStart || original.request == Request::DscStop)) {
        record_startup("usb_control", function, static_cast<std::uint8_t>(original.request),
                       original.value, original.length, result);
    }
    if (result < 0 || result != original.length) {
        int expected = 0;
        const bool first_error =
            first_transport_error_.compare_exchange_strong(expected, result < 0 ? result : 1);
#if defined(DTV_USERLAND_DIAGNOSTICS)
        if (first_error && prepare_runtime_diagnostic_active) {
            const Error normalized = result < 0 ? map_libusb_error(result) : Error::PROTOCOL_ERROR;
            dtv::userland::detail::diagnostic_log(
                "asicen_prepare_control_error receiver=%u function=%u request_type=0x%02x request=0x%02x tuner=%u value=0x%04x index=0x%04x length=%u result=%d error=%u",
                static_cast<unsigned>(cleanup_route_receiver_.load(std::memory_order_acquire)),
                static_cast<unsigned>(function),
                static_cast<unsigned>(original.direction == Direction::In ? kVendorIn : kVendorOut),
                static_cast<unsigned>(original.request),
                static_cast<unsigned>(original.tuner_num), static_cast<unsigned>(original.value),
                static_cast<unsigned>(original.index), static_cast<unsigned>(original.length),
                result, static_cast<unsigned>(normalized));
        }
#else
        (void)first_error;
#endif
    }
    return result;
}

Error LibusbW3u3Hardware::transport_failure(Error fallback) const noexcept
{
    const int error = first_transport_error_.load();
    if (error < 0) {
        return map_libusb_error(error);
    }
    if (error > 0) {
        return Error::PROTOCOL_ERROR;
    }
    return fallback;
}

int LibusbW3u3Hardware::control_function_impl(std::uint8_t function,
                                              const ControlTransfer& original, unsigned char* data)
{
    if (!original.valid || (original.length != 0U && data == nullptr)) {
        return LIBUSB_ERROR_INVALID_PARAM;
    }
    // Defense in depth: no unsupported profile can bypass the public entry
    // guards through card, diagnostic, teardown or future plan dispatch.
    if (!runtime_model_supported()) {
        return LIBUSB_ERROR_NOT_SUPPORTED;
    }
    // V2 keeps its secondary function read-only for the source-backed pair checks.
    if (function > 1U ||
        (function == 1U && uses_v2_frontend() &&
         (original.direction != Direction::In ||
          !(original.request == Request::CustomerInfo ||
            (original.request == Request::SysCtrlRead && original.value == 2U &&
             original.index == 0U && original.length == 3U) ||
            (original.request == Request::I2cRead && original.value == 0xb0a8U &&
             original.index == 0U && (original.length == 2U || original.length == 17U)))))) {
        return LIBUSB_ERROR_NOT_SUPPORTED;
    }
    if (uses_v2_frontend() && !v2_roles_verified_ && original.request != Request::SysCtrlRead &&
        original.request != Request::CustomerInfo) {
        return LIBUSB_ERROR_ACCESS;
    }
    const unsigned requested_timeout = original.timeout_ms == 0U ? 1000U : original.timeout_ms;
    const auto gate_deadline = steady_now() + std::chrono::milliseconds(requested_timeout);
    if (!acquire_control_gate(gate_deadline, nullptr, true)) {
        return LIBUSB_ERROR_TIMEOUT;
    }
    std::unique_lock<std::recursive_timed_mutex> gate(control_gate_, std::adopt_lock);
    if (cleanup_failed_.load(std::memory_order_acquire) && !card_cleanup_active_ &&
        !cleanup_io_active_.load(std::memory_order_acquire)) {
        return LIBUSB_ERROR_ACCESS;
    }
    if (!claimed_ || disconnected_.load()) {
        return LIBUSB_ERROR_NO_DEVICE;
    }
    if (!poll_v2_lnb_feedback_if_due()) {
        return disconnected_.load() ? LIBUSB_ERROR_NO_DEVICE : LIBUSB_ERROR_IO;
    }
    ControlTransfer transfer = original;
    if (uses_v2_frontend() && lnb_on_ && !lnb_feedback_active_ && !lnb_power_transition_active_) {
        // Do not let one normal USB request monopolize the bridge beyond the
        // source watchdog cadence. A timeout fails the operation normally.
        transfer.timeout_ms =
            static_cast<std::uint16_t>(std::min<unsigned>(100U, requested_timeout));
    }
    const auto lnb = lnb_gpio_control(profile_);
    const bool exact_lnb_write =
        function == 0U && lnb_gpio_io_active_ && transfer.request == Request::Gpio &&
        transfer.direction == Direction::In && transfer.index == 0U && transfer.length == 1U &&
        (transfer.value == setup_word(lnb.on_value, lnb.mask) ||
         transfer.value == setup_word(lnb.off_value, lnb.mask));
    if ((!uses_legacy_frontend() && !uses_v2_frontend()) || lnb.supported) {
        // Only the narrow, gate-held LNB transaction may address bit20.
        // Retain the original W3 GPIOEx prohibition and mask startup/restore
        // plans for V2 as well; S3 board reset lines have different meanings.
        if (!exact_lnb_write) {
            bool skip = false;
            ControlTransfer safe{};
            if (!mask_lnb_gpio_operation(transfer, &safe, &skip)) {
                return LIBUSB_ERROR_ACCESS;
            }
            if (skip) {
                if (data != nullptr && transfer.length > 0) {
                    data[0] = 1U;
                }
                return transfer.length;
            }
            transfer = safe;
        }
    }
    if (deadline_active_) {
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline_ - steady_now()).count();
        if (left <= 0) {
            return LIBUSB_ERROR_TIMEOUT;
        }
        const unsigned requested = transfer.timeout_ms == 0U ? 1000U : transfer.timeout_ms;
        transfer.timeout_ms = static_cast<std::uint16_t>(std::min<long long>(left, requested));
    }
    int rc = LIBUSB_ERROR_NOT_SUPPORTED;
    if (capture_usb_hooks_ != nullptr && capture_usb_hooks_->control_function != nullptr) {
        rc = capture_usb_hooks_->control_function(capture_usb_hooks_->context, function, transfer,
                                                  data);
    } else if (function == 0U && capture_usb_hooks_ != nullptr &&
               capture_usb_hooks_->control != nullptr) {
        rc = capture_usb_hooks_->control(capture_usb_hooks_->context, transfer, data);
    } else {
        rc = function == 0U ? primary_.control(transfer, data) : sibling_.control(transfer, data);
    }
    if (rc == LIBUSB_ERROR_NO_DEVICE) {
        disconnected_.store(true);
    }
    if (rc >= 0 && !poll_v2_lnb_feedback_if_due()) {
        return disconnected_.load() ? LIBUSB_ERROR_NO_DEVICE : LIBUSB_ERROR_IO;
    }
    return rc;
}

libusb_device_handle* LibusbW3u3Hardware::capture_handle(std::uint8_t receiver) const noexcept
{
    if (profile_ == nullptr) {
        return nullptr;
    }
    const auto route = receiver_address(*profile_, receiver);
    if (!route) {
        return nullptr;
    }
    if (capture_usb_hooks_ != nullptr && capture_usb_hooks_->handle_for_function != nullptr) {
        return capture_usb_hooks_->handle_for_function(capture_usb_hooks_->context,
                                                       route.value().function_index);
    }
    return route.value().function_index == 0U ? primary_.handle() : sibling_.handle();
}
void LibusbW3u3Hardware::delay_ms(unsigned ms)
{
    const auto until = steady_now() + std::chrono::milliseconds(ms);
    for (;;) {
        if (cancelled() || !poll_v2_lnb_feedback_if_due()) {
            return;
        }
        auto end = deadline_active_ ? std::min(until, deadline_) : until;
        if (uses_v2_frontend() && lnb_on_ && !lnb_power_transition_active_) {
            end = std::min(end, steady_now() + std::chrono::milliseconds(100));
        }
        const auto now = steady_now();
        if (end <= now) {
            return;
        }
        std::this_thread::sleep_for(end - now);
        if (steady_now() >= until) {
            (void)poll_v2_lnb_feedback_if_due();
            return;
        }
    }
}
bool LibusbW3u3Hardware::cancelled() const
{
    if (cleanup_io_active_.load(std::memory_order_acquire)) {
        return false;
    }
    return (!card_cleanup_active_ && stop_requested_.load()) ||
           (diagnostic_stop_flag_ != nullptr && *diagnostic_stop_flag_ != 0) ||
           (diagnostic_stop_callback_ != nullptr && diagnostic_stop_callback_());
}
bool LibusbW3u3Hardware::expired() const
{
    return deadline_active_ && steady_now() >= deadline_;
}

Result<void> LibusbW3u3Hardware::prepare(std::uint8_t receiver, System system,
                                         const std::atomic<bool>& cancelled_flag) noexcept
{
    if (!frontend_io_allowed_after_cleanup(cleanup_failed_.load())) {
        return Result<void>::failure(Error::USB_IO);
    }
    const auto gate_deadline = steady_now() + std::chrono::seconds(15);
    if (!acquire_control_gate(gate_deadline, nullptr)) {
        return Result<void>::failure(Error::TIMEOUT);
    }
    std::unique_lock<std::recursive_timed_mutex> gate(control_gate_, std::adopt_lock);
    ReceiverRouteScope route_scope(cleanup_route_receiver_, receiver);
    first_transport_error_.store(0);
    if (!receiver_supports(receiver, system) || !claimed_ || !active_receiver_.owns(receiver) ||
        !receiver_initialized(receiver) || !receiver_tune_states_[receiver].tuned ||
        receiver_tune_states_[receiver].system != system || source_prepared_[receiver] ||
        stop_requested_.load()) {
        return Result<void>::failure(Error::UNSUPPORTED);
    }
    const std::uint8_t local = receiver_local(receiver);
    const std::uint8_t endpoint = bulk_endpoint_for_local(local);
    libusb_device_handle* const handle = capture_handle(receiver);
    if (endpoint == 0U || handle == nullptr) {
        return Result<void>::failure(Error::UNSUPPORTED);
    }
    source_receiver_ = receiver;
    routed_source_receiver_.store(receiver, std::memory_order_release);
    counters_receiver_ = receiver;
    auto& capture = receiver_capture_states_[receiver];
    capture.dsc_attempted = false;
    capture.dsc_stopped = true;
    capture.cf_snapshot_valid = false;
    capture.link_snapshot_valid = false;
    capture.link_apply_attempted = false;
    capture.output_start_attempted = false;
    capture_interrupted_[receiver].store(false);
    record_startup("capture_prepare_begin", receiver, local, static_cast<int>(system));
    const auto fail_prepare = [this, receiver](Error error) {
        const Error original_error = error;
        error = transport_failure(error);
        record_startup("capture_prepare_failed", static_cast<int>(original_error),
                       static_cast<int>(error), receiver);
        const CaptureRunResult cleanup = stop_and_drain(
            receiver, receiver_capture_states_[receiver].dsc_attempted);
        if (startup_config_.enabled) {
            const AsyncState* const async = capture_async(receiver);
            bool async_prepared = false;
            if (async != nullptr) {
                std::lock_guard<std::mutex> lock(async->mutex);
                async_prepared = async->prepared;
            }
            const auto& capture = receiver_capture_states_[receiver];
            std::fprintf(stderr,
                         "asicen-capture-prepare-cleanup receiver=%u original_error=%u "
                         "mapped_error=%u drain_result=%u source_prepared=%u "
                         "async_prepared=%u cf_snapshot=%u link_snapshot=%u "
                         "dsc_attempted=%u cleanup_failed=%u\n",
                         static_cast<unsigned>(receiver),
                         static_cast<unsigned>(original_error),
                         static_cast<unsigned>(error), static_cast<unsigned>(cleanup),
                         source_prepared_[receiver] ? 1U : 0U, async_prepared ? 1U : 0U,
                         capture.cf_snapshot_valid ? 1U : 0U,
                         capture.link_snapshot_valid ? 1U : 0U,
                         capture.dsc_attempted ? 1U : 0U,
                         cleanup_failed_.load() ? 1U : 0U);
        }
        if (cleanup == CaptureRunResult::fatal_drain) {
            return Result<void>::failure(Error::USB_IO);
        }
        if (cleanup != CaptureRunResult::cancelled) {
            mark_capture_cleanup_failed(receiver, Error::USB_IO);
        }
        return Result<void>::failure(error);
    };
    if (cancelled_flag.load()) {
        return fail_prepare(Error::NOT_READY);
    }
    if (uses_legacy_frontend() && system == System::ISDB_T &&
        !receiver_tune_states_[receiver].gain_applied) {
        // These tune wrappers establish the vendor's default cached state.
        // Apply the model-specific one-shot once per tune, without importing
        // W3U3's lock-settle/retry policy or starting a polling thread.
        if (!run_plan(plan_legacy_default_gain(legacy_frontend(), true), 1000U, nullptr, receiver)) {
            return fail_prepare(plan_error_);
        }
        receiver_tune_states_[receiver].gain_applied = true;
    }
    if (!snapshot_link_diagnostic()) {
        return fail_prepare(Error::NOT_READY);
    }
    capture.link_snapshot_valid = true;
    record_startup("capture_link_snapshot_complete", receiver);
    capture.cf_snapshot_valid = read_cf_block(local, capture.cf_snapshot.data(),
                                               capture.cf_snapshot.size());
    if (!capture.cf_snapshot_valid) {
        return fail_prepare(Error::USB_IO);
    }
    record_startup("capture_cf_snapshot_complete", receiver);
#if defined(_WIN32)
    px4::userland::ipc::windows::WindowsTunerNonceSource nonce;
#else
    px4::userland::ipc::posix::PosixTunerNonceSource nonce;
#endif
    const auto seed = nonce.generate();
    if (!seed) {
        capture.link_seed.fill(0U);
        return fail_prepare(seed.error());
    }
    capture.link_seed = seed.value();
    record_startup("capture_nonce_complete", receiver);
    record_startup("filter_reset_begin");
    if (!run_plan(plan_stream_setup(local, 1U), 5000U, nullptr, receiver)) {
        return fail_prepare(plan_error_);
    }
    record_startup("filter_reset_complete");
    if (!set_cf_bit(local, 0x03U, true)) {
        return fail_prepare(Error::USB_IO);
    }
    if (async_states_[receiver] == nullptr) {
        async_states_[receiver] = new (std::nothrow) AsyncState();
        if (!async_states_[receiver]) {
            return fail_prepare(Error::INTERNAL);
        }
    }
    AsyncState& state = *async_states_[receiver];
    bool already_prepared = false;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        already_prepared = state.prepared;
        state.next_submission_order = state.next_delivery_order = 0U;
        state.drain_failed = false;
        state.split_completion_failed = false;
        state.split_draining = false;
        state.split_callback_result = CaptureRunResult::cancelled;
        for (auto& slot : state.slots) {
            slot.owner = &state;
            slot.status = LIBUSB_TRANSFER_ERROR;
            slot.actual = 0;
            slot.pending = false;
            slot.ready = false;
            slot.generation = 0U;
        }
    }
    if (already_prepared) {
        return fail_prepare(Error::BUSY);
    }
    state.endpoint = endpoint;
#if defined(_WIN32)
    if (capture_usb_hooks_ == nullptr) {
        std::size_t transfer_size = kChunkSize;
        const int result = prepare_libusb_stream(handle, endpoint, &transfer_size,
                                                 &state.raw_io_enabled);
        if (result != 0) {
            return fail_prepare(map_libusb_error(result));
        }
        if (transfer_size != kChunkSize) {
            return fail_prepare(Error::INVALID_ARGUMENT);
        }
    }
#endif
    for (auto& slot : state.slots) {
        slot.transfer = allocate_transfer();
        if (slot.transfer == nullptr) {
            return fail_prepare(Error::INTERNAL);
        }
        libusb_fill_bulk_transfer(slot.transfer, handle, endpoint, slot.buffer.data(),
                                  static_cast<int>(kChunkSize), AsyncState::completed,
                                  &slot, 0U);
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
            slot.submission_order = state.next_submission_order++;
        }
        if (submit_transfer(slot.transfer) != 0) {
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                slot.pending = false;
            }
            return fail_prepare(Error::USB_IO);
        }
    }
    record_startup("initial_urbs_submitted", kQueueDepth, kChunkSize);
    capture.dsc_attempted = true;
    capture.dsc_stopped = false;
    if (!dsc_start(local)) {
        return fail_prepare(Error::USB_IO);
    }
    record_startup("dsc_started");
    capture.output_start_attempted = true;
    if (!set_cf_bit(local, 0x08U, true)) {
        return fail_prepare(Error::USB_IO);
    }
    record_startup("cf40_started");
    capture.link_apply_attempted = true;
    if (!apply_link_seed()) {
        return fail_prepare(Error::USB_IO);
    }
    record_startup("controller_output_enabled");
    if (!decoders_[receiver]) {
        decoders_[receiver].reset(
            new (std::nothrow) TransportCaptureDecoderV7(capture.link_seed.data(),
                                                         capture.link_seed.size()));
    } else {
        decoders_[receiver]->clear();
        *decoders_[receiver] =
            TransportCaptureDecoderV7(capture.link_seed.data(), capture.link_seed.size());
    }
    if (!capture_decoder(receiver) || !capture_decoder(receiver)->valid()) {
        return fail_prepare(Error::INTERNAL);
    }
    source_prepared_[receiver] = true;
    capture_sync_errors_[receiver].store(0U);
    capture_empty_intervals_[receiver].store(0U);
    capture.link_seed.fill(0);
    return Result<void>::success();
}

CaptureRunResult LibusbW3u3Hardware::run_split_usb_pump(
    std::uint8_t receiver, const std::atomic<bool>& cancelled_flag,
    bool (*emit)(void*, const std::uint8_t*, std::size_t), void* context) noexcept
{
    if (receiver >= source_prepared_.size() || !source_prepared_[receiver] ||
        receiver >= async_states_.size() || emit == nullptr ||
        async_states_[receiver] == nullptr) return CaptureRunResult::usb_error;
    AsyncState& state = *async_states_[receiver];
    record_startup("worker_run");
    std::unique_ptr<SplitPump> pump(
        new (std::nothrow) SplitPump(*this, receiver, cancelled_flag, emit, context));
    CaptureRunResult outcome = CaptureRunResult::cancelled;
    bool discard = false;
    if (!pump) {
        std::fprintf(stderr, "asicen-split-usb-pump version=1 terminal=allocation_failed ingress_overflow=0\n");
        outcome = CaptureRunResult::usb_error;
    } else if (!pump->stopping() && !pump->consumer.start(SplitPump::entry, pump.get(),
                   capture_usb_hooks_ ? capture_usb_hooks_->launch_thread : nullptr)) {
        pump->terminal = "launch_failed";
        outcome = CaptureRunResult::usb_error;
    } else while (!pump->stopping() && !pump->done.load(std::memory_order_acquire)) {
        std::size_t index = kQueueDepth;
        int status = LIBUSB_TRANSFER_ERROR;
        int actual = 0;
        std::uint64_t submission = 0;
        bool invalid = false;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            invalid = state.drain_failed;
            for (std::size_t i = 0; !invalid && i < state.slots.size(); ++i) {
                const auto& slot = state.slots[i];
                if (slot.ready && slot.submission_order == state.next_delivery_order) {
                    index = i;
                    status = slot.status;
                    actual = slot.actual;
                    submission = slot.submission_order;
                    break;
                }
            }
        }
        if (invalid) {
            pump->terminal = "invalid_callback_or_length";
            outcome = CaptureRunResult::usb_error;
            break;
        }
        if (index == kQueueDepth) {
            const bool ok = wait_capture_events(receiver, 100U, cancelled_flag);
            if (!ok) {
                pump->terminal = "event_failure";
                outcome = disconnected_.load() ? CaptureRunResult::disconnected : CaptureRunResult::usb_error;
                break;
            }
            continue;
        }
        auto& slot = state.slots[index];
        if (status != LIBUSB_TRANSFER_COMPLETED && status != LIBUSB_TRANSFER_TIMED_OUT) {
            pump->terminal = "transfer_status";
            outcome = status == LIBUSB_TRANSFER_NO_DEVICE || disconnected_.load()
                ? CaptureRunResult::disconnected : CaptureRunResult::usb_error;
            break;
        }
        const bool has_bytes = status == LIBUSB_TRANSFER_COMPLETED && actual > 0;
        std::size_t copy = SplitPump::kIngressDepth;
        if (has_bytes) {
            copy = pump->reserve();
            if (copy == SplitPump::kIngressDepth) {
                // Latch a transport failure before waking/joining the consumer.
                // Leave this completion unretired for bounded stop/drain.
                pump->ingress_overflow = 1;
                pump->overflow_submission = submission;
                pump->terminal = "ingress_overflow";
                std::fprintf(stderr, "asicen-split-usb-pump terminal=ingress_overflow submission=%llu "
            "pool_blocks=32 block_bytes=65536"
            "\n",
                    static_cast<unsigned long long>(submission));
                outcome = CaptureRunResult::usb_error;
                discard = true;
                break;
            }
            auto& block = pump->blocks[copy];
            block.submission = submission;
            block.size = static_cast<std::size_t>(actual);
            // The sole producer has not resubmitted this completed generation.
            // Its buffer is therefore owned until copied and explicitly reused.
            std::memcpy(block.bytes.data(), slot.buffer.data(), block.size);
        }
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            slot.ready = false;
            ++state.next_delivery_order;
        }
        if (submission < kQueueDepth)
            record_startup("delivery", static_cast<std::int64_t>(submission), actual);
        if (has_bytes) {
            if (!pump->publish(copy)) break;
        } else capture_empty_intervals_[receiver].fetch_add(1U);

        // Publication and submission are solely on this original run caller.
        // No mutex is held across libusb: it can synchronously call completed().
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            if (state.drain_failed) {
                pump->terminal = "invalid_callback_or_length";
                outcome = CaptureRunResult::usb_error;
                break;
            }
            if (pump->stopping() || pump->done.load(std::memory_order_acquire)) break;
            slot.actual = 0;
            slot.pending = true;
            ++slot.generation;
            slot.submission_order = state.next_submission_order++;
        }
        if (submit_transfer(slot.transfer) != 0) {
            {
                std::lock_guard<std::mutex> lock(state.mutex);
                slot.pending = false;
            }
            pump->terminal = "submit_failure";
            outcome = disconnected_.load() ? CaptureRunResult::disconnected : CaptureRunResult::usb_error;
            break;
        }
    }
    if (pump) {
        // On transport failure let preceding copied input finish in FIFO order.
        // External cancellation and sink failure still stop between packets.
        pump->close(discard || outcome == CaptureRunResult::cancelled);
        pump->consumer.join(); // No state, queue, service or control lock held.
        if (outcome == CaptureRunResult::cancelled && pump->consumer_result != CaptureRunResult::cancelled)
            outcome = pump->consumer_result;
        {
            std::lock_guard<std::mutex> lock(state.mutex);
            if (state.split_completion_failed) {
                outcome = CaptureRunResult::usb_error;
                if (!pump->ingress_overflow) pump->terminal = "invalid_callback_or_length";
            }
            // These callbacks precede cancel_pending(), so terminal statuses
            // are real capture failures, not expected drain cancellation.
            for (const auto& slot : state.slots) {
                if (!slot.ready || slot.status == LIBUSB_TRANSFER_COMPLETED ||
                    slot.status == LIBUSB_TRANSFER_TIMED_OUT) continue;
                if (slot.status == LIBUSB_TRANSFER_NO_DEVICE) outcome = CaptureRunResult::disconnected;
                else if (outcome == CaptureRunResult::cancelled) outcome = CaptureRunResult::usb_error;
                if (!pump->ingress_overflow && !state.split_completion_failed)
                    pump->terminal = "transfer_status";
            }
        }
        pump->dump();
    }
    if (cleanup_failed_.load(std::memory_order_acquire)) outcome = CaptureRunResult::usb_error;
    // Consumer use has ended before transfer release, decoder clear,
    // bounded drain or callback-storage quarantine.
    const auto cleanup = stop_and_drain(receiver, receiver_capture_states_[receiver].dsc_attempted);
    if (cleanup == CaptureRunResult::fatal_drain) return cleanup;
    if (cleanup != CaptureRunResult::cancelled) {
        mark_capture_cleanup_failed(receiver, Error::USB_IO);
        return cleanup;
    }
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        // A callback can arrive during bounded stop/drain as well. Preserve its
        // invalid-observation result even though release clears drain_failed.
        if (state.split_completion_failed) outcome = CaptureRunResult::usb_error;
        if (state.split_callback_result == CaptureRunResult::disconnected)
            outcome = CaptureRunResult::disconnected;
        else if (state.split_callback_result == CaptureRunResult::usb_error &&
                 outcome == CaptureRunResult::cancelled) outcome = CaptureRunResult::usb_error;
    }
    source_prepared_[receiver] = false;
    return outcome;
}

CaptureRunResult LibusbW3u3Hardware::run(const std::atomic<bool>& cancelled_flag,
                                         bool (*emit)(void*, const std::uint8_t*, std::size_t),
                                         void* context) noexcept
{
    return run_split_usb_pump(source_receiver_, cancelled_flag, emit, context);
}

CaptureRunResult LibusbW3u3Hardware::run_for_receiver(
    std::uint8_t receiver, const std::atomic<bool>& cancelled_flag,
    bool (*emit)(void*, const std::uint8_t*, std::size_t), void* context) noexcept
{
    return run_split_usb_pump(receiver, cancelled_flag, emit, context);
}

void LibusbW3u3Hardware::interrupt() noexcept
{
    for (auto& interrupted : capture_interrupted_) {
        interrupted.store(true, std::memory_order_release);
    }
    if (capture_usb_hooks_ != nullptr && capture_usb_hooks_->interrupt_events != nullptr) {
        capture_usb_hooks_->interrupt_events(capture_usb_hooks_->context);
    } else if (context_ != nullptr) {
        libusb_interrupt_event_handler(context_);
    }
}

void LibusbW3u3Hardware::interrupt_receiver(std::uint8_t receiver) noexcept
{
    if (receiver < capture_interrupted_.size()) {
        capture_interrupted_[receiver].store(true, std::memory_order_release);
        if (capture_usb_hooks_ != nullptr && capture_usb_hooks_->interrupt_events != nullptr) {
            capture_usb_hooks_->interrupt_events(capture_usb_hooks_->context);
        } else if (context_ != nullptr) {
            libusb_interrupt_event_handler(context_);
        }
    }
}

px4::userland::TunerStreamCounters LibusbW3u3Hardware::source_counters() const noexcept
{
    px4::userland::TunerStreamCounters counters{};
    const auto receiver = counters_receiver_.load(std::memory_order_acquire);
    if (receiver < capture_sync_errors_.size()) {
        counters.sync_errors = capture_sync_errors_[receiver].load();
        counters.empty_intervals = capture_empty_intervals_[receiver].load();
        counters.availability[2U] = dtv::userland::wire::MeasureState::valid;
        counters.availability[7U] = dtv::userland::wire::MeasureState::valid;
    }
    return counters;
}

px4::userland::TunerStreamCounters
LibusbW3u3Hardware::source_counters_for_receiver(std::uint8_t receiver) const noexcept
{
    if (receiver >= capture_sync_errors_.size()) {
        return {};
    }
    px4::userland::TunerStreamCounters counters{};
    counters.sync_errors = capture_sync_errors_[receiver].load();
    counters.empty_intervals = capture_empty_intervals_[receiver].load();
    counters.availability[2U] = dtv::userland::wire::MeasureState::valid;
    counters.availability[7U] = dtv::userland::wire::MeasureState::valid;
    return counters;
}

StreamCaptureFramingCounters LibusbW3u3Hardware::source_framing_counters() const noexcept
{
    const auto receiver = counters_receiver_.load(std::memory_order_acquire);
    const auto* decoder = capture_decoder(receiver);
    return decoder != nullptr ? decoder->counters() : StreamCaptureFramingCounters{};
}

Result<void> LibusbW3u3Hardware::stop() noexcept
{
    if (abandoned_) {
        return Result<void>::failure(Error::USB_IO);
    }
    if (!claimed_) {
        return Result<void>::failure(Error::NOT_READY);
    }
    Error first_error = Error::OK;
    for (std::uint8_t receiver = 0U; receiver < receiver_count(); ++receiver) {
        const auto stopped = stop_source_receiver(receiver);
        if (!stopped && first_error == Error::OK) {
            first_error = stopped.error();
        }
    }
    if (!cleanup_failed_.load()) {
        source_receiver_ = kNoActiveReceiver;
        routed_source_receiver_.store(kNoActiveReceiver, std::memory_order_release);
    }
    if (!cleanup_failed_.load() && first_error == Error::OK) {
        return Result<void>::success();
    }
    const int stored = cleanup_failure_error_.load(std::memory_order_acquire);
    return Result<void>::failure(stored == 0 ? first_error : static_cast<Error>(stored));
}

Result<void> LibusbW3u3Hardware::stop_receiver(std::uint8_t receiver) noexcept
{
    return stop_source_receiver(receiver);
}

Result<void> LibusbW3u3Hardware::stop_source_receiver(std::uint8_t receiver) noexcept
{
    if (!runtime_model_supported() || receiver >= receiver_count() ||
        receiver >= source_prepared_.size()) {
        return Result<void>::failure(Error::UNSUPPORTED);
    }
    if (!claimed_) {
        return Result<void>::failure(Error::NOT_READY);
    }
    const auto& capture = receiver_capture_states_[receiver];
    const AsyncState* const async = capture_async(receiver);
    if (source_prepared_[receiver] || (async != nullptr && async->prepared) ||
        capture.cf_snapshot_valid || capture.link_snapshot_valid) {
        const auto result = stop_and_drain(receiver, capture.dsc_attempted);
        if (result == CaptureRunResult::fatal_drain) {
            if (startup_config_.enabled) {
                std::fprintf(stderr,
                             "asicen-capture-stop receiver=%u drain_result=%u "
                             "returned_error=%u cleanup_failed=%u\n",
                             static_cast<unsigned>(receiver), static_cast<unsigned>(result),
                             static_cast<unsigned>(Error::USB_IO),
                             cleanup_failed_.load() ? 1U : 0U);
            }
            return Result<void>::failure(Error::USB_IO);
        }
        if (result != CaptureRunResult::cancelled) {
            mark_capture_cleanup_failed(receiver, Error::USB_IO);
            if (startup_config_.enabled) {
                std::fprintf(stderr,
                             "asicen-capture-stop receiver=%u drain_result=%u "
                             "returned_error=%u cleanup_failed=%u\n",
                             static_cast<unsigned>(receiver), static_cast<unsigned>(result),
                             static_cast<unsigned>(Error::USB_IO),
                             cleanup_failed_.load() ? 1U : 0U);
            }
            return Result<void>::failure(Error::USB_IO);
        }
        source_prepared_[receiver] = false;
    }
    const bool failed = cleanup_failed_.load();
    if (startup_config_.enabled) {
        std::fprintf(stderr,
                     "asicen-capture-stop receiver=%u drain_result=not_needed "
                     "returned_error=%u cleanup_failed=%u\n",
                     static_cast<unsigned>(receiver),
                     static_cast<unsigned>(failed ? Error::USB_IO : Error::OK),
                     failed ? 1U : 0U);
    }
    return failed ? Result<void>::failure(Error::USB_IO) : Result<void>::success();
}

CaptureRunResult LibusbW3u3Hardware::stop_and_drain(std::uint8_t receiver,
                                                   bool dsc_was_attempted) noexcept
{
    if (abandoned_) {
        return CaptureRunResult::fatal_drain;
    }
    if (auto* const async = capture_async(receiver); async != nullptr) {
        std::lock_guard<std::mutex> lock(async->mutex);
        async->split_draining = true;
    }
    // A card transaction owns the bridge until its bounded operation ends.
    // Hold the same gate across DSC stop, callback drain and restoration so
    // no controller operation can enter between cleanup stages. Event/callback
    // handling never takes this gate.
    // Like PX4's command event gate, join the bounded control operation before
    // beginning cancellation. Never abandon an owner that still uses `this`.
    std::unique_lock<std::recursive_timed_mutex> gate(control_gate_);
    ReceiverRouteScope route_scope(cleanup_route_receiver_, receiver);
    AtomicFlagScope cleanup_scope(cleanup_io_active_);
    DrainAdapter ops(*this, receiver);
    const CaptureRunResult result =
        drain_capture_callbacks(ops, dsc_was_attempted,
                                receiver_capture_states_[receiver].dsc_stopped, kDrainLimit);
    if (result == CaptureRunResult::fatal_drain) {
        quarantine_capture();
    }
    if (result != CaptureRunResult::cancelled) {
        mark_capture_cleanup_failed(receiver, Error::USB_IO);
    }
    record_startup("drain_complete", static_cast<int>(result));
    flush_startup_trace();
    return result;
}


void LibusbW3u3Hardware::record_startup(const char* stage, std::int64_t first,
                                        std::int64_t second, std::int64_t third,
                                        std::int64_t fourth, std::int64_t fifth) noexcept
{
    if (!startup_trace_active_.load()) return;
    std::lock_guard<std::mutex> lock(startup_trace_mutex_);
    if (!startup_trace_active_.load()) return;
    // Keep capacity for capture/drain milestones after a long lock poll.
    const bool repetitive = std::strcmp(stage, "pre_gain_lock") == 0 ||
                            std::strcmp(stage, "post_gain_lock") == 0 ||
                            (std::strcmp(stage, "readiness") == 0 && first == 0);
    if (repetitive && startup_event_count_ >= startup_events_.size() - 64U) {
        ++startup_events_dropped_;
        return;
    }
    if (startup_event_count_ == startup_events_.size()) {
        ++startup_events_dropped_;
        return;
    }
    startup_events_[startup_event_count_++] = {stage, steady_now(), first, second, third,
                                               fourth, fifth};
}

void LibusbW3u3Hardware::flush_startup_trace() noexcept
{
    if (!startup_config_.enabled) return;
    std::lock_guard<std::mutex> lock(startup_trace_mutex_);
    startup_trace_active_.store(false);
    if (startup_event_count_ == 0U) return;
    const auto origin = startup_events_[0].at;
    std::fprintf(stderr, "asicen-startup-diagnostic stable_ms=%u events=%zu dropped=%zu\n",
                 startup_config_.stable_ms, startup_event_count_, startup_events_dropped_);
    for (std::size_t i = 0; i < startup_event_count_; ++i) {
        const auto& event = startup_events_[i];
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                            event.at - origin).count();
        std::fprintf(stderr, "asicen-startup-diagnostic us=%lld stage=%s first=%lld second=%lld third=%lld fourth=%lld fifth=%lld\n",
                     static_cast<long long>(us), event.stage,
                     static_cast<long long>(event.first),
                     static_cast<long long>(event.second),
                     static_cast<long long>(event.third),
                     static_cast<long long>(event.fourth),
                     static_cast<long long>(event.fifth));
    }
    startup_event_count_ = 0U;
    startup_events_dropped_ = 0U;
}

void LibusbW3u3Hardware::reset_startup_diagnostic(std::uint8_t receiver) noexcept
{
    flush_startup_trace();
    if (receiver < receiver_tune_states_.size()) {
        receiver_tune_states_[receiver].readiness.reset();
    }
}

void LibusbW3u3Hardware::quarantine_capture() noexcept
{
    // AsyncState is self-contained: callbacks retain only Slot/AsyncState,
    // transfer, buffer and libusb handle/context state, never this backend.
    // Match PX4's bounded-drain quarantine rather than terminating the daemon.
    mark_cleanup_failed(Error::USB_IO);
    abandoned_ = true;
    quarantine_libusb_context(context_);
    for (auto& async : async_states_) {
        if (async != nullptr && async->prepared) {
            async = nullptr;
        }
    }
    ownership_.abandon();
    primary_.abandon();
    sibling_.abandon();
    source_prepared_.fill(false);
    claimed_ = false;
}

CaptureRunResult LibusbW3u3Hardware::cleanup_after_drain(std::uint8_t receiver, bool dsc_ok,
                                                         bool dsc_was_attempted) noexcept
{
    const auto gate_deadline = steady_now() + std::chrono::seconds(5);
    if (!acquire_control_gate(gate_deadline, nullptr, true)) {
        mark_cleanup_failed(Error::TIMEOUT);
        return CaptureRunResult::usb_error;
    }
    std::unique_lock<std::recursive_timed_mutex> gate(control_gate_, std::adopt_lock);
    AtomicFlagScope cleanup_scope(cleanup_io_active_);
    auto& capture = receiver_capture_states_[receiver];
    DrainAdapter ops(*this, receiver);
    const CaptureRunResult result =
        cleanup_capture_state(ops, dsc_ok, dsc_was_attempted, capture.link_apply_attempted,
                              capture.output_start_attempted, capture.cf_snapshot_valid);
    const bool cleaned = result == CaptureRunResult::cancelled;
    if (auto* const decoder = capture_decoder(receiver); decoder != nullptr) {
        decoder->clear();
    }
    capture.link_seed.fill(0U);
    if (cleaned || disconnected_.load()) {
        capture.link_snapshot_valid = false;
        capture.cf_snapshot_valid = false;
        capture.link_apply_attempted = false;
        capture.output_start_attempted = false;
        capture.dsc_attempted = false;
        capture.dsc_stopped = true;
        if (source_receiver_ == receiver) {
            source_receiver_ = kNoActiveReceiver;
            for (std::uint8_t active = 0U; active < receiver_count(); ++active) {
                if (source_prepared_[active] ||
                    receiver_capture_states_[active].link_snapshot_valid ||
                    receiver_capture_states_[active].cf_snapshot_valid) {
                    source_receiver_ = active;
                    break;
                }
            }
            routed_source_receiver_.store(source_receiver_, std::memory_order_release);
        }
    } else {
        mark_capture_cleanup_failed(receiver, Error::USB_IO);
    }
    return result;
}

bool LibusbW3u3Hardware::wait_capture_events(std::uint8_t receiver, unsigned timeout_ms,
                                            const std::atomic<bool>& cancelled_flag) noexcept
{
    // Preserve existing injected event pumps. Real libusb callbacks may also
    // run in a synchronous control caller, between run's scan and this wait.
    if (capture_usb_hooks_ != nullptr && capture_usb_hooks_->pump_events != nullptr)
        return handle_events(timeout_ms);
    AsyncState* const async = capture_async(receiver);
    if (context_ == nullptr || async == nullptr) return false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    const auto should_return = [&]() noexcept {
        if (cancelled_flag.load() || stop_requested_.load() || receiver >= capture_interrupted_.size() ||
            capture_interrupted_[receiver].load() ||
            cleanup_failed_.load(std::memory_order_acquire)) return true;
        std::lock_guard<std::mutex> lock(async->mutex);
        if (async->drain_failed) return true;
        for (const auto& slot : async->slots)
            if (slot.ready && slot.submission_order == async->next_delivery_order) return true;
        return false;
    };
    const auto remaining = [&]() noexcept {
        const auto us = std::max<std::int64_t>(0, std::chrono::duration_cast<std::chrono::microseconds>(
            deadline - std::chrono::steady_clock::now()).count());
        timeval tv{};
        tv.tv_sec = static_cast<long>(us / 1000000);
        tv.tv_usec = static_cast<long>(us % 1000000);
        return tv;
    };
    int rc = 0;
    for (;;) {
        if (libusb_try_lock_events(context_) == 0) {
            // Lock order matches completion: libusb events -> AsyncState.
            // Never hold AsyncState while acquiring a libusb lock or polling.
            if (should_return()) {
                libusb_unlock_events(context_);
                return true;
            }
            if (!libusb_event_handling_ok(context_)) {
                libusb_unlock_events(context_);
                if (std::chrono::steady_clock::now() >= deadline) return true;
                continue;
            }
            auto timeout = remaining();
            rc = libusb_handle_events_locked(context_, &timeout);
            libusb_unlock_events(context_);
            break;
        }
        libusb_lock_event_waiters(context_);
        if (should_return()) {
            libusb_unlock_event_waiters(context_);
            return true;
        }
        if (!libusb_event_handler_active(context_)) {
            libusb_unlock_event_waiters(context_);
            if (std::chrono::steady_clock::now() >= deadline) return true;
            continue;
        }
        auto timeout = remaining();
        rc = libusb_wait_for_event(context_, &timeout);
        libusb_unlock_event_waiters(context_);
        // A timeout or spurious/other-submission wake returns to run's scan.
        break;
    }
    if (rc == LIBUSB_ERROR_NO_DEVICE) disconnected_.store(true);
    return rc >= 0;
}

bool LibusbW3u3Hardware::handle_events(unsigned timeout_ms) noexcept
{
    if (capture_usb_hooks_ != nullptr && capture_usb_hooks_->pump_events != nullptr) {
        const int rc = capture_usb_hooks_->pump_events(capture_usb_hooks_->context, timeout_ms);
        if (rc == LIBUSB_ERROR_NO_DEVICE) {
            disconnected_.store(true);
        }
        return rc >= 0;
    }
    if (context_ == nullptr) {
        return false;
    }
    timeval timeout{};
    timeout.tv_sec = static_cast<long>(timeout_ms / 1000U);
    timeout.tv_usec = static_cast<long>((timeout_ms % 1000U) * 1000U);
    const int rc = libusb_handle_events_timeout_completed(context_, &timeout, nullptr);
    if (rc == LIBUSB_ERROR_NO_DEVICE) {
        disconnected_.store(true);
    }
    return rc >= 0;
}

libusb_transfer* LibusbW3u3Hardware::allocate_transfer() noexcept
{
    return capture_usb_hooks_ != nullptr && capture_usb_hooks_->allocate != nullptr
               ? capture_usb_hooks_->allocate(capture_usb_hooks_->context)
               : libusb_alloc_transfer(0);
}

int LibusbW3u3Hardware::submit_transfer(libusb_transfer* transfer) noexcept
{
    const int result = capture_usb_hooks_ != nullptr && capture_usb_hooks_->submit != nullptr
                           ? capture_usb_hooks_->submit(capture_usb_hooks_->context, transfer)
                           : libusb_submit_transfer(transfer);
    if (result < 0) {
        int expected = 0;
        (void)first_transport_error_.compare_exchange_strong(expected, result);
        if (result == LIBUSB_ERROR_NO_DEVICE) {
            disconnected_.store(true);
        }
    }
    return result;
}

int LibusbW3u3Hardware::cancel_transfer(libusb_transfer* transfer) noexcept
{
    return capture_usb_hooks_ != nullptr && capture_usb_hooks_->cancel != nullptr
               ? capture_usb_hooks_->cancel(capture_usb_hooks_->context, transfer)
               : libusb_cancel_transfer(transfer);
}

void LibusbW3u3Hardware::free_transfer(libusb_transfer* transfer) noexcept
{
    if (capture_usb_hooks_ != nullptr && capture_usb_hooks_->free != nullptr) {
        capture_usb_hooks_->free(capture_usb_hooks_->context, transfer);
    } else {
        libusb_free_transfer(transfer);
    }
}

bool LibusbW3u3Hardware::dsc_start(std::uint8_t local)
{
    const auto transfer = make_dsc_control(local, false, 1000U);
    std::array<unsigned char, 1> response{};
    return control(transfer, response.data()) == transfer.length && response[0] == 1U;
}
bool LibusbW3u3Hardware::dsc_stop(std::uint8_t local)
{
    const auto transfer = make_dsc_control(local, true, 1000U);
    std::array<unsigned char, 1> response{};
    return control(transfer, response.data()) == transfer.length && response[0] == 1U;
}
CaptureIo LibusbW3u3Hardware::bulk_read(std::uint8_t, unsigned char*, int, int*, unsigned)
{
    return CaptureIo::Error;
}

bool LibusbW3u3Hardware::read_cf40(std::uint8_t local, std::uint8_t* value)
{
    if (value == nullptr || local > 1U) {
        return false;
    }
    const auto transfer = make_cf_read(local, 0x40U, 1U, 1000U);
    std::array<unsigned char, 2> response{};
    return control(transfer, response.data()) == transfer.length && ((*value = response[1]), true);
}
bool LibusbW3u3Hardware::write_cf40(std::uint8_t local, std::uint8_t value)
{
    ControlTransfer transfer{};
    if (!make_cf_write(local, 0x40U, &value, 1U, &transfer, 1000U)) {
        return false;
    }
    std::array<unsigned char, 2> response{};
    return control(transfer, response.data()) == transfer.length;
}
bool LibusbW3u3Hardware::read_cf_block(std::uint8_t local, std::uint8_t* data, std::size_t size)
{
    if (data == nullptr || local > 1U || size != 0x45U) {
        return false;
    }
    for (std::size_t offset = 0; offset < size;) {
        const std::size_t chunk = std::min<std::size_t>(0x20U, size - offset);
        const auto transfer = make_cf_read(local, static_cast<std::uint8_t>(offset),
                                           static_cast<std::uint16_t>(chunk), 1000U);
        std::array<unsigned char, 0x21> response{};
        if (control(transfer, response.data()) != transfer.length) {
            return false;
        }
        std::copy_n(response.begin() + 1, chunk, data + offset);
        offset += chunk;
    }
    return true;
}
bool LibusbW3u3Hardware::write_cf_block(std::uint8_t local, const std::uint8_t* data,
                                        std::size_t size)
{
    if (data == nullptr || local > 1U || size != 0x45U) {
        return false;
    }
    for (std::size_t offset = 0; offset < size;) {
        const std::size_t chunk = std::min<std::size_t>(3U, size - offset);
        ControlTransfer transfer{};
        if (!make_cf_write(local, static_cast<std::uint8_t>(offset), data + offset, chunk,
                           &transfer, 1000U)) {
            return false;
        }
        std::array<unsigned char, 4> response{};
        if (control(transfer, response.data()) != transfer.length) {
            return false;
        }
        offset += chunk;
    }
    return true;
}
bool LibusbW3u3Hardware::terrestrial_locked(std::uint8_t local, bool* locked,
                                            std::chrono::steady_clock::time_point deadline)
{
    const std::uint8_t receiver = current_receiver();
    if (!receiver_supports(local, System::ISDB_T) || locked == nullptr ||
        receiver >= receiver_tune_states_.size() || steady_now() >= deadline) {
        return false;
    }
    if (uses_v2_frontend()) {
        return read_v2_frontend_lock(this, frontend_target(current_receiver()), locked,
                                     nullptr, 500U) == V2FrontendResult::Completed;
    }
    FrontendRunReport report{};
    const auto plan = uses_legacy_frontend()
                          ? plan_legacy_terrestrial_lock(
                                legacy_frontend(), receiver_tune_states_[receiver].frequency_khz)
                          : plan_terrestrial_lock_read(
                                receiver_tune_states_[receiver].frequency_khz);
    if (!run_plan(plan, 500U, &report, receiver) || !report.have_last_read) {
        return false;
    }
    *locked = (report.last_read & 0x0fU) == 0x09U;
    return true;
}
bool LibusbW3u3Hardware::set_cf_bit(std::uint8_t local, std::uint8_t mask, bool value) noexcept
{
    std::uint8_t current = 0;
    if (!read_cf40(local, &current)) {
        return false;
    }
    const std::uint8_t next = value ? static_cast<std::uint8_t>(current | mask)
                                    : static_cast<std::uint8_t>(current & ~mask);
    return next == current || write_cf40(local, next);
}
bool LibusbW3u3Hardware::snapshot_link_diagnostic()
{
    const auto receiver = current_receiver();
    auto* const coordinator = capture_coordinator(receiver);
    if (coordinator == nullptr) return false;
    if (!verify_device_revision()) {
        record_startup("capture_snapshot_revision_failed", receiver);
        return false;
    }
    std::uint8_t type_reg = 0;
    if (!read_i2c(0x4aU, 0x09U, 1U, &type_reg)) {
        record_startup("capture_snapshot_type_read_failed", receiver);
        return false;
    }
    if (!powered_controller_type_supported(profile_, type_reg)) {
        record_startup("capture_snapshot_type_rejected", receiver);
        return false;
    }
    if (!coordinator->reserve(this, receiver)) {
        record_startup("capture_snapshot_idle_rejected", receiver,
                       coordinator->quarantined()
                           ? static_cast<int>(LinkSeedSnapshotResult::unavailable)
                           : static_cast<int>(LinkSeedSnapshotResult::controller_active));
        return false;
    }
    receiver_capture_states_[receiver].link_apply_attempted = true;
    record_startup("capture_snapshot_complete", receiver);
    return true;
}
bool LibusbW3u3Hardware::verify_device_revision() noexcept
{
    ControlTransfer rev{0, Request::SysCtrlRead, 2, 0, 3, Direction::In, 1000};
    std::array<unsigned char, 3> response{};
    return control(rev, response.data()) == rev.length && response[0] == 1U &&
           response[1] == 0x11U && response[2] == 0x52U;
}
LibusbW3u3Hardware::PoweredControllerCheck LibusbW3u3Hardware::verify_powered_controller() noexcept
{
    std::uint8_t type = 0xffU;
    std::uint8_t state = 0xffU;
    if (!read_i2c(0x4aU, 0x09U, 1U, &type)) {
        return PoweredControllerCheck::type_read_failed;
    }
    const char* expected = powered_controller_type_supported(profile_, 0x20U)
                               ? "0x0f or 0x10 (PX-W3U2)"
                               : "0x0f";
    std::fprintf(stderr,
                 "asicend: powered-controller model=%s i2c=4a reg=09 raw=0x%02x decoded=0x%02x expected=%s\n",
                 profile_ != nullptr ? profile_->model : "unknown",
                 static_cast<unsigned>(type),
                 static_cast<unsigned>((type & 0x3eU) >> 1U), expected);
    if (!powered_controller_type_supported(profile_, type)) {
        return PoweredControllerCheck::unsupported_type;
    }
    if (!read_controller05(&state)) {
        return PoweredControllerCheck::output_state_read_failed;
    }
    return state == 0U ? PoweredControllerCheck::ready : PoweredControllerCheck::output_busy;
}
bool LibusbW3u3Hardware::apply_link_seed()
{
    const auto receiver = current_receiver();
    auto* const coordinator = capture_coordinator(receiver);
    if (coordinator == nullptr || receiver >= receiver_capture_states_.size()) return false;
    auto& capture = receiver_capture_states_[receiver];
    if (!coordinator->apply(this, receiver, capture.link_seed.data(), capture.link_seed.size()))
        return false;
    capture.link_seed = coordinator->seed();
    return true;
}
bool LibusbW3u3Hardware::clear_link_seed_and_verify_controller()
{
    const auto receiver = current_receiver();
    auto* const coordinator = capture_coordinator(receiver);
    if (coordinator == nullptr) return false;
    return coordinator->release(this, receiver) != LinkSeedLaneRelease::cleanup_failed;
}
LinkSeedCaptureCoordinator* LibusbW3u3Hardware::capture_coordinator(
    std::uint8_t receiver) noexcept
{
    if (profile_ == nullptr) return nullptr;
    const auto route = receiver_address(*profile_, receiver);
    if (!route || route.value().rf_function_index >= function_capture_coordinators_.size())
        return nullptr;
    return &function_capture_coordinators_[route.value().rf_function_index];
}
bool LibusbW3u3Hardware::read_controller05(std::uint8_t* value)
{
    std::uint8_t local = 0;
    if (value == nullptr) {
        value = &local;
    }
    return read_i2c(0x4aU, 0x05U, 1U, value);
}
bool LibusbW3u3Hardware::read_controller05_value_zero()
{
    std::uint8_t value = 0xff;
    return read_controller05(&value) && value == 0U;
}
bool LibusbW3u3Hardware::write_controller05(std::uint8_t value)
{
    return write_i2c_byte(0x4aU, 0x05U, value);
}
bool LibusbW3u3Hardware::write_link_seed_byte(std::uint8_t reg, std::uint8_t value)
{
    return reg >= 0x10U && reg <= 0x1fU && write_i2c_byte(0x4aU, reg, value);
}
bool LibusbW3u3Hardware::read_i2c(std::uint8_t slave, std::uint8_t reg, std::uint16_t length,
                                  std::uint8_t* output) noexcept
{
    if (output == nullptr || length == 0U || length > 0x20U) {
        return false;
    }
    const auto transfer = make_i2c_read(slave, reg, length, 0U, 1000U);
    std::array<unsigned char, 0x21> response{};
    const int rc = control(transfer, response.data());
    if (rc != transfer.length ||
        !parse_status_response(response.data(), static_cast<std::size_t>(rc), nullptr, 0)) {
        return false;
    }
    std::copy_n(response.begin() + 1, length, output);
    return true;
}
bool LibusbW3u3Hardware::write_i2c_byte(std::uint8_t slave, std::uint8_t reg,
                                        std::uint8_t value) noexcept
{
    ControlTransfer transfer{};
    if (!make_i2c_write_chunk(slave, reg, &value, 1U, false, &transfer, 1000U)) {
        return false;
    }
    std::array<unsigned char, 2> response{};
    const int rc = control(transfer, response.data());
    return rc == transfer.length &&
           parse_status_response(response.data(), static_cast<std::size_t>(rc), nullptr, 0);
}

}  // namespace asicen
