// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/capture_drain.h"

#include <algorithm>
#include <thread>

namespace asicen {

CaptureRunResult drain_capture_callbacks(CaptureDrainOps& ops, bool dsc_attempted,
                                         bool dsc_already_stopped,
                                         std::chrono::milliseconds limit) noexcept
{
    const bool dsc_stopped = !dsc_attempted || dsc_already_stopped || ops.stop_dsc();
    ops.cancel_pending();
    const auto deadline = std::chrono::steady_clock::now() + limit;
    constexpr std::size_t kMaxDrainEventCalls = 8U;
    std::size_t drain_event_calls = 0U;
    while (ops.has_pending() && drain_event_calls < kMaxDrainEventCalls) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return CaptureRunResult::fatal_drain;
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        const unsigned wait_ms =
            static_cast<unsigned>(std::max<long long>(1, std::min<long long>(1000, remaining)));
        ops.pump_events(wait_ms);
        ++drain_event_calls;
    }
    if (ops.has_pending()) {
        return CaptureRunResult::fatal_drain;
    }
    ops.release_transfers();
    return ops.cleanup_after_drain(dsc_stopped, dsc_attempted);
}

CaptureRunResult cleanup_capture_state(CaptureCleanupOps& ops, bool dsc_stopped, bool dsc_attempted,
                                       bool link_apply_attempted, bool output_start_attempted,
                                       bool cf_snapshot_valid) noexcept
{
    if (ops.disconnected()) {
        return CaptureRunResult::disconnected;
    }
    bool clean = !(dsc_attempted && !dsc_stopped);
    if (dsc_attempted && !dsc_stopped) {
        clean = ops.disable_output_and_verify() && clean;
    } else if (link_apply_attempted) {
        clean = ops.clear_seed_and_verify_output() && clean;
    } else if (output_start_attempted) {
        clean = ops.disable_output_and_verify() && clean;
    }
    if (cf_snapshot_valid) {
        clean = ops.restore_cf_and_verify() && clean;
    }
    if (ops.disconnected()) {
        return CaptureRunResult::disconnected;
    }
    return clean ? CaptureRunResult::cancelled : CaptureRunResult::usb_error;
}

bool attempt_hardware_shutdown_cleanup(HardwareShutdownOps& ops, bool stop_needed,
                                       bool gpio_snapshot_valid) noexcept
{
    bool clean = true;
    if (stop_needed && !ops.stop_capture_safely()) {
        clean = false;
    }
    if (gpio_snapshot_valid && !ops.restore_gpio_snapshot_safely()) {
        clean = false;
    }
    return clean;
}

}  // namespace asicen
