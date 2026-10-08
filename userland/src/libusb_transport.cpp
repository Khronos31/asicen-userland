#include "asicen/libusb_transport.h"

#include <algorithm>

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
        for (auto it = claimed_interfaces_.rbegin();
             it != claimed_interfaces_.rend(); ++it) {
            libusb_release_interface(handle_, *it);
        }
        claimed_interfaces_.clear();
        libusb_close(handle_);
        handle_ = nullptr;
    } else {
        claimed_interfaces_.clear();
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

int LibusbDevice::vendor_out(std::uint8_t request,
                             std::uint16_t value,
                             std::uint16_t index,
                             const unsigned char* data,
                             std::uint16_t length,
                             unsigned int timeout_ms) {
    if (handle_ == nullptr) {
        return LIBUSB_ERROR_NO_DEVICE;
    }
    return libusb_control_transfer(handle_, kVendorOut, request, value, index,
                                   const_cast<unsigned char*>(data), length,
                                   timeout_ms);
}

int LibusbDevice::kernel_driver_active(int interface_number) const {
    if (handle_ == nullptr) {
        return LIBUSB_ERROR_NO_DEVICE;
    }
    return libusb_kernel_driver_active(handle_, interface_number);
}

int LibusbDevice::claim_interface(int interface_number) {
    if (handle_ == nullptr) {
        return LIBUSB_ERROR_NO_DEVICE;
    }
    if (interface_claimed(interface_number)) {
        return interface_number;
    }

    const int claim_rc = libusb_claim_interface(handle_, interface_number);
    if (claim_rc != 0) {
        return claim_rc;
    }

    claimed_interfaces_.push_back(interface_number);
    return interface_number;
}

int LibusbDevice::release_interface(int interface_number) noexcept {
    if (handle_ == nullptr) return LIBUSB_ERROR_NO_DEVICE;
    const auto it = std::find(claimed_interfaces_.begin(),
                              claimed_interfaces_.end(), interface_number);
    if (it == claimed_interfaces_.end()) return 0;
    const int rc = libusb_release_interface(handle_, interface_number);
    if (rc == 0) claimed_interfaces_.erase(it);
    return rc;
}

bool LibusbDevice::interface_claimed(int interface_number) const {
    return std::find(claimed_interfaces_.begin(), claimed_interfaces_.end(),
                     interface_number) != claimed_interfaces_.end();
}

int LibusbDevice::claim_endpoint(std::uint8_t endpoint_address) {
    if (handle_ == nullptr) {
        return LIBUSB_ERROR_NO_DEVICE;
    }

    libusb_config_descriptor* config = nullptr;
    const int config_rc =
        libusb_get_active_config_descriptor(device(), &config);
    if (config_rc != 0 || config == nullptr) {
        return config_rc != 0 ? config_rc : LIBUSB_ERROR_NOT_FOUND;
    }

    int interface_number = -1;
    int alternate_setting = 0;
    for (int i = 0; i < config->bNumInterfaces && interface_number < 0; ++i) {
        const libusb_interface& iface = config->interface[i];
        for (int a = 0; a < iface.num_altsetting && interface_number < 0; ++a) {
            const libusb_interface_descriptor& alt = iface.altsetting[a];
            for (std::uint8_t e = 0; e < alt.bNumEndpoints; ++e) {
                if (alt.endpoint[e].bEndpointAddress == endpoint_address) {
                    interface_number = alt.bInterfaceNumber;
                    alternate_setting = alt.bAlternateSetting;
                    break;
                }
            }
        }
    }
    libusb_free_config_descriptor(config);

    if (interface_number < 0) {
        return LIBUSB_ERROR_NOT_FOUND;
    }
    if (interface_claimed(interface_number)) {
        return interface_number;
    }

    const int claim_rc = libusb_claim_interface(handle_, interface_number);
    if (claim_rc != 0) {
        return claim_rc;
    }

    if (alternate_setting != 0) {
        const int alt_rc = libusb_set_interface_alt_setting(
            handle_, interface_number, alternate_setting);
        if (alt_rc != 0) {
            libusb_release_interface(handle_, interface_number);
            return alt_rc;
        }
    }

    claimed_interfaces_.push_back(interface_number);
    return interface_number;
}

int LibusbDevice::bulk_read(std::uint8_t endpoint_address,
                            unsigned char* data,
                            int length,
                            int* transferred,
                            unsigned int timeout_ms) {
    if (handle_ == nullptr) {
        return LIBUSB_ERROR_NO_DEVICE;
    }
    if (data == nullptr || transferred == nullptr || length <= 0 ||
        (endpoint_address & LIBUSB_ENDPOINT_DIR_MASK) != LIBUSB_ENDPOINT_IN) {
        return LIBUSB_ERROR_INVALID_PARAM;
    }
    return libusb_bulk_transfer(handle_, endpoint_address, data, length,
                                transferred, timeout_ms);
}

UsbFunctionSnapshot LibusbFunctionClaim::snapshot() const {
    UsbFunctionSnapshot result{};
    libusb_device* dev = device_.device();
    if (dev == nullptr) return result;
    result.bus = libusb_get_bus_number(dev);
    result.address = libusb_get_device_address(dev);
    libusb_device_descriptor descriptor{};
    if (libusb_get_device_descriptor(dev, &descriptor) != 0) return result;
    result.vendor_id = descriptor.idVendor;
    result.product_id = descriptor.idProduct;

    std::uint8_t ports[8]{};
    const int port_count = libusb_get_port_numbers(dev, ports,
                                                   static_cast<int>(sizeof(ports)));
    if (port_count > 0) result.port_path.assign(ports, ports + port_count);

    libusb_config_descriptor* config = nullptr;
    if (libusb_get_active_config_descriptor(dev, &config) != 0 || config == nullptr)
        return result;
    const libusb_interface_descriptor* interface0 = nullptr;
    bool has_only_alt0 = true;
    for (std::uint8_t i = 0; i < config->bNumInterfaces; ++i) {
        const libusb_interface& iface = config->interface[i];
        for (int a = 0; a < iface.num_altsetting; ++a) {
            const auto& alt = iface.altsetting[a];
            if (alt.bInterfaceNumber != 0) continue;
            if (alt.bAlternateSetting == 0) interface0 = &alt;
            else has_only_alt0 = false;
        }
    }
    if (interface0 != nullptr) {
        result.interface0_present = true;
        // With exactly one alternate setting (alt 0), the claimed interface
        // cannot be using a nonzero alternate without changing configuration.
        result.active_alt0 = has_only_alt0 ? 0 : -1;
        for (std::uint8_t e = 0; e < interface0->bNumEndpoints; ++e) {
            if (interface0->endpoint[e].bEndpointAddress == 0x82U) {
                result.endpoint82_in_alt0 = true;
                result.endpoint82_bulk_in_alt0 =
                    (interface0->endpoint[e].bmAttributes &
                     LIBUSB_TRANSFER_TYPE_MASK) == LIBUSB_TRANSFER_TYPE_BULK;
            }
        }
    }
    libusb_free_config_descriptor(config);
    const int kernel = device_.kernel_driver_active(0);
    result.kernel_driver_state_known = kernel >= 0;
    result.interface0_kernel_driver = kernel > 0;
    return result;
}

int LibusbFunctionClaim::claim_interface0() {
    return device_.claim_interface(0);
}

int LibusbFunctionClaim::release_interface0() noexcept {
    return device_.release_interface(0);
}

}  // namespace asicen
