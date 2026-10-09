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
#include "asicen/v2_frontend.h"
#include "asicen/card_mailbox_hardware.h"
#include "asicen/card_operation_guard.h"

#include <atomic>
#include <array>
#include <chrono>
#include <csignal>
#include <memory>
#include <mutex>
#include <thread>

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

// Source-gated in-process backend. S3U exposes one combined receiver; other
// supported models expose primary satellite0/terrestrial1. W3U2/W3U3 share
// their frontend path, while S3U/S3U2/V2 use model-specific plans. Physical
// enclosure capacity is in device_profile(); receiver_count() reports only
// operational lanes. A paired model claims both functions before writes, but
// the secondary function stays reserved and never receives frontend I/O.
// The shared frontend permits one active receiver lease/capture at a time.
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
                       std::vector<std::uint8_t> sibling_path,
                       const DeviceProfile* expected_profile = nullptr);
    // Android/Termux entry point. primary_fd is required and sibling_fd is -1
    // for single-function models. The caller's fds are never closed; the
    // LibusbDevice duplicates them for the wrapped handle's lifetime. A fd
    // count that disagrees with the descriptor-resolved profile fails claim().
    LibusbW3u3Hardware(libusb_context* context, int primary_fd, int sibling_fd,
                       const DeviceProfile* expected_profile = nullptr);
    ~LibusbW3u3Hardware() noexcept override;
    LibusbW3u3Hardware(const LibusbW3u3Hardware&) = delete;
    LibusbW3u3Hardware& operator=(const LibusbW3u3Hardware&) = delete;

    // With no expected profile, resolve it from the primary USB descriptor.
    // An expected profile is still checked against that descriptor on claim.
    const DeviceProfile* device_profile() const noexcept { return profile_; }
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

    // px4-compatible 15 V gate. The daemon passes whether the user granted
    // --allow-lnb-power; the backend still requires model LNB control, so only
    // models with verified source-backed GPIO report UNSUPPORTED-free requests.
    // The default keeps standalone backend callers (tests, probes) unchanged.
    void set_allow_lnb_power(bool allow) noexcept { allow_lnb_power_ = allow; }
    bool allow_lnb_power() const noexcept { return allow_lnb_power_; }

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
        LibusbW3u3Hardware&, const char*, const char*, bool,
        const volatile std::sig_atomic_t*) noexcept;
    friend int run_live_card_stream_server(
        LibusbW3u3Hardware&, const char*, const char*, bool,
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
    int control_function(std::uint8_t, const ControlTransfer&, unsigned char*);
    struct CaptureUsbHooks {
        void* context = nullptr;
        int (*control)(void*, const ControlTransfer&, unsigned char*) = nullptr;
        libusb_transfer* (*allocate)(void*) = nullptr;
        int (*submit)(void*, libusb_transfer*) = nullptr;
        int (*cancel)(void*, libusb_transfer*) = nullptr;
        void (*free)(void*, libusb_transfer*) = nullptr;
        int (*pump_events)(void*, unsigned) = nullptr;
        void (*interrupt_events)(void*) = nullptr;
        int (*control_function)(void*, std::uint8_t, const ControlTransfer&, unsigned char*) = nullptr;
        // Test-only monotonic clock; production always uses steady_clock.
        std::chrono::steady_clock::time_point (*now)(void*) = nullptr;
    };
    libusb_transfer* allocate_transfer() noexcept;
    int submit_transfer(libusb_transfer*) noexcept;
    int cancel_transfer(libusb_transfer*) noexcept;
    void free_transfer(libusb_transfer*) noexcept;
    void delay_ms(unsigned) override;
    bool cancelled() const override;
    bool expired() const override;
    std::chrono::steady_clock::time_point steady_now() const noexcept;
    bool acquire_control_gate(std::chrono::steady_clock::time_point deadline,
                              const volatile std::sig_atomic_t* stop_flag = nullptr,
                              bool cleanup = false) noexcept;
    void mark_cleanup_failed(px4::userland::Error error) noexcept;
    bool dsc_start(std::uint8_t) override;
    bool dsc_stop(std::uint8_t) override;
    CaptureIo bulk_read(std::uint8_t, unsigned char*, int, int*, unsigned) override;
    bool read_cf40(std::uint8_t, std::uint8_t*) override;
    bool write_cf40(std::uint8_t, std::uint8_t) override;
    bool read_cf_block(std::uint8_t, std::uint8_t*, std::size_t) override;
    bool write_cf_block(std::uint8_t, const std::uint8_t*, std::size_t) override;
    bool terrestrial_locked(std::uint8_t, bool*, std::chrono::steady_clock::time_point) override;
    bool snapshot_link_diagnostic() override;
    bool runtime_model_supported() const noexcept;
    bool uses_legacy_frontend() const noexcept;
    bool uses_v2_frontend() const noexcept;
    bool verify_v2_pair_roles() noexcept;
    bool verify_v2_pair_identity() noexcept;
    SatelliteOperationResult run_model_satellite_tune(std::uint32_t) noexcept;
    SatelliteLockResult read_model_satellite_lock() noexcept;
    SatelliteLockResult poll_model_satellite_lock() noexcept;
    SatelliteTsidReadyResult wait_model_satellite_tsid(std::size_t, std::uint16_t,
                                                       bool) noexcept;
    SatelliteTsidSelectResult select_model_satellite_tsid(
        std::size_t, const std::array<std::uint16_t, kW3u3SatelliteTsidSlots>&) noexcept;
    LegacyFrontendProfile legacy_frontend() const noexcept;
    bool snapshot_gpio_state() noexcept;
    bool supports_lnb_control() const noexcept;
    px4::userland::Result<void> set_lnb_power(bool on, bool cleanup) noexcept;
    px4::userland::Result<void> clear_lnb_power() noexcept;
    px4::userland::Result<void> check_v2_lnb_feedback() noexcept;
    bool poll_v2_lnb_feedback_if_due() noexcept;
    bool start_lnb_monitor() noexcept;
    void stop_lnb_monitor() noexcept;
    void monitor_lnb_power() noexcept;
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

    const DeviceProfile* profile_ = nullptr;
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
    bool allow_lnb_power_ = true;
    bool card_cleanup_active_ = false;
    std::chrono::steady_clock::time_point deadline_{};
    bool deadline_active_ = false;
    std::recursive_timed_mutex control_gate_;
    std::atomic<bool> cleanup_io_active_{false};
    bool claimed_ = false;
    bool initialized_ = false;
    bool fd_function_mismatch_ = false;
    bool v2_roles_verified_ = false;
    bool v2_identity_verified_ = false;
    bool tuned_ = false;
    bool gain_applied_ = false;
    bool source_prepared_ = false;
    bool dsc_attempted_ = false;
    bool dsc_stopped_ = true;
    bool cf_snapshot_valid_ = false;
    bool link_snapshot_valid_ = false;
    bool link_apply_attempted_ = false;
    bool output_start_attempted_ = false;
    std::atomic<bool> cleanup_failed_{false};
    std::atomic<int> cleanup_failure_error_{0};
    static constexpr std::uint8_t kNoActiveReceiver = 0xffU;
    ReceiverLaneReservation active_receiver_{};
    std::uint8_t tuned_receiver_ = kNoActiveReceiver;
    std::uint8_t source_receiver_ = kNoActiveReceiver;
    px4::userland::ipc::System tuned_system_ = px4::userland::ipc::System::ISDB_T;
    const volatile std::sig_atomic_t* diagnostic_stop_flag_ = nullptr;
    bool gpio_snapshot_valid_ = false;
    std::uint8_t gpio_snapshot_ = 0;
    bool board_power_attempted_ = false;
    // LNB ownership is distinct from board-power snapshots: close/shutdown
    // must never restore an initially powered antenna feed. All fields below
    // are protected by control_gate_. Only set_lnb_power may bypass the
    // model-specific LNB mask on ordinary frontend plans.
    bool lnb_gpio_io_active_ = false;
    bool lnb_fault_latched_ = false;
    bool lnb_feedback_active_ = false;
    bool lnb_power_transition_active_ = false;
    std::chrono::steady_clock::time_point lnb_feedback_checked_at_{};
    std::atomic<bool> lnb_monitor_stop_{false};
    std::mutex lnb_monitor_mutex_;
    std::thread lnb_monitor_;
    bool lnb_state_known_ = false;
    bool lnb_on_ = false;
    bool lnb_cleanup_required_ = false;
    bool lnb_transaction_active_ = false;
    bool lnb_previous_on_ = false;
    bool lnb_requested_on_ = false;
    std::uint8_t lnb_transaction_receiver_ = kNoActiveReceiver;
    std::uint32_t tuned_frequency_khz_ = 0;
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
