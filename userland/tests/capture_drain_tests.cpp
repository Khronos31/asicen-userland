// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/capture_drain.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {
void check(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

class FakeDrain final : public asicen::CaptureDrainOps,
                        public asicen::CaptureCleanupOps {
public:
    bool stop_dsc() noexcept override { events.emplace_back("dsc-stop"); return dsc_ok; }
    void cancel_pending() noexcept override { events.emplace_back("cancel"); cancel_called = true; }
    bool has_pending() const noexcept override { return pending; }
    void pump_events(unsigned) noexcept override {
        events.emplace_back("pump");
        if (!withhold_callback && cancel_called) pending = false;
    }
    void release_transfers() noexcept override { events.emplace_back("free"); freed = true; }
    asicen::CaptureRunResult cleanup_after_drain(bool dsc_stopped,
                                                 bool dsc_attempted) noexcept override {
        events.emplace_back("cleanup");
        return asicen::cleanup_capture_state(*this, dsc_stopped, dsc_attempted,
            link_apply_attempted, output_start_attempted, cf_snapshot_valid);
    }
    bool disconnected() const noexcept override { return disconnected_flag; }
    bool clear_seed_and_verify_output() noexcept override {
        events.emplace_back("clear-seed"); seeds_cleared = true; return cleanup_ok;
    }
    bool disable_output_and_verify() noexcept override {
        events.emplace_back("disable-output"); disable_output = true; return cleanup_ok;
    }
    bool restore_cf_and_verify() noexcept override {
        events.emplace_back("restore-cf"); cf_restored = true; return cleanup_ok;
    }
    bool pending = false;
    bool dsc_ok = true;
    bool withhold_callback = false;
    bool cancel_called = false;
    bool freed = false;
    bool link_apply_attempted = false;
    bool output_start_attempted = false;
    bool cf_snapshot_valid = false;
    bool disable_output = false;
    bool seeds_cleared = false;
    bool cf_restored = false;
    bool cleanup_ok = true;
    bool disconnected_flag = false;
    std::vector<std::string> events;
};

class FakeShutdown final : public asicen::HardwareShutdownOps {
public:
    bool stop_capture_safely() noexcept override {
        events.emplace_back("stop");
        return stop_ok;
    }
    bool restore_gpio_snapshot_safely() noexcept override {
        events.emplace_back("gpio-restore");
        ++gpio_restore_calls;
        return gpio_ok;
    }
    bool stop_ok = false;
    bool gpio_ok = true;
    unsigned gpio_restore_calls = 0;
    std::vector<std::string> events;
};

void partially_submitted_setup_failure_drains_before_free() {
    FakeDrain fake;
    fake.pending = true; // one of four transfers submitted before a later submit failed
    const auto result = asicen::drain_capture_callbacks(
        fake, false, false, std::chrono::milliseconds(100));
    check(result == asicen::CaptureRunResult::cancelled && fake.freed,
          "partial setup failure drains and releases callbacks");
    check(fake.events.size() >= 4U && fake.events[0] == "cancel" &&
              fake.events[1] == "pump" && fake.events[2] == "free" &&
              fake.events[3] == "cleanup",
          "release follows callback drain and precedes state cleanup");
}

void failed_dsc_stop_disables_output_but_never_clears_seed() {
    FakeDrain fake;
    fake.pending = true;
    fake.dsc_ok = false;
    fake.link_apply_attempted = true;
    fake.output_start_attempted = true;
    fake.cf_snapshot_valid = true;
    const auto result = asicen::drain_capture_callbacks(
        fake, true, false, std::chrono::milliseconds(100));
    check(result == asicen::CaptureRunResult::usb_error && fake.freed,
          "failed DSC stop is reported after callback drain");
    check(fake.disable_output && !fake.seeds_cleared,
          "failed DSC stop independently disables output and skips seed writes");
    check(fake.cf_restored,
          "CF restoration is still attempted after the independent output disable");
}

void preapply_failure_does_not_clear_unwritten_seed_and_disconnect_skips_writes() {
    FakeDrain preapply;
    preapply.cf_snapshot_valid = true;
    const auto restored = asicen::cleanup_capture_state(
        preapply, true, true, false, false, true);
    check(restored == asicen::CaptureRunResult::cancelled && preapply.cf_restored &&
              !preapply.seeds_cleared && !preapply.disable_output,
          "failure before seed/output apply restores only snapshotted CF state");
    FakeDrain gone;
    gone.disconnected_flag = true;
    const auto disconnected = asicen::cleanup_capture_state(
        gone, false, true, true, true, true);
    check(disconnected == asicen::CaptureRunResult::disconnected &&
              !gone.disable_output && !gone.seeds_cleared && !gone.cf_restored,
          "disconnect forbids all subsequent cleanup writes");
}

void successful_dsc_stop_clears_seed_then_restores_cf() {
    FakeDrain fake;
    fake.link_apply_attempted = true;
    fake.output_start_attempted = true;
    fake.cf_snapshot_valid = true;
    const auto result = asicen::cleanup_capture_state(fake, true, true, true, true, true);
    check(result == asicen::CaptureRunResult::cancelled && fake.seeds_cleared &&
              fake.cf_restored && !fake.disable_output,
          "successful stop clears attempted seed/output and restores CF snapshot");
}

void withheld_callbacks_escalate_without_free_or_cleanup() {
    FakeDrain fake;
    fake.pending = true;
    fake.withhold_callback = true;
    const auto result = asicen::drain_capture_callbacks(
        fake, true, false, std::chrono::milliseconds(20));
    check(result == asicen::CaptureRunResult::fatal_drain,
          "withheld callback reaches bounded fatal result");
    check(!fake.freed && fake.events.back() == "pump" &&
              std::find(fake.events.begin(), fake.events.end(), "cleanup") == fake.events.end(),
          "callback storage and device state remain untouched after drain timeout");
}

void ordinary_stop_failure_still_attempts_independent_gpio_restore() {
    FakeShutdown fake;
    const bool clean = asicen::attempt_hardware_shutdown_cleanup(fake, true, true);
    check(!clean && fake.gpio_restore_calls == 1U,
          "ordinary capture cleanup failure does not skip GPIO restoration");
    check(fake.events.size() == 2U && fake.events[0] == "stop" &&
              fake.events[1] == "gpio-restore",
          "GPIO restoration follows safe stop attempt and preserves its failure result");
}
}  // namespace

int main() {
    partially_submitted_setup_failure_drains_before_free();
    failed_dsc_stop_disables_output_but_never_clears_seed();
    preapply_failure_does_not_clear_unwritten_seed_and_disconnect_skips_writes();
    successful_dsc_stop_clears_seed_then_restores_cf();
    withheld_callbacks_escalate_without_free_or_cleanup();
    ordinary_stop_failure_still_attempts_independent_gpio_restore();
    std::cout << "capture drain tests passed\n";
    return 0;
}
