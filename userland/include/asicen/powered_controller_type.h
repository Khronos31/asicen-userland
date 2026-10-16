// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef ASICEN_USERLAND_POWERED_CONTROLLER_TYPE_H
#define ASICEN_USERLAND_POWERED_CONTROLLER_TYPE_H

#include "asicen/device_profile.h"

#include <cstdint>

namespace asicen {

inline bool powered_controller_type_supported(const DeviceProfile* profile,
                                             std::uint8_t raw) noexcept
{
    if (profile == nullptr || find_profile(profile->model_id) != profile) {
        return false;
    }

    const std::uint8_t decoded = static_cast<std::uint8_t>((raw & 0x3eU) >> 1U);
    return decoded == 0x0fU ||
           (profile->model_id == ModelId::W3u2 && decoded == 0x10U);
}

}  // namespace asicen

#endif  // ASICEN_USERLAND_POWERED_CONTROLLER_TYPE_H
