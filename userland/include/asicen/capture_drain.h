// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <cstdint>

namespace asicen {

enum class CaptureRunResult : std::uint8_t {
    completed,
    cancelled,
    usb_error,
    disconnected,
    sync_error,
    fatal_drain,
};

// Minimal injected seam for the exact callback-drain policy used by the
// libusb backend. Implementations retain transfer/callback storage until
// release_transfers() is called after has_pending() becomes false.
class CaptureDrainOps {
public:
    virtual ~CaptureDrainOps() noexcept = default;
    virtual bool stop_dsc() noexcept = 0;
    virtual void cancel_pending() noexcept = 0;
    virtual bool has_pending() const noexcept = 0;
    virtual void pump_events(unsigned timeout_ms) noexcept = 0;
    virtual void release_transfers() noexcept = 0;
    virtual CaptureRunResult cleanup_after_drain(bool dsc_stopped,
                                                 bool dsc_attempted) noexcept = 0;
};

class CaptureCleanupOps {
public:
    virtual ~CaptureCleanupOps() noexcept = default;
    virtual bool disconnected() const noexcept = 0;
    virtual bool clear_seed_and_verify_output() noexcept = 0;
    virtual bool disable_output_and_verify() noexcept = 0;
    virtual bool restore_cf_and_verify() noexcept = 0;
};

// Shutdown cleanup is deliberately best-effort after an ordinary stop error:
// independent restoration (for example the GPIO snapshot) must still run.
// Fatal callback-drain failures never return from stop_capture_safely().
class HardwareShutdownOps {
public:
    virtual ~HardwareShutdownOps() noexcept = default;
    virtual bool stop_capture_safely() noexcept = 0;
    virtual bool restore_gpio_snapshot_safely() noexcept = 0;
};

CaptureRunResult drain_capture_callbacks(CaptureDrainOps& ops,
                                         bool dsc_attempted,
                                         bool dsc_already_stopped,
                                         std::chrono::milliseconds limit) noexcept;
CaptureRunResult cleanup_capture_state(CaptureCleanupOps& ops,
                                       bool dsc_stopped, bool dsc_attempted,
                                       bool link_apply_attempted,
                                       bool output_start_attempted,
                                       bool cf_snapshot_valid) noexcept;
bool attempt_hardware_shutdown_cleanup(HardwareShutdownOps& ops,
                                      bool stop_needed,
                                      bool gpio_snapshot_valid) noexcept;

}  // namespace asicen
