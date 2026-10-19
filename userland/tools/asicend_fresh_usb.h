// SPDX-License-Identifier: GPL-2.0-only
// Cold-startup USB observation and re-acquisition after a loader transfer.
// The per-scan context follows the loader lab's fresh observer
// (asicen-common-loader-lab src/libusb_backend.cpp scan()): a context created
// before re-enumeration is not trusted to show the new runtime or address.
#ifndef ASICEN_USERLAND_ASICEND_FRESH_USB_H
#define ASICEN_USERLAND_ASICEND_FRESH_USB_H

#include "asicen/enclosure_grouping.h"
#include "asicen/libusb_transport.h"
#include "px4/error.h"

#include <libusb.h>

#include <cstdint>
#include <vector>

namespace asicen::cli {

// Observes one bus in a context initialized for this call only. Bus, address,
// port path and VID:PID are copied into owned values; the list is freed with
// unref before the context exits, so no libusb pointer outlives the call.
inline px4::userland::Error observe_bus_fresh(
    LibusbAcquisitionApi& api, std::uint8_t bus,
    std::vector<UsbFunctionObservation>* observations) noexcept
{
    if (observations == nullptr) return px4::userland::Error::INVALID_ARGUMENT;
    observations->clear();
    LibusbContext scan(api);
    const int initialized = scan.initialize();
    if (initialized != 0) return map_libusb_acquisition_error(initialized);
    libusb_device** list = nullptr;
    const ssize_t count = api.get_device_list(scan.get(), &list);
    if (count < 0) return map_libusb_error(static_cast<int>(count));
    for (ssize_t i = 0; i < count; ++i) {
        libusb_device* device = list[i];
        if (api.get_bus_number(device) != bus) continue;
        libusb_device_descriptor descriptor{};
        if (api.get_device_descriptor(device, &descriptor) != 0) continue;
        std::uint8_t ports[8]{};
        const int port_count = api.get_port_numbers(device, ports, static_cast<int>(sizeof(ports)));
        if (port_count <= 0 || port_count > static_cast<int>(sizeof(ports))) continue;
        observations->push_back(UsbFunctionObservation{
            descriptor.idVendor, descriptor.idProduct, bus,
            std::vector<std::uint8_t>(ports, ports + port_count),
            api.get_device_address(device)});
    }
    api.free_device_list(list, 1);
    return px4::userland::Error::OK;
}

// Replaces the context used to reopen a re-enumerated function: the device that
// used it is closed first, then the old context exits and a new one starts.
// Declare the context before its device so the handle unwinds first.
inline int refresh_context(LibusbContext& context, LibusbDevice* device)
{
    if (device != nullptr) device->close();
    context.close();
    return context.initialize();
}

}  // namespace asicen::cli

#endif  // ASICEN_USERLAND_ASICEND_FRESH_USB_H
