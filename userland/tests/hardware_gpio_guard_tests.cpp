// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/frontend_sequence.h"
#include "asicen/hardware_gpio_guard.h"
#include "asicen/write_protocol.h"

#include <cstdlib>
#include <iostream>

namespace {
void check(bool ok, const char* message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void exhaustive_gpio_value_mask_guard() {
    for (unsigned int value = 0; value <= 0xffU; ++value) {
        for (unsigned int mask = 0; mask <= 0xffU; ++mask) {
            asicen::ControlTransfer output{};
            bool skip = false;
            const auto input = asicen::make_gpio_set(
                static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(mask));
            check(asicen::mask_lnb_gpio_operation(input, &output, &skip),
                  "ordinary GPIO operation is sanitizable");
            if (skip) {
                check((mask & 0xdfU) == 0U,
                      "skip means all requested bits were forbidden LNB bit");
            } else {
                const auto safe_mask = static_cast<std::uint8_t>(output.value >> 8U);
                const auto safe_value = static_cast<std::uint8_t>(output.value & 0xffU);
                check((safe_mask & 0x20U) == 0U,
                      "no emitted GPIO operation addresses the LNB bit");
                check((safe_value & static_cast<std::uint8_t>(~safe_mask)) == 0U,
                      "GPIO value contains no unmasked bits");
                check((safe_value & static_cast<std::uint8_t>(mask & 0xdfU)) ==
                          (value & mask & 0xdfU),
                      "all allowed GPIO bits preserve requested value");
            }
        }
    }
}

void existing_startup_plans_are_guarded_before_transport() {
    auto plan = asicen::plan_startup_subset();
    const auto power = asicen::plan_safe_power_on();
    plan.insert(plan.end(), power.begin(), power.end());
    bool saw_lnb_only = false;
    for (const auto& op : plan) {
        if (op.kind != asicen::FrontendOpKind::Control ||
            op.transfer.request != asicen::Request::Gpio) continue;
        asicen::ControlTransfer safe{};
        bool skip = false;
        check(asicen::mask_lnb_gpio_operation(op.transfer, &safe, &skip),
              "GPIO plan operation passes through policy");
        if (skip) saw_lnb_only = true;
        else check((safe.value >> 8U & 0x20U) == 0U,
                   "startup/power plan cannot write LNB bit");
    }
    check(saw_lnb_only, "the LNB-only power step is skipped entirely");
    asicen::ControlTransfer safe{};
    bool skip = false;
    check(!asicen::mask_lnb_gpio_operation(
              asicen::make_gpio_ex_set(0xffU, 0xffU), &safe, &skip),
          "GPIOEx writes are rejected by product policy");
}
}  // namespace

int main() {
    exhaustive_gpio_value_mask_guard();
    existing_startup_plans_are_guarded_before_transport();
    std::cout << "hardware GPIO guard tests passed\n";
    return 0;
}
