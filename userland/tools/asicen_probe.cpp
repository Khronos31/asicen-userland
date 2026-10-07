#include <libusb.h>

#include <cstdint>
#include <iomanip>
#include <iostream>

#include "asicen/device_profile.h"

int main() {
    libusb_context* ctx = nullptr;
    if (libusb_init(&ctx) != 0) {
        std::cerr << "libusb_init failed\n";
        return 1;
    }

    libusb_device** list = nullptr;
    const ssize_t count = libusb_get_device_list(ctx, &list);
    if (count < 0) {
        std::cerr << "libusb_get_device_list failed\n";
        libusb_exit(ctx);
        return 1;
    }

    for (ssize_t i = 0; i < count; ++i) {
        libusb_device_descriptor desc{};
        if (libusb_get_device_descriptor(list[i], &desc) != 0) {
            continue;
        }
        const auto* profile = asicen::find_profile(desc.idVendor, desc.idProduct);
        const bool loader = desc.idVendor == 0x1738 &&
                            (desc.idProduct == 0x5211 || desc.idProduct == 0x5216);
        if (!profile && !loader) {
            continue;
        }

        std::cout << std::hex << std::setfill('0')
                  << "usb=" << std::setw(4) << desc.idVendor << ':'
                  << std::setw(4) << desc.idProduct << std::dec
                  << " bus=" << static_cast<unsigned>(libusb_get_bus_number(list[i]))
                  << " address=" << static_cast<unsigned>(libusb_get_device_address(list[i]));
        if (profile) {
            std::cout << " model=\"" << profile->model << "\""
                      << " receivers=" << static_cast<unsigned>(profile->receiver_count);
        } else {
            std::cout << " model=\"ASICEN firmware loader\" status=loader";
        }
        std::cout << '\n';
    }

    libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    return 0;
}
