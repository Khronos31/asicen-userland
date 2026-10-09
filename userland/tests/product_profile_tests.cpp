// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/product_profile.h"

#include <cstdio>

int main()
{
    using namespace asicen::profile;
    constexpr std::uint8_t expected_device[] = {1U, 1U, 2U, 2U};
    constexpr std::uint8_t expected_local[] = {0U, 1U, 0U, 1U};
    constexpr bool expected_satellite[] = {true, false, true, false};
    static_assert(valid_receiver_count(1U) && valid_receiver_count(2U) &&
                  valid_receiver_count(4U));
    static_assert(!valid_receiver_count(0U) && !valid_receiver_count(3U) &&
                  !valid_receiver_count(8U));
    static_assert(usb_function_count(1U) == 1U && usb_function_count(2U) == 1U &&
                  usb_function_count(4U) == 2U && usb_function_count(3U) == 0U);
    static_assert(usb_present_mask(1U) == 0x01U && usb_present_mask(2U) == 0x01U &&
                  usb_present_mask(4U) == 0x03U && usb_present_mask(3U) == 0U);
    static_assert(is_combined_receiver(1U, 0U) && !is_combined_receiver(1U, 1U) &&
                  !is_combined_receiver(2U, 0U) && !is_combined_receiver(4U, 0U));
    static_assert(kReceiverCount == 4U);
    static_assert(kUsbFunctionCount == 2U);
    static_assert(kReceiversPerUsbFunction == 2U);
    for (std::uint8_t receiver = 0U; receiver < kReceiverCount; ++receiver) {
        const auto function = usb_function_for_receiver(receiver);
        if (function + 1U != expected_device[receiver] ||
            local_receiver_for(receiver) != expected_local[receiver] ||
            is_satellite_receiver(receiver) != expected_satellite[receiver]) {
            std::fprintf(stderr, "profile mapping mismatch at receiver %u\n",
                         static_cast<unsigned int>(receiver));
            return 1;
        }
    }
    return 0;
}
