#include "asicen/libusb_transport.h"

namespace asicen {

LibusbDevice::~LibusbDevice() {
    close();
}

int LibusbDevice::open(libusb_context* context, UsbLocation location) {
    close();

    libusb_device** list = nullptr;
    const ssize_t count = libusb_get_device_list(context, &list);
    if (count < 0) {
        return static_cast<int>(count);
    }

    int result = LIBUSB_ERROR_NO_DEVICE;
    for (ssize_t i = 0; i < count; ++i) {
        libusb_device* candidate = list[i];
        if (libusb_get_bus_number(candidate) != location.bus ||
            libusb_get_device_address(candidate) != location.address) {
            continue;
        }

        result = libusb_open(candidate, &handle_);
        break;
    }

    libusb_free_device_list(list, 1);
    return result;
}

void LibusbDevice::close() {
    if (handle_ != nullptr) {
        libusb_close(handle_);
        handle_ = nullptr;
    }
}

bool LibusbDevice::is_open() const {
    return handle_ != nullptr;
}

libusb_device_handle* LibusbDevice::handle() const {
    return handle_;
}

libusb_device* LibusbDevice::device() const {
    return handle_ == nullptr ? nullptr : libusb_get_device(handle_);
}

int LibusbDevice::control(const ControlTransfer& transfer, unsigned char* data) {
    if (handle_ == nullptr) {
        return LIBUSB_ERROR_NO_DEVICE;
    }

    return libusb_control_transfer(
        handle_,
        bm_request_type(transfer.direction),
        static_cast<std::uint8_t>(transfer.request),
        transfer.value,
        transfer.index,
        data,
        transfer.length,
        transfer.timeout_ms);
}

}  // namespace asicen
