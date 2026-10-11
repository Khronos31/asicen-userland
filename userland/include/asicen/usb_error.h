// SPDX-License-Identifier: GPL-2.0-only
#ifndef ASICEN_USERLAND_USB_ERROR_H
#define ASICEN_USERLAND_USB_ERROR_H

#include "px4/error.h"

namespace asicen {

// libusb's stable public error-number contract. Kept independent of libusb.h
// so offline control/card transport adapters share the exact same mapping.
inline px4::userland::Error map_libusb_error(int error) noexcept
{
    using px4::userland::Error;
    switch (error) {
    case 0:
        return Error::OK;
    case -7:
        return Error::TIMEOUT;
    case -4:
        return Error::DISCONNECTED;
    case -6:
        return Error::BUSY;
    case -5:
        return Error::NOT_FOUND;
    case -12:
        return Error::UNSUPPORTED;
    case -2:
        return Error::INVALID_ARGUMENT;
    case -11:
        return Error::INTERNAL;
    default:
        return Error::USB_IO;
    }
}

}  // namespace asicen

#endif  // ASICEN_USERLAND_USB_ERROR_H
