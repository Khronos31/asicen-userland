#pragma once

#include <libusb.h>

#include <cstdint>
#include <vector>

#include "asicen/protocol.h"

namespace asicen {

struct UsbLocation {
    std::uint8_t bus = 0;
    std::uint8_t address = 0;
};

class LibusbDevice final {
public:
    LibusbDevice() = default;
    ~LibusbDevice();

    LibusbDevice(const LibusbDevice&) = delete;
    LibusbDevice& operator=(const LibusbDevice&) = delete;

    int open(libusb_context* context, UsbLocation location);
    void close();

    bool is_open() const;
    libusb_device_handle* handle() const;
    libusb_device* device() const;

    int control(const ControlTransfer& transfer, unsigned char* data);

    // Vendor host-to-device control transfer with an explicit setup. Returns the
    // transferred byte count or a negative libusb error code.
    int vendor_out(std::uint8_t request,
                   std::uint16_t value,
                   std::uint16_t index,
                   const unsigned char* data,
                   std::uint16_t length,
                   unsigned int timeout_ms);

    // Returns 1 when a kernel driver is bound to interface_number, 0 when the
    // interface is free, or a negative libusb error code.
    int kernel_driver_active(int interface_number) const;

    // Claims interface_number as-is. Released by close(). Repeated claims are
    // ignored; kernel drivers are never detached.
    int claim_interface(int interface_number);

    // Finds the active alternate setting that owns endpoint_address, claims
    // its interface, and selects the alternate setting when needed.
    // Kernel drivers are never detached implicitly.
    int claim_endpoint(std::uint8_t endpoint_address);

    int bulk_read(std::uint8_t endpoint_address,
                  unsigned char* data,
                  int length,
                  int* transferred,
                  unsigned int timeout_ms);

private:
    bool interface_claimed(int interface_number) const;

    libusb_device_handle* handle_ = nullptr;
    std::vector<int> claimed_interfaces_;
};

}  // namespace asicen
