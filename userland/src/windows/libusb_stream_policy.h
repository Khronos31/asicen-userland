// SPDX-License-Identifier: GPL-2.0-only
#ifndef ASICEN_WINDOWS_LIBUSB_STREAM_POLICY_H
#define ASICEN_WINDOWS_LIBUSB_STREAM_POLICY_H

#include <cstddef>
#include <cstdint>

struct libusb_device_handle;

namespace asicen {

// A narrow seam for testing the WinUSB policy without a Windows host or USB.
class WindowsRawIoApi {
public:
    virtual ~WindowsRawIoApi() noexcept = default;
    virtual int supports_raw_io() noexcept = 0;
    virtual int maximum_packet_size() noexcept = 0;
    virtual int maximum_transfer_size() noexcept = 0;
    virtual int enable_raw_io() noexcept = 0;
};

int prepare_windows_stream(WindowsRawIoApi& api, std::size_t* transfer_size,
                           bool* raw_io_enabled) noexcept;

int prepare_libusb_stream(libusb_device_handle* handle, std::uint8_t endpoint,
                          std::size_t* transfer_size, bool* raw_io_enabled) noexcept;
int finish_libusb_stream(libusb_device_handle* handle, std::uint8_t endpoint) noexcept;

}  // namespace asicen

#endif
