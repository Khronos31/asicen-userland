// SPDX-License-Identifier: GPL-2.0-only
// Location formatting follows px4d_list_format.cpp at
// 1a1485d0c3e972e0a47be907edb67949564aa9a7. ASICEN's missing serial descriptor
// requires a topology identity; re-enumeration must never widen that identity.
#ifndef ASICEN_USERLAND_ASICEND_IDENTITY_H
#define ASICEN_USERLAND_ASICEND_IDENTITY_H

#include "asicen/device_profile.h"
#include "asicen/enclosure_grouping.h"

#include <string>

namespace asicen::cli {

inline std::string port_string(const UsbFunctionObservation& location)
{
    if (location.bus == 0U || location.port_path.empty()) return {};
    std::string value = std::to_string(location.bus);
    for (std::size_t i = 0U; i < location.port_path.size(); ++i) {
        value.push_back(i == 0U ? '-' : '.');
        value += std::to_string(location.port_path[i]);
    }
    return value;
}

inline std::string topology_identity(const UsbFunctionObservation& observation,
                                      bool dual)
{
    UsbFunctionObservation enclosure = observation;
    if (dual) {
        if (enclosure.port_path.size() < 2U) return {};
        enclosure.port_path.pop_back();
    }
    const std::string port = port_string(enclosure);
    return port.empty() ? std::string{} : "usb-" + port;
}

inline bool runtime_matches_loader(const UsbFunctionObservation& observation,
                                    const DeviceProfile& model, std::uint8_t bus,
                                    const std::vector<std::uint8_t>& port_path) noexcept
{
    return !port_path.empty() && observation.vid == model.vid && observation.pid == model.pid &&
           observation.bus == bus && observation.port_path == port_path;
}

}  // namespace asicen::cli

#endif  // ASICEN_USERLAND_ASICEND_IDENTITY_H
