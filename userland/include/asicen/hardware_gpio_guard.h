// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef ASICEN_USERLAND_HARDWARE_GPIO_GUARD_H
#define ASICEN_USERLAND_HARDWARE_GPIO_GUARD_H

#include "asicen/protocol.h"

#include <cstdint>

namespace asicen {

// Applies the hardware-daemon's no-LNB rule to a planned GPIO control. A
// successful result with skip=true has no remaining mask and must not reach
// USB. GPIOEx writes are always rejected by this backend policy.
bool mask_lnb_gpio_operation(const ControlTransfer& input, ControlTransfer* output,
                             bool* skip) noexcept;

// Reacquire the non-LNB GPIO snapshot after a previous successful restore.
// Existing valid snapshots are preserved; failed reads never mark one valid.
template <typename ReadGpio>
bool snapshot_gpio_if_needed(bool* valid, std::uint8_t* snapshot, ReadGpio read_gpio)
{
    if (valid == nullptr || snapshot == nullptr) {
        return false;
    }
    if (*valid) {
        return true;
    }
    std::uint8_t value = 0;
    if (!read_gpio(&value)) {
        return false;
    }
    *snapshot = value;
    *valid = true;
    return true;
}

}  // namespace asicen

#endif  // ASICEN_USERLAND_HARDWARE_GPIO_GUARD_H
