// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/product_profile.h"

#include <cstdio>

int main()
{
    using namespace asicen::profile;
    constexpr std::uint8_t expected_device[] = {1U, 1U, 2U, 2U};
    constexpr std::uint8_t expected_local[] = {0U, 1U, 0U, 1U};
    constexpr bool expected_satellite[] = {true, false, true, false};
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
