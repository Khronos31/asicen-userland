#include <libusb.h>

#include <array>
#include <cctype>
#include <charconv>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "asicen/device_profile.h"
#include "asicen/diagnostic_probe.h"
#include "asicen/firmware.h"
#include "asicen/libusb_transport.h"
#include "asicen/loader_firmware.h"
#include "asicen/protocol.h"

namespace {

struct Arguments {
    bool help = false;
    std::string command = "list";
    bool have_command = false;
    bool claim = false;
    bool initialize = false;
    bool require_cold = false;
    bool have_device = false;
    asicen::UsbLocation location{};
    std::string firmware;
    bool have_firmware = false;
    const asicen::DeviceProfile* firmware_model = nullptr;
    std::string error;
};

void usage(const char* argv0, std::ostream& output = std::cerr)
{
    output << "usage:\n"
           << "  " << argv0 << " list\n"
           << "  " << argv0 << " --device BUS:ADDRESS describe\n"
           << "  " << argv0 << " --device BUS:ADDRESS --claim\n"
           << "  " << argv0 << " --device BUS:ADDRESS high-speed\n"
           << "  " << argv0 << " --device BUS:ADDRESS customer-info\n"
           << "  " << argv0 << " --device BUS:ADDRESS random-key\n"
           << "  " << argv0 << " --device BUS:ADDRESS --model MODEL --firmware PATH load-firmware\n"
           << "  " << argv0
           << " --device BUS:ADDRESS --model MODEL --firmware PATH --initialize [--require-cold]\n"
           << "  " << argv0 << " card-probe --help\n";
}

bool parse_u8(const std::string& value, std::uint8_t* out)
{
    if (out == nullptr || value.empty()) {
        return false;
    }
    unsigned parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || parsed > 255) {
        return false;
    }
    *out = static_cast<std::uint8_t>(parsed);
    return true;
}

bool parse_location(const std::string& value, asicen::UsbLocation* out)
{
    const std::size_t colon = value.find(':');
    if (colon == std::string::npos || value.find(':', colon + 1) != std::string::npos) {
        return false;
    }
    return parse_u8(value.substr(0, colon), &out->bus) &&
           parse_u8(value.substr(colon + 1), &out->address);
}

bool parse_arguments(int argc, char** argv, Arguments* out)
{
    if (argc < 1 || argv == nullptr || out == nullptr)
        return false;
    for (int i = 1; i < argc; ++i) {
        if (argv[i] == nullptr)
            return false;
        const std::string arg = argv[i];
        if (arg == "--help") {
            if (argc != 2) {
                out->error = "--help cannot be combined with other arguments";
                return false;
            }
            out->help = true;
            return true;
        }
        if (i + 1 < argc && argv[i + 1] == nullptr)
            return false;
        if (arg == "--device") {
            if (out->have_device) {
                out->error = "duplicate --device";
                return false;
            }
            if (++i >= argc || !parse_location(argv[i], &out->location) || out->location.bus == 0 ||
                out->location.address == 0) {
                return false;
            }
            out->have_device = true;
        } else if (arg == "--model") {
            if (out->firmware_model != nullptr) {
                out->error = "duplicate --model";
                return false;
            }
            if (++i >= argc)
                return false;
            out->firmware_model = asicen::find_profile_by_model(argv[i]);
            if (out->firmware_model == nullptr)
                return false;
        } else if (arg == "--firmware") {
            if (out->have_firmware) {
                out->error = "duplicate --firmware";
                return false;
            }
            if (++i >= argc || argv[i][0] == '\0') {
                return false;
            }
            out->firmware = argv[i];
            out->have_firmware = true;
        } else if (arg == "--claim") {
            if (out->claim) {
                out->error = "duplicate --claim";
                return false;
            }
            out->claim = true;
        } else if (arg == "--initialize") {
            if (out->initialize) {
                out->error = "duplicate --initialize";
                return false;
            }
            out->initialize = true;
        } else if (arg == "--require-cold") {
            if (out->require_cold) {
                out->error = "duplicate --require-cold";
                return false;
            }
            out->require_cold = true;
        } else if (arg == "list" || arg == "describe" || arg == "high-speed" ||
                   arg == "customer-info" || arg == "random-key" || arg == "load-firmware") {
            if (out->have_command) {
                out->error = "only one command is allowed";
                return false;
            }
            out->command = arg;
            out->have_command = true;
        } else {
            return false;
        }
    }

    if (out->initialize) {
        if (out->have_command || out->claim) {
            out->error = "--initialize cannot be combined with a command or --claim";
            return false;
        }
        out->command = "load-firmware";
    }
    if (out->require_cold && out->command != "load-firmware") {
        out->error = "--require-cold requires --initialize or load-firmware";
        return false;
    }
    if (out->command == "load-firmware" &&
        (!out->have_firmware || out->firmware_model == nullptr)) {
        return false;
    }
    if (out->command != "load-firmware" && (out->have_firmware || out->firmware_model != nullptr)) {
        out->error = "--model and --firmware are only valid for load-firmware";
        return false;
    }
    if (out->claim) {
        if (out->have_command && out->command != "describe") {
            out->error = "--claim cannot be combined with this command";
            return false;
        }
        if (!out->have_command)
            out->command = "claim";
    }
    if (out->command == "list" && out->have_device) {
        out->error = "--device requires a device command or --claim";
        return false;
    }
    return out->command == "list" || out->have_device;
}

std::string port_path(libusb_device* device)
{
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

bool is_loader(const libusb_device_descriptor& desc)
{
    return desc.idVendor == 0x1738 && (desc.idProduct == 0x5211 || desc.idProduct == 0x5216);
}

int list_devices(libusb_context* context)
{
    libusb_device** list = nullptr;
    const ssize_t count = libusb_get_device_list(context, &list);
    if (count < 0) {
        std::cerr << "libusb_get_device_list: " << libusb_error_name(static_cast<int>(count))
                  << '\n';
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

        std::cout << std::hex << std::setfill('0') << "usb=" << std::setw(4) << desc.idVendor << ':'
                  << std::setw(4) << desc.idProduct << std::dec
                  << " bus=" << static_cast<unsigned>(libusb_get_bus_number(list[i]))
                  << " address=" << static_cast<unsigned>(libusb_get_device_address(list[i]))
                  << " port=" << port_path(list[i]);

        if (profile != nullptr) {
            std::cout << " model=\"" << profile->model << "\""
                      << " runtime="
                      << (asicen::profile_runtime_supported(*profile) ? "source-backed-experimental"
                                                                      : "not-enabled")
                      << " enclosure_receivers="
                      << static_cast<unsigned>(profile->enclosure_receiver_count)
                      << " expected_functions="
                      << static_cast<unsigned>(profile->expected_runtime_functions)
                      << " local_lanes=" << static_cast<unsigned>(profile->local_lane_count)
                      << " status=runtime";
        } else {
            std::cout << " model=\"ASICEN firmware loader\" status=loader";
        }
        std::cout << '\n';
    }

    libusb_free_device_list(list, 1);
    return 0;
}

int describe_device(libusb_device* device)
{
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
              << " device_protocol=" << static_cast<unsigned>(desc.bDeviceProtocol) << '\n';

    for (std::uint8_t config_index = 0; config_index < desc.bNumConfigurations; ++config_index) {
        libusb_config_descriptor* config = nullptr;
        rc = libusb_get_config_descriptor(device, config_index, &config);
        if (rc != 0 || config == nullptr) {
            std::cerr << "config " << static_cast<unsigned>(config_index) << ": "
                      << libusb_error_name(rc) << '\n';
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
                          << " endpoints=" << static_cast<unsigned>(alt.bNumEndpoints) << '\n';

                for (std::uint8_t ep_index = 0; ep_index < alt.bNumEndpoints; ++ep_index) {
                    const libusb_endpoint_descriptor& ep = alt.endpoint[ep_index];
                    std::cout << "  endpoint=0x" << std::hex
                              << static_cast<unsigned>(ep.bEndpointAddress) << " attributes=0x"
                              << static_cast<unsigned>(ep.bmAttributes) << std::dec
                              << " max_packet=" << ep.wMaxPacketSize
                              << " interval=" << static_cast<unsigned>(ep.bInterval) << '\n';
                }
            }
        }

        libusb_free_config_descriptor(config);
    }

    return 0;
}

bool runtime_profile(asicen::LibusbDevice* device, const asicen::DeviceProfile** profile)
{
    libusb_device_descriptor desc{};
    if (device == nullptr || device->device() == nullptr ||
        device->descriptor(&desc) != 0) {
        return false;
    }
    *profile = asicen::find_profile(desc.idVendor, desc.idProduct);
    return *profile != nullptr;
}

int exact_in(asicen::LibusbDevice* device, asicen::Request request, unsigned char* data,
             std::uint16_t length)
{
    const asicen::ControlTransfer transfer{
        0, request, 0, 0, length, asicen::Direction::In, 1000,
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

std::string fixed_ascii(const std::uint8_t* data, std::size_t size)
{
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

template <std::size_t N> std::string fixed_ascii(const std::array<std::uint8_t, N>& data)
{
    return fixed_ascii(data.data(), data.size());
}

template <std::size_t N> void print_hex(const std::array<std::uint8_t, N>& data)
{
    std::cout << std::hex << std::setfill('0');
    for (std::uint8_t value : data) {
        std::cout << std::setw(2) << static_cast<unsigned>(value);
    }
    std::cout << std::dec;
}

int run_runtime_command(asicen::LibusbDevice* device, const std::string& command)
{
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

        std::cout << "use=" << static_cast<unsigned>(info.use_customer_info) << " vid_bytes=";
        print_hex(info.vid);
        std::cout << " pid_bytes=";
        print_hex(info.pid);
        std::cout << " manufacturer=\"" << fixed_ascii(info.manufacturer) << "\""
                  << " product=\"" << fixed_ascii(info.product) << "\""
                  << " hid=\"" << fixed_ascii(info.hid) << "\""
                  << " remote=" << static_cast<unsigned>(info.remote_control_number)
                  << " support_feature=0x" << std::hex
                  << static_cast<unsigned>(info.support_feature) << std::dec << " raw=";
        print_hex(data);
        std::cout << '\n';
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

bool loader_interface_ready(libusb_device* device)
{
    libusb_config_descriptor* config = nullptr;
    const int rc = libusb_get_active_config_descriptor(device, &config);
    if (rc != 0 || config == nullptr) {
        return false;
    }

    bool ready = false;
    for (int i = 0; i < config->bNumInterfaces && !ready; ++i) {
        const libusb_interface& interface = config->interface[i];
        for (int a = 0; a < interface.num_altsetting && !ready; ++a) {
            const libusb_interface_descriptor& alt = interface.altsetting[a];
            if (alt.bInterfaceNumber != 0 || alt.bAlternateSetting != 0) {
                continue;
            }
            for (std::uint8_t e = 0; e < alt.bNumEndpoints; ++e) {
                const libusb_endpoint_descriptor& ep = alt.endpoint[e];
                if (ep.bEndpointAddress == 0x01 &&
                    (ep.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) == LIBUSB_TRANSFER_TYPE_BULK &&
                    ep.wMaxPacketSize == 512) {
                    ready = true;
                    break;
                }
            }
        }
    }

    libusb_free_config_descriptor(config);
    return ready;
}

int load_firmware(asicen::LibusbDevice* device, const asicen::FirmwareImage& firmware,
                  asicen::ModelId model)
{
    libusb_device* raw = device->device();
    if (raw == nullptr) {
        std::cerr << "selected device is not available\n";
        return 1;
    }

    libusb_device_descriptor desc{};
    const int desc_rc = device->descriptor(&desc);
    if (desc_rc != 0) {
        std::cerr << "libusb_get_device_descriptor: " << libusb_error_name(desc_rc) << '\n';
        return 1;
    }
    if (!is_loader(desc) || (model == asicen::ModelId::W3u3V2 && desc.idProduct != 0x5211U)) {
        std::cerr << "selected device is not an ASICEN firmware loader\n";
        return 1;
    }
    if (!loader_interface_ready(raw)) {
        std::cerr << "loader interface 0 alt 0 with bulk OUT 0x01 "
                     "(512-byte packets) not present\n";
        return 1;
    }

    const int driver = device->kernel_driver_active(0);
    if (driver > 0) {
        std::cerr << "refusing to load: kernel driver bound to interface 0\n";
        return 1;
    }
    if (driver < 0 && driver != LIBUSB_ERROR_NOT_SUPPORTED) {
        std::cerr << "kernel_driver_active: " << libusb_error_name(driver) << '\n';
        return 1;
    }

    const int claim = device->claim_interface(0);
    if (claim < 0) {
        std::cerr << "claim interface 0: " << libusb_error_name(claim) << '\n';
        return 1;
    }

    const std::vector<asicen::LoaderTransfer> plan = asicen::build_loader_transfer_plan(model);
    for (std::size_t i = 0; i < plan.size(); ++i) {
        const asicen::LoaderTransfer& transfer = plan[i];
        const bool final_transfer = (i + 1 == plan.size());
        const int rc =
            device->vendor_out(transfer.request, transfer.value, transfer.index,
                               firmware.data() + transfer.blob_offset, transfer.length, 1000);

        std::cout << "transfer=" << (i + 1) << " request=0x" << std::hex << std::setw(2)
                  << std::setfill('0') << static_cast<unsigned>(transfer.request) << " value=0x"
                  << std::setw(4) << transfer.value << " index=0x" << transfer.index
                  << " length=" << std::dec << transfer.length << " returned=" << rc << '\n';

        if (rc == LIBUSB_ERROR_NO_DEVICE && final_transfer) {
            std::cout << "final AC transfer returned NO_DEVICE; firmware "
                         "acceptance is ambiguous, not success. "
                         "Inspect enumeration.\n";
            return 1;
        }
        if (rc < 0) {
            std::cerr << "control transfer failed: " << libusb_error_name(rc) << '\n';
            return 1;
        }
        if (rc != transfer.length) {
            std::cerr << "short control transfer: expected=" << transfer.length << " actual=" << rc
                      << '\n';
            return 1;
        }
    }

    std::cout << "sent all " << plan.size()
              << " loader transfers; inspect enumeration before use\n";
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc > 1 && std::string(argv[1]) == "card-probe") {
        return asicen::run_diagnostic_probe(asicen::DiagnosticProbeRole::card, argc - 1,
                                            const_cast<const char* const*>(argv + 1));
    }
    Arguments args{};
    if (!parse_arguments(argc, argv, &args)) {
        if (!args.error.empty())
            std::cerr << args.error << '\n';
        usage(argv[0]);
        return 2;
    }
    if (args.help) {
        usage(argv[0], std::cout);
        std::cout.flush();
        return std::cout ? 0 : 8;
    }
    if (std::signal(SIGPIPE, SIG_IGN) == SIG_ERR)
        return 10;

    auto firmware = px4::userland::Result<asicen::FirmwareImage>::failure(
        px4::userland::Error::NOT_READY);
    if (args.command == "load-firmware") {
        asicen::FirmwareProvider provider(args.firmware, args.firmware_model->model_id);
        firmware = provider.load();
        if (!firmware) {
            std::cerr << "firmware load failed: "
                      << px4::userland::error_string(firmware.error()) << '\n';
            switch (firmware.error()) {
            case px4::userland::Error::INVALID_ARGUMENT:
                return 2;
            case px4::userland::Error::NOT_FOUND:
                return 3;
            case px4::userland::Error::FIRMWARE_REJECTED:
                return 10;
            default:
                return 70;
            }
        }
        std::cerr << "verified firmware for " << args.firmware_model->model
                  << "; runtime identity must be rechecked after re-enumeration\n";
    }

    asicen::LibusbContext context_owner;
    const int init_rc = context_owner.initialize();
    libusb_context* context = context_owner.get();
    if (init_rc != 0) {
        std::cerr << "libusb_init: " << libusb_error_name(init_rc) << '\n';
        return 1;
    }

    if (args.command == "list") {
        const int rc = list_devices(context);

        std::cout.flush();
        std::cerr.flush();
        return rc != 0 ? rc : (!std::cout || !std::cerr ? 8 : 0);
    }

    asicen::LibusbDevice device;
    const int open_rc = device.open(context, args.location);
    if (open_rc != 0) {
        std::cerr << "open " << static_cast<unsigned>(args.location.bus) << ':'
                  << static_cast<unsigned>(args.location.address) << ": "
                  << libusb_error_name(open_rc) << '\n';

        return 1;
    }

    int result = 1;
    if (args.claim) {
        const asicen::DeviceProfile* profile = nullptr;
        if (!runtime_profile(&device, &profile)) {
            std::cerr << "selected device is not a supported ASICEN runtime device\n";
        } else if (device.claim_interface(0) < 0) {
            std::cerr << "claim interface 0 failed\n";
        } else {
            result = args.command == "describe" ? describe_device(device.device()) : 0;
        }
    } else if (args.command == "load-firmware") {
        const asicen::DeviceProfile* profile = nullptr;
        if (args.initialize && !args.require_cold && runtime_profile(&device, &profile)) {
            if (profile->model_id != args.firmware_model->model_id) {
                std::cerr << "runtime model does not match --model\n";
                result = 4;
            } else {
                std::cout << "already runtime; no firmware upload performed\n";
                result = 0;
            }
        } else {
            result = load_firmware(&device, firmware.value(), args.firmware_model->model_id);
        }
    } else if (args.command == "describe") {
        result = describe_device(device.device());
    } else {
        result = run_runtime_command(&device, args.command);
    }

    device.close();

    std::cout.flush();
    std::cerr.flush();
    if (result == 0 && (!std::cout || !std::cerr))
        result = 8;
    return result;
}
