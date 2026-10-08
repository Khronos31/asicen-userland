// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/hardware_gpio_guard.h"

namespace asicen {

bool mask_lnb_gpio_operation(const ControlTransfer& input,
                             ControlTransfer* output,
                             bool* skip) noexcept {
    if (output == nullptr || skip == nullptr) return false;
    *output = input;
    *skip = false;
    if (input.request == Request::GpioExSet) return false;
    if (input.request != Request::Gpio) return true;
    const std::uint8_t value = static_cast<std::uint8_t>(input.value & 0xffU);
    const std::uint8_t original_mask = static_cast<std::uint8_t>(input.value >> 8U);
    // Mask zero is the source-proven GPIO read form. It cannot change the
    // output latch and must pass through for state snapshot/readback.
    if (original_mask == 0U) {
        output->value = setup_word(0U, 0U);
        return true;
    }
    const std::uint8_t mask = static_cast<std::uint8_t>(original_mask & 0xdfU);
    const std::uint8_t safe_value = static_cast<std::uint8_t>(value & mask);
    if (mask == 0U) {
        *skip = true;
        return true;
    }
    output->value = setup_word(safe_value, mask);
    return true;
}

}  // namespace asicen
