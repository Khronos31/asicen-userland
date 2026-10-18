// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef ASICEN_USERLAND_USB_FUNCTION_OBSERVATION_H
#define ASICEN_USERLAND_USB_FUNCTION_OBSERVATION_H

#include <cstdint>
#include <vector>

namespace asicen {

struct ClaimedUsbFunctionObservation {
    std::uint16_t vendor_id = 0U;
    std::uint16_t product_id = 0U;
    bool has_bus = false;
    std::uint8_t bus = 0U;
    bool has_address = false;
    std::uint8_t address = 0U;
    std::vector<std::uint8_t> port_path;
};

}  // namespace asicen

#endif  // ASICEN_USERLAND_USB_FUNCTION_OBSERVATION_H
