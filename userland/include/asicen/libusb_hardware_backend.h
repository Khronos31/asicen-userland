// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "asicen/hardware_ownership.h"
#include "asicen/hardware_stream_session.h"
#include "asicen/libusb_transport.h"
#include "asicen/capture_drain.h"
#include "asicen/link_seed_diagnostic.h"
#include "asicen/transport_capture.h"
#include "asicen/stream_capture.h"
#include "asicen/frontend_sequence.h"
#include "asicen/satellite_tune.h"
#include "asicen/card_mailbox_hardware.h"
#include "asicen/card_operation_guard.h"

#include <atomic>
#include <array>
#include <chrono>
#include <csignal>
#include <memory>
#include <mutex>

namespace asicen {

struct SatelliteProbeSummary {
    SatelliteOperationResult result = SatelliteOperationResult::InvalidArgument;
    bool locked = false;
    std::uint8_t nonempty_tsid_slots = 0;
    bool selected_slot = false;
};

struct CardProbeSummary {
    px4::userland::Error error = px4::userland::Error::INTERNAL;
    bool atr_valid = false;
    std::size_t atr_length = 0;
    std::size_t response_length = 0;
    std::uint16_t status_word = 0;
};

// In-process backend: primary receiver 0 is satellite and receiver 1 is
// terrestrial. Both USB functions are claimed before frontend writes; the
// shared frontend permits one active receiver lease/capture at a time.
class LibusbW3u3Hardware final : public px4::userland::TunerServiceBackend,
                                 public StreamCaptureSource,
                                 public CardOperationGuard,
                                 private HardwareShutdownOps,
                                 private FrontendTransport,
                                 private CaptureBackend,
                                 private LinkSeedDiagnosticIo {
public:
    LibusbW3u3Hardware(libusb_context* context, UsbLocation primary,
                       UsbLocation sibling, std::vector<std::uint8_t> primary_path,
                       std::vector<std::uint8_t> sibling_path);
    ~LibusbW3u3Hardware() noexcept override;
    LibusbW3u3Hardware(const LibusbW3u3Hardware&) = delete;
    LibusbW3u3Hardware& operator=(const LibusbW3u3Hardware&) = delete;

    px4::userland::Result<void> claim();
    px4::userland::Result<void> release() noexcept;
    SatelliteProbeSummary probe_satellite(
        std::uint32_t rf_khz, bool select_slot, std::size_t slot,
        const volatile std::sig_atomic_t* stop_flag) noexcept;
    CardProbeSummary probe_card(
        const volatile std::sig_atomic_t* stop_flag) noexcept;

    std::uint8_t receiver_count() const noexcept override;
    bool receiver_supports(std::uint8_t, px4::userland::ipc::System) const noexcept override;
    bool selects_satellite_stream_before_tune() const noexcept override;
    bool requires_terrestrial_lock_settle() const noexcept override;
    px4::userland::Result<void> open_receiver(std::uint8_t) noexcept override;
    px4::userland::Result<void> tune_terrestrial(std::uint8_t, std::uint32_t,
                                                 std::uint32_t) noexcept override;
    px4::userland::Result<void> tune_satellite(std::uint8_t, std::uint32_t,
                                               std::uint32_t) noexcept override;
    px4::userland::Result<bool> is_locked(std::uint8_t,
                                         px4::userland::ipc::System) noexcept override;
    px4::userland::Result<void> select_satellite_slot(std::uint8_t, std::uint8_t,
                                                      std::uint32_t) noexcept override;
    px4::userland::Result<void> select_satellite_tsid(std::uint8_t, std::uint16_t,
                                                      std::uint32_t) noexcept override;
    px4::userland::Result<void> close_receiver(std::uint8_t) noexcept override;
    px4::userland::Result<void> begin_tune_power(std::uint8_t,
                                                px4::userland::ipc::System,
                                                std::uint8_t) noexcept override;
    px4::userland::Result<void> commit_tune_power(std::uint8_t) noexcept override;
    px4::userland::Result<void> rollback_tune_power(std::uint8_t) noexcept override;
    void mark_receiver_disconnected(std::uint8_t) noexcept override;
    void request_stop() noexcept override;
    void request_card_stop() noexcept override { request_stop(); }
    px4::userland::Result<void> shutdown() noexcept override;

    px4::userland::Result<void> prepare(
        std::uint8_t, px4::userland::ipc::System,
        const std::atomic<bool>& cancelled) noexcept override;
    CaptureRunResult run(const std::atomic<bool>& cancelled,
                         bool (*emit)(void*, const std::uint8_t*, std::size_t),
                         void* context) noexcept override;
    void interrupt() noexcept override;
    px4::userland::Result<void> stop() noexcept override;

private:
    friend struct LibusbW3u3HardwareTestPeer;

    friend int run_card_only_server(
        LibusbW3u3Hardware&, const char*, const char*,
        const volatile std::sig_atomic_t*) noexcept;
    bool begin_card_operation(
        std::uint32_t timeout_ms,
        const volatile std::sig_atomic_t* stop_flag) noexcept override;
    void end_card_operation() noexcept override;
    bool begin_card_cleanup(std::uint32_t timeout_ms) noexcept override;
    void end_card_cleanup(bool cleanup_succeeded) noexcept override;
    enum class PoweredControllerCheck : std::uint8_t {
        ready,
        type_read_failed,
        unsupported_type,
        output_state_read_failed,
        output_busy,
    };

    int control(const ControlTransfer&, unsigned char*) override;
    struct CaptureUsbHooks {
        void* context = nullptr;
        int (*control)(void*, const ControlTransfer&, unsigned char*) = nullptr;
        libusb_transfer* (*allocate)(void*) = nullptr;
        int (*submit)(void*, libusb_transfer*) = nullptr;
        int (*cancel)(void*, libusb_transfer*) = nullptr;
        void (*free)(void*, libusb_transfer*) = nullptr;
        int (*pump_events)(void*, unsigned) = nullptr;
    };
    libusb_transfer* allocate_transfer() noexcept;
    int submit_transfer(libusb_transfer*) noexcept;
    int cancel_transfer(libusb_transfer*) noexcept;
    void free_transfer(libusb_transfer*) noexcept;
    void delay_ms(unsigned) override;
    bool cancelled() const override;
    bool expired() const override;
    bool dsc_start(std::uint8_t) override;
    bool dsc_stop(std::uint8_t) override;
    CaptureIo bulk_read(std::uint8_t, unsigned char*, int, int*, unsigned) override;
    bool read_cf40(std::uint8_t, std::uint8_t*) override;
    bool write_cf40(std::uint8_t, std::uint8_t) override;
    bool read_cf_block(std::uint8_t, std::uint8_t*, std::size_t) override;
    bool write_cf_block(std::uint8_t, const std::uint8_t*, std::size_t) override;
    bool terrestrial_locked(std::uint8_t, bool*, std::chrono::steady_clock::time_point) override;
    bool snapshot_link_diagnostic() override;
    bool verify_device_revision() noexcept;
    PoweredControllerCheck verify_powered_controller() noexcept;
    bool apply_link_seed() override;
    bool clear_link_seed_and_verify_controller() override;
    bool read_controller05(std::uint8_t*) override;
    bool read_controller05_value_zero();
    bool write_controller05(std::uint8_t) override;
    bool write_link_seed_byte(std::uint8_t, std::uint8_t) override;

    bool run_plan(const FrontendPlan&, unsigned timeout_ms = 3000U,
                  FrontendRunReport* report = nullptr) noexcept;
    bool read_i2c(std::uint8_t slave, std::uint8_t reg, std::uint16_t length,
                  std::uint8_t* output) noexcept;
    bool write_i2c_byte(std::uint8_t slave, std::uint8_t reg,
                        std::uint8_t value) noexcept;
    bool set_cf_bit(std::uint8_t local, std::uint8_t mask, bool value) noexcept;
    CaptureRunResult stop_and_drain(bool dsc_was_attempted) noexcept;
    CaptureRunResult cleanup_after_drain(bool dsc_stopped,
                                         bool dsc_attempted) noexcept;
    bool handle_events(unsigned timeout_ms) noexcept;
    bool stop_capture_safely() noexcept override;
    bool restore_gpio_snapshot_safely() noexcept override;

    libusb_context* context_ = nullptr;
    LibusbDevice primary_;
    LibusbDevice sibling_;
    LibusbFunctionClaim primary_claim_;
    LibusbFunctionClaim sibling_claim_;
    EnclosureOwnership ownership_;
    std::vector<std::uint8_t> primary_path_;
    std::vector<std::uint8_t> sibling_path_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> disconnected_{false};
    std::atomic<bool> capture_interrupted_{false};
    bool card_cleanup_active_ = false;
    std::chrono::steady_clock::time_point deadline_{};
    bool deadline_active_ = false;
    bool claimed_ = false;
    bool initialized_ = false;
    bool tuned_ = false;
    bool gain_applied_ = false;
    bool source_prepared_ = false;
    bool dsc_attempted_ = false;
    bool dsc_stopped_ = true;
    bool cf_snapshot_valid_ = false;
    bool link_snapshot_valid_ = false;
    bool link_apply_attempted_ = false;
    bool output_start_attempted_ = false;
    bool cleanup_failed_ = false;
    static constexpr std::uint8_t kNoActiveReceiver = 0xffU;
    ReceiverLaneReservation active_receiver_{};
    std::uint8_t tuned_receiver_ = kNoActiveReceiver;
    std::uint8_t source_receiver_ = kNoActiveReceiver;
    px4::userland::ipc::System tuned_system_ = px4::userland::ipc::System::ISDB_T;
    const volatile std::sig_atomic_t* diagnostic_stop_flag_ = nullptr;
    bool gpio_snapshot_valid_ = false;
    std::uint8_t gpio_snapshot_ = 0;
    std::array<std::uint8_t, 0x45> cf_snapshot_{};
    std::array<std::uint8_t, 16> link_seed_{};
    LinkSeedDiagnostic link_diagnostic_{};
    struct AsyncState;
    struct DrainAdapter;
    std::unique_ptr<AsyncState> async_;
    TransportCaptureDecoderV7 decoder_;
    const CaptureUsbHooks* capture_usb_hooks_ = nullptr;
};

}  // namespace asicen
