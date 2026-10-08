// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/frontend_sequence.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {
void check(bool ok, const char* message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void controller_may_nack_before_power_then_pass_after_power() {
    bool powered = false;
    unsigned probes = 0;
    auto controller_probe = [&] {
        ++probes;
        return powered;  // Off-state bridge NACK; powered controller responds.
    };
    check(!controller_probe(), "pre-power controller access models the observed NACK");

    std::vector<std::string> order;
    const auto result = asicen::execute_powered_init_sequence(
        [&] { order.emplace_back("power"); powered = true; return true; },
        [&] { order.emplace_back("controller-guard"); return controller_probe(); },
        [&] { order.emplace_back("demod-init"); return true; });
    check(result == asicen::PoweredInitResult::completed && probes == 2U,
          "controller is rechecked only after successful power-up");
    check(order == std::vector<std::string>{"power", "controller-guard", "demod-init"},
          "power precedes controller type/idle guard and demod initialization");
}

void powered_controller_guard_failure_stops_before_demod_init() {
    std::vector<std::string> order;
    const auto result = asicen::execute_powered_init_sequence(
        [&] { order.emplace_back("power"); return true; },
        [&] { order.emplace_back("controller-guard"); return false; },
        [&] { order.emplace_back("demod-init"); return true; });
    check(result == asicen::PoweredInitResult::controller_guard_failed,
          "post-power NACK or invalid controller state aborts initialization");
    check(order == std::vector<std::string>{"power", "controller-guard"},
          "demod initialization is not attempted after a powered guard failure");
}
}  // namespace

int main() {
    controller_may_nack_before_power_then_pass_after_power();
    powered_controller_guard_failure_stops_before_demod_init();
    std::cout << "powered init sequence tests passed\n";
    return 0;
}
