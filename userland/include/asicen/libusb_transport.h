#pragma once

#include <libusb.h>

#include <cstdint>

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

private:
    libusb_device_handle* handle_ = nullptr;
};

}  // namespace asicen
