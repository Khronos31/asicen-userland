#include <libusb.h>

#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

#include "asicen/device_profile.h"
#include "asicen/libusb_transport.h"
#include "asicen/protocol.h"

namespace {

struct Arguments {
    std::string command = "list";
    bool have_device = false;
    asicen::UsbLocation location{};
};

void usage(const char* argv0) {
    std::cerr
        << "usage:\n"
        << "  " << argv0 << " list\n"
        << "  " << argv0 << " --device BUS:ADDRESS describe\n"
        << "  " << argv0 << " --device BUS:ADDRESS high-speed\n"
        << "  " << argv0 << " --device BUS:ADDRESS customer-info\n"
        << "  " << argv0 << " --device BUS:ADDRESS random-key\n";
}

bool parse_u8(const std::string& value, std::uint8_t* out) {
    if (out == nullptr || value.empty()) {
        return false;
    }
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value.c_str(), &end, 0);
    if (end == nullptr || *end != '\0' || parsed > 255) {
        return false;
    }
    *out = static_cast<std::uint8_t>(parsed);
    return true;
}

bool parse_location(const std::string& value, asicen::UsbLocation* out) {
    const std::size_t colon = value.find(':');
    if (colon == std::string::npos || value.find(':', colon + 1) != std::string::npos) {
        return false;
    }
    return parse_u8(value.substr(0, colon), &out->bus) &&
           parse_u8(value.substr(colon + 1), &out->address);
}

bool parse_arguments(int argc, char** argv, Arguments* out) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--device") {
            if (++i >= argc || !parse_location(argv[i], &out->location)) {
                return false;
            }
            out->have_device = true;
        } else if (arg == "list" || arg == "describe" || arg == "high-speed" ||
                   arg == "customer-info" || arg == "random-key") {
            out->command = arg;
        } else if (arg == "-h" || arg == "--help") {
            usage(argv[0]);
            std::exit(0);
        } else {
            return false;
        }
    }

    return out->command == "list" || out->have_device;
}

std::string port_path(libusb_device* device) {
    std::uint8_t ports[8]{};
    const int count = libusb_get_port_numbers(device, ports, sizeof(ports));
    if (count <= 0) {
        return "-";
    }

    std::ostringstream out;
    out << static_cast<unsigned>(libusb_get_bus_number(device)) << '-';
    for (int i = 0; i < count; ++i) {
        if (i != 0) {
            out << '.';
        }
        out << static_cast<unsigned>(ports[i]);
    }
    return out.str();
}

bool is_loader(const libusb_device_descriptor& desc) {
    return desc.idVendor == 0x1738 &&
           (desc.idProduct == 0x5211 || desc.idProduct == 0x5216);
}

int list_devices(libusb_context* context) {
    libusb_device** list = nullptr;
    const ssize_t count = libusb_get_device_list(context, &list);
    if (count < 0) {
        std::cerr << "libusb_get_device_list: " << libusb_error_name(static_cast<int>(count)) << '\n';
        return 1;
    }

    for (ssize_t i = 0; i < count; ++i) {
        libusb_device_descriptor desc{};
        if (libusb_get_device_descriptor(list[i], &desc) != 0) {
            continue;
        }

        const auto* profile = asicen::find_profile(desc.idVendor, desc.idProduct);
        const bool loader = is_loader(desc);
        if (profile == nullptr && !loader) {
            continue;
        }

        std::cout << std::hex << std::setfill('0')
                  << "usb=" << std::setw(4) << desc.idVendor << ':'
                  << std::setw(4) << desc.idProduct << std::dec
                  << " bus=" << static_cast<unsigned>(libusb_get_bus_number(list[i]))
                  << " address=" << static_cast<unsigned>(libusb_get_device_address(list[i]))
                  << " port=" << port_path(list[i]);

        if (profile != nullptr) {
            std::cout << " model=\"" << profile->model << "\""
                      << " receivers=" << static_cast<unsigned>(profile->receiver_count)
                      << " status=runtime";
        } else {
            std::cout << " model=\"ASICEN firmware loader\" status=loader";
        }
        std::cout << '\n';
    }

    libusb_free_device_list(list, 1);
    return 0;
}

int describe_device(libusb_device* device) {
    if (device == nullptr) {
        return 1;
    }

    libusb_device_descriptor desc{};
    int rc = libusb_get_device_descriptor(device, &desc);
    if (rc != 0) {
        std::cerr << "libusb_get_device_descriptor: " << libusb_error_name(rc) << '\n';
        return 1;
    }

    std::cout << "configurations=" << static_cast<unsigned>(desc.bNumConfigurations)
              << " device_class=" << static_cast<unsigned>(desc.bDeviceClass)
              << " device_subclass=" << static_cast<unsigned>(desc.bDeviceSubClass)
              << " device_protocol=" << static_cast<unsigned>(desc.bDeviceProtocol)
              << '\n';

    for (std::uint8_t config_index = 0; config_index < desc.bNumConfigurations; ++config_index) {
        libusb_config_descriptor* config = nullptr;
        rc = libusb_get_config_descriptor(device, config_index, &config);
        if (rc != 0 || config == nullptr) {
            std::cerr << "config " << static_cast<unsigned>(config_index)
                      << ": " << libusb_error_name(rc) << '\n';
            continue;
        }

        std::cout << "config=" << static_cast<unsigned>(config->bConfigurationValue)
                  << " interfaces=" << static_cast<unsigned>(config->bNumInterfaces)
                  << " attributes=0x" << std::hex << static_cast<unsigned>(config->bmAttributes)
                  << std::dec << " max_power_ma=" << static_cast<unsigned>(config->MaxPower) * 2U
                  << '\n';

        for (int interface_index = 0; interface_index < config->bNumInterfaces; ++interface_index) {
            const libusb_interface& interface = config->interface[interface_index];
            for (int alt_index = 0; alt_index < interface.num_altsetting; ++alt_index) {
                const libusb_interface_descriptor& alt = interface.altsetting[alt_index];
                std::cout << "interface=" << static_cast<unsigned>(alt.bInterfaceNumber)
                          << " alt=" << static_cast<unsigned>(alt.bAlternateSetting)
                          << " class=" << static_cast<unsigned>(alt.bInterfaceClass)
                          << " subclass=" << static_cast<unsigned>(alt.bInterfaceSubClass)
                          << " protocol=" << static_cast<unsigned>(alt.bInterfaceProtocol)
                          << " endpoints=" << static_cast<unsigned>(alt.bNumEndpoints)
                          << '\n';

                for (std::uint8_t ep_index = 0; ep_index < alt.bNumEndpoints; ++ep_index) {
                    const libusb_endpoint_descriptor& ep = alt.endpoint[ep_index];
                    std::cout << "  endpoint=0x" << std::hex
                              << static_cast<unsigned>(ep.bEndpointAddress)
                              << " attributes=0x" << static_cast<unsigned>(ep.bmAttributes)
                              << std::dec
                              << " max_packet=" << ep.wMaxPacketSize
                              << " interval=" << static_cast<unsigned>(ep.bInterval)
                              << '\n';
                }
            }
        }

        libusb_free_config_descriptor(config);
    }

    return 0;
}

bool runtime_profile(asicen::LibusbDevice* device, const asicen::DeviceProfile** profile) {
    libusb_device_descriptor desc{};
    if (device == nullptr || device->device() == nullptr ||
        libusb_get_device_descriptor(device->device(), &desc) != 0) {
        return false;
    }
    *profile = asicen::find_profile(desc.idVendor, desc.idProduct);
    return *profile != nullptr;
}

int exact_in(asicen::LibusbDevice* device, asicen::Request request,
             unsigned char* data, std::uint16_t length) {
    const asicen::ControlTransfer transfer{
        0,
        request,
        0,
        0,
        length,
        asicen::Direction::In,
        1000,
    };
    const int rc = device->control(transfer, data);
    if (rc < 0) {
        std::cerr << "control transfer: " << libusb_error_name(rc) << '\n';
        return 1;
    }
    if (rc != length) {
        std::cerr << "short control transfer: expected=" << length << " actual=" << rc << '\n';
        return 1;
    }
    return 0;
}

std::string fixed_ascii(const std::uint8_t* data, std::size_t size) {
    std::string result;
    for (std::size_t i = 0; i < size; ++i) {
        const unsigned char value = data[i];
        if (value == 0 || value == 0xff) {
            break;
        }
        result.push_back(std::isprint(value) != 0 ? static_cast<char>(value) : '.');
    }
    while (!result.empty() && result.back() == ' ') {
        result.pop_back();
    }
    return result;
}

template <std::size_t N>
std::string fixed_ascii(const std::array<std::uint8_t, N>& data) {
    return fixed_ascii(data.data(), data.size());
}

template <std::size_t N>
void print_hex(const std::array<std::uint8_t, N>& data) {
    std::cout << std::hex << std::setfill('0');
    for (std::uint8_t value : data) {
        std::cout << std::setw(2) << static_cast<unsigned>(value);
    }
    std::cout << std::dec;
}

int run_runtime_command(asicen::LibusbDevice* device, const std::string& command) {
    const asicen::DeviceProfile* profile = nullptr;
    if (!runtime_profile(device, &profile)) {
        std::cerr << "selected device is not a supported ASICEN runtime device\n";
        return 1;
    }

    std::cout << "model=\"" << profile->model << "\" ";

    if (command == "high-speed") {
        std::array<std::uint8_t, 1> data{};
        if (exact_in(device, asicen::Request::GetHighSpeed, data.data(), data.size()) != 0) {
            return 1;
        }
        std::cout << "high_speed=" << static_cast<unsigned>(data[0]) << '\n';
        return 0;
    }

    if (command == "customer-info") {
        std::array<std::uint8_t, asicen::kCustomerInfoSize> data{};
        if (exact_in(device, asicen::Request::CustomerInfo, data.data(), data.size()) != 0) {
            return 1;
        }

        asicen::CustomerInfo info{};
        if (!asicen::parse_customer_info(data.data(), data.size(), &info)) {
            std::cerr << "customer-info decode failed\n";
            return 1;
        }

        const std::uint16_t vid = static_cast<std::uint16_t>(info.vid[0]) |
                                  (static_cast<std::uint16_t>(info.vid[1]) << 8);
        const std::uint16_t pid = static_cast<std::uint16_t>(info.pid[0]) |
                                  (static_cast<std::uint16_t>(info.pid[1]) << 8);

        std::cout << "use=" << static_cast<unsigned>(info.use_customer_info)
                  << " vid=0x" << std::hex << std::setw(4) << std::setfill('0') << vid
                  << " pid=0x" << std::setw(4) << pid << std::dec
                  << " manufacturer=\"" << fixed_ascii(info.manufacturer) << "\""
                  << " product=\"" << fixed_ascii(info.product) << "\""
                  << " hid=\"" << fixed_ascii(info.hid) << "\""
                  << " remote=" << static_cast<unsigned>(info.remote_control_number)
                  << " support_feature=0x" << std::hex
                  << static_cast<unsigned>(info.support_feature) << std::dec
                  << '\n';
        return 0;
    }

    if (command == "random-key") {
        std::array<std::uint8_t, 16> data{};
        if (exact_in(device, asicen::Request::GetDeviceRandomKey, data.data(), data.size()) != 0) {
            return 1;
        }
        std::cout << "random_key=";
        print_hex(data);
        std::cout << '\n';
        return 0;
    }

    std::cerr << "unsupported runtime command\n";
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    Arguments args{};
    if (!parse_arguments(argc, argv, &args)) {
        usage(argv[0]);
        return 2;
    }

    libusb_context* context = nullptr;
    const int init_rc = libusb_init(&context);
    if (init_rc != 0) {
        std::cerr << "libusb_init: " << libusb_error_name(init_rc) << '\n';
        return 1;
    }

    if (args.command == "list") {
        const int rc = list_devices(context);
        libusb_exit(context);
        return rc;
    }

    asicen::LibusbDevice device;
    const int open_rc = device.open(context, args.location);
    if (open_rc != 0) {
        std::cerr << "open " << static_cast<unsigned>(args.location.bus) << ':'
                  << static_cast<unsigned>(args.location.address) << ": "
                  << libusb_error_name(open_rc) << '\n';
        libusb_exit(context);
        return 1;
    }

    int result = 1;
    if (args.command == "describe") {
        result = describe_device(device.device());
    } else {
        result = run_runtime_command(&device, args.command);
    }

    device.close();
    libusb_exit(context);
    return result;
}
