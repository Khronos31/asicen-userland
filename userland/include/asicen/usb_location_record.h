// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef ASICEN_USERLAND_USB_LOCATION_RECORD_H
#define ASICEN_USERLAND_USB_LOCATION_RECORD_H

#include "asicen/libusb_transport.h"

#include <dtv/userland/records.h>

#include <charconv>
#include <cstdint>
#include <system_error>
#include <vector>

namespace asicen {

inline bool set_usb_location_record(
    dtv::userland::wire::Location& record, UsbLocation location,
    const std::vector<std::uint8_t>& port_path) noexcept
{
    using dtv::userland::detail::kMaxUsbPathBytes;
    using dtv::userland::wire::WString;

    char path[kMaxUsbPathBytes]{};
    char* cursor = path;
    char* const end = path + sizeof(path);
    const auto append_number = [&cursor, end](unsigned value) noexcept {
        const auto result = std::to_chars(cursor, end, value);
        if (result.ec != std::errc{}) return false;
        cursor = result.ptr;
        return true;
    };

    if (location.bus != 0U && !port_path.empty()) {
        if (!append_number(location.bus)) return false;
        for (std::size_t index = 0U; index < port_path.size(); ++index) {
            if (cursor == end) return false;
            *cursor++ = index == 0U ? '-' : '.';
            if (!append_number(port_path[index])) return false;
        }
    }

    if (location.bus != 0U && !port_path.empty()) {
        record.bus.present = true;
        record.bus.value = location.bus;
    }
    if (location.address != 0U) {
        record.address.present = true;
        record.address.value = location.address;
    }
    if (location.bus != 0U) {
        auto& usb_path = record.usb_path;
        usb_path.present = true;
        usb_path.value.length = static_cast<std::uint16_t>(cursor - path);
        for (std::size_t index = 0U; index < usb_path.value.length; ++index)
            usb_path.value.bytes[index] = path[index];
    }
    return true;
}

}  // namespace asicen

#endif
