// SPDX-License-Identifier: GPL-2.0-only
#include <libusb.h>

#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

bool parse_fd(const char* text, int* fd) noexcept
{
    if (text == nullptr || fd == nullptr || *text == '\0') {
        return false;
    }
    int value = -1;
    const char* end = text;
    while (*end != '\0') {
        ++end;
    }
    const auto parsed = std::from_chars(text, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end || value < 0) {
        return false;
    }
    *fd = value;
    return true;
}

void report_error(const char* operation, int rc) noexcept
{
    std::fprintf(stderr, "%s: %s (%d)\n", operation, libusb_error_name(rc), rc);
}

int init_no_device_discovery(libusb_context** context) noexcept
{
#if LIBUSB_API_VERSION >= 0x0100010A
    libusb_init_option option{};
    option.option = LIBUSB_OPTION_NO_DEVICE_DISCOVERY;
    option.value.ival = 1;
    return libusb_init_context(context, &option, 1);
#else
    const int rc = libusb_set_option(nullptr, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
    return rc == LIBUSB_SUCCESS ? libusb_init(context) : rc;
#endif
}

} // namespace

int main(int argc, char** argv)
{
    int fd = -1;
    if (argc != 2 || !parse_fd(argc == 2 ? argv[1] : nullptr, &fd)) {
        std::fprintf(stderr, "usage: termux-fd-info-probe FD\n");
        return 2;
    }

    libusb_context* context = nullptr;
    int rc = init_no_device_discovery(&context);
    if (rc != LIBUSB_SUCCESS) {
        report_error("libusb_init", rc);
        return 1;
    }

    libusb_device_handle* handle = nullptr;
    rc = libusb_wrap_sys_device(context, static_cast<intptr_t>(fd), &handle);
    if (rc != LIBUSB_SUCCESS) {
        report_error("libusb_wrap_sys_device", rc);
        libusb_exit(context);
        return 1;
    }

    libusb_device* device = libusb_get_device(handle);
    libusb_device_descriptor descriptor{};
    rc = libusb_get_device_descriptor(device, &descriptor);
    if (rc != LIBUSB_SUCCESS) {
        report_error("libusb_get_device_descriptor", rc);
        libusb_close(handle);
        libusb_exit(context);
        return 1;
    }

    std::array<std::uint8_t, 8> ports{};
    const int port_count = libusb_get_port_numbers(
        device, ports.data(), static_cast<int>(ports.size()));
    std::printf("fd=%d bus=%u vid=%04x pid=%04x bcd=%04x device-class=%u/%u/%u configurations=%u\n",
                fd, libusb_get_bus_number(device), descriptor.idVendor, descriptor.idProduct,
                descriptor.bcdDevice, descriptor.bDeviceClass, descriptor.bDeviceSubClass,
                descriptor.bDeviceProtocol, descriptor.bNumConfigurations);
    if (port_count < 0) {
        report_error("libusb_get_port_numbers", port_count);
    } else {
        std::printf("port-count=%d path=", port_count);
        for (int i = 0; i < port_count; ++i) {
            std::printf("%s%u", i == 0 ? "" : ".", ports[static_cast<std::size_t>(i)]);
        }
        std::printf("\n");
    }

    if (descriptor.bNumConfigurations > 0) {
        libusb_config_descriptor* config = nullptr;
        rc = libusb_get_config_descriptor(device, 0, &config);
        if (rc == LIBUSB_SUCCESS) {
            std::printf("config-index=0 value=%u interfaces=%u max-power=%u\n",
                        config->bConfigurationValue, config->bNumInterfaces,
                        config->MaxPower);
            libusb_free_config_descriptor(config);
        } else {
            report_error("libusb_get_config_descriptor", rc);
        }
    }

    libusb_close(handle);
    libusb_exit(context);
    return port_count < 0 ? 1 : 0;
}
