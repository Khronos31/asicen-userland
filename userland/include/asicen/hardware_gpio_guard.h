// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "asicen/protocol.h"

namespace asicen {

// Applies the hardware-daemon's no-LNB rule to a planned GPIO control. A
// successful result with skip=true has no remaining mask and must not reach
// USB. GPIOEx writes are always rejected by this backend policy.
bool mask_lnb_gpio_operation(const ControlTransfer& input,
                             ControlTransfer* output,
                             bool* skip) noexcept;

}  // namespace asicen
