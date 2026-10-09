// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/hardware_stream_session.h"
#include "asicen/enclosure_lock.h"
#include "asicen/card_only_service.h"
#include "asicen/libusb_hardware_backend.h"
#include "asicen/libusb_transport.h"
#include "asicen/loader_firmware.h"
#include "asicen/product_profile.h"
#include "asicen/px4_mock_backend.h"
#include "px4/control_server.h"
#include "px4/posix_tuner_nonce.h"

#include <signal.h>
#include <unistd.h>
#include <libusb.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {
volatile sig_atomic_t stop_requested = 0;
void signal_handler(int) { stop_requested = 1; }

class Time final : public px4::userland::TunerServiceTime {
public:
    std::uint64_t monotonic_ms() noexcept override {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    void sleep_ms(std::uint32_t ms) noexcept override {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
};

struct Options {
    const asicen::DeviceProfile* expected_model = nullptr;
    std::array<std::string, 2U> usb_paths;
    std::size_t usb_path_count = 0U;
    std::array<int, 2U> file_descriptors{};
    std::size_t file_descriptor_count = 0U;
    std::string runtime_dir;
    std::string instance = "default";
    bool probe_satellite = false;
    bool probe_card = false;
    bool card_only = false;
    bool have_satellite_slot = false;
    std::uint32_t satellite_rf_khz = 0;
    std::uint8_t satellite_slot = 0;
    bool allow_lnb_power = false;
    bool group = false;
    std::string firmware_path;
};

bool parse_unsigned_decimal(const std::string& text, std::uint64_t maximum,
                            std::uint64_t* value) {
    if (value == nullptr || text.empty() ||
        !std::all_of(text.begin(), text.end(), [](unsigned char c) {
            return c >= '0' && c <= '9';
        })) return false;
    try {
        std::size_t consumed = 0;
        const unsigned long long parsed = std::stoull(text, &consumed, 10);
        if (consumed != text.size() || parsed > maximum) return false;
        *value = static_cast<std::uint64_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

// Splits "BUS-P1.P2..." into the bus and a non-empty port path. Returns false
// for "BUS:ADDR" address forms and for malformed port text.
bool parse_bus_port(const std::string& text, std::uint8_t* bus,
                    std::vector<std::uint8_t>* ports) {
    if (bus == nullptr || ports == nullptr || !asicen::parse_port_path(text)) {
        return false;
    }
    const auto dash = text.find('-');
    if (dash == std::string::npos) return false;
    std::uint64_t parsed_bus = 0U;
    if (!parse_unsigned_decimal(text.substr(0, dash), 255U, &parsed_bus)) return false;
    *bus = static_cast<std::uint8_t>(parsed_bus);
    std::size_t offset = dash + 1U;
    while (offset < text.size()) {
        const auto end = text.find('.', offset);
        const auto token = text.substr(offset, end == std::string::npos
                                                   ? std::string::npos : end - offset);
        std::uint64_t value = 0U;
        if (!parse_unsigned_decimal(token, 255U, &value)) return false;
        ports->push_back(static_cast<std::uint8_t>(value));
        if (end == std::string::npos) break;
        offset = end + 1U;
    }
    return !ports->empty();
}

// For a port-form USB path "BUS-P1.P2...", the address is resolved once the
// device is open; see resolve_device below, which handles both forms.

bool valid_instance(const std::string& value) {
    if (value.empty() || value.size() > 80U || value == "." || value == "..") return false;
    const bool serial_shaped = (value.size() == 14U || value.size() == 15U) &&
        std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return c >= '0' && c <= '9';
        });
    if (serial_shaped) return false;
    for (const unsigned char c : value) {
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') || c == '_' || c == '-' || c == '.') continue;
        return false;
    }
    return true;
}

bool parse_arguments(int argc, char** argv, Options* out) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--model" && i + 1 < argc) {
            out->expected_model = asicen::find_profile_by_model(argv[++i]);
            if (out->expected_model == nullptr) return false;
        }
        else if (arg == "--usb-path" && i + 1 < argc) {
            if (out->usb_path_count >= out->usb_paths.size()) return false;
            const std::string value(argv[++i]);
            const bool address_form =
                value.find(':') != std::string::npos;
            if (!address_form && !asicen::parse_port_path(value)) return false;
            for (std::size_t existing = 0U; existing < out->usb_path_count; ++existing)
                if (out->usb_paths[existing] == value) return false;
            out->usb_paths[out->usb_path_count++] = value;
        } else if (arg == "--fd" && i + 1 < argc) {
            if (out->file_descriptor_count >= out->file_descriptors.size()) return false;
            std::uint64_t parsed = 0;
            if (!parse_unsigned_decimal(argv[++i],
                    static_cast<std::uint64_t>(std::numeric_limits<int>::max()),
                    &parsed)) return false;
            for (std::size_t existing = 0U; existing < out->file_descriptor_count; ++existing)
                if (out->file_descriptors[existing] == static_cast<int>(parsed)) return false;
            out->file_descriptors[out->file_descriptor_count++] = static_cast<int>(parsed);
        } else if (arg == "--runtime-dir" && i + 1 < argc) {
            out->runtime_dir = argv[++i];
        } else if (arg == "--instance" && i + 1 < argc) {
            out->instance = argv[++i];
        } else if (arg == "--probe-satellite" && i + 1 < argc) {
            if (out->probe_satellite || out->probe_card) return false;
            std::uint64_t parsed = 0;
            if (!parse_unsigned_decimal(argv[++i],
                    std::numeric_limits<std::uint32_t>::max(), &parsed) ||
                !asicen::is_w3u3_satellite_rf_khz(static_cast<std::uint32_t>(parsed)))
                return false;
            out->probe_satellite = true;
            out->satellite_rf_khz = static_cast<std::uint32_t>(parsed);
        } else if (arg == "--probe-card") {
            if (out->probe_satellite || out->probe_card || out->card_only) return false;
            out->probe_card = true;
        } else if (arg == "--card-only") {
            if (out->probe_satellite || out->probe_card || out->card_only) return false;
            out->card_only = true;
        } else if (arg == "--slot" && i + 1 < argc) {
            if (out->have_satellite_slot) return false;
            std::uint64_t parsed = 0;
            if (!parse_unsigned_decimal(argv[++i],
                    asicen::kW3u3SatelliteTsidSlots - 1U, &parsed)) return false;
            out->have_satellite_slot = true;
            out->satellite_slot = static_cast<std::uint8_t>(parsed);
        } else if (arg == "--allow-lnb-power") {
            if (out->allow_lnb_power) return false;
            out->allow_lnb_power = true;
        } else if (arg == "--group") {
            if (out->group) return false;
            out->group = true;
        } else if (arg == "--firmware" && i + 1 < argc) {
            if (!out->firmware_path.empty()) return false;
            const std::string value(argv[++i]);
            if (value.empty()) return false;
            out->firmware_path = value;
        } else if (arg == "--help" || arg == "-h") {
            std::puts("usage: asicend --usb-path BUS:ADDRESS|BUS-PORT "
                      "[--usb-path BUS:ADDRESS|BUS-PORT] [--model MODEL]\n"
                      "                 [--runtime-dir PATH] [--instance TOKEN] "
                      "[--group] [--allow-lnb-power] [--firmware PATH]\n"
                      "       asicend --fd FD [--fd FD] [--model MODEL]\n"
                      "                 [--runtime-dir PATH] [--instance TOKEN] "
                      "[--group] [--allow-lnb-power] [--firmware PATH]\n"
                      "       asicend --usb-path BUS:ADDRESS|BUS-PORT "
                      "[--usb-path BUS:ADDRESS|BUS-PORT] [--model MODEL]\n"
                      "                 --probe-satellite RF_KHZ [--slot 0..7]\n"
                      "       asicend --usb-path BUS:ADDRESS|BUS-PORT "
                      "[--usb-path BUS:ADDRESS|BUS-PORT] --model MODEL --probe-card\n"
                      "       asicend --usb-path BUS:ADDRESS|BUS-PORT "
                      "[--usb-path BUS:ADDRESS|BUS-PORT] --model MODEL --card-only "
                      "[--runtime-dir PATH] [--instance TOKEN]\n"
                      "  --usb-path   native topology; up to two values, primary first then\n"
                      "               sibling. Each value is BUS:ADDRESS or BUS-PORT.\n"
                      "  --fd         granted USB descriptors; up to two, primary first.\n"
                      "  --allow-lnb-power  permit explicit ISDB-S 15 V requests; default off\n"
                      "  --firmware   loader image; loaded before claiming\n");
            std::exit(0);
        } else return false;
    }
    const bool fd_mode = out->file_descriptor_count != 0U;
    const bool path_mode = out->usb_path_count != 0U;
    if (out->runtime_dir.size() >= 400U ||
        !valid_instance(out->instance)) return false;
    if (fd_mode == path_mode) return false;
    return (!out->have_satellite_slot || out->probe_satellite) &&
           (!out->card_only || (!out->probe_satellite && !out->probe_card));
}

// Resolves one --usb-path value (BUS:ADDRESS or BUS-PORT) to the opened
// device's location and its actual topology. A port-form value returns the
// matching device on the bus; an address-form value returns that address plus
// its port path, so the canonical primary/sibling port ordering can be checked
// before the daemon claims the enclosure.
struct ResolvedDevice final {
    asicen::UsbLocation location{};
    std::vector<std::uint8_t> port_path;
};

px4::userland::Result<ResolvedDevice> resolve_device(
    libusb_context* context, const std::string& text) {
    std::uint8_t bus = 0U;
    std::uint8_t address = 0U;
    const bool address_form = asicen::parse_usb_location(text, &bus, &address);
    std::vector<std::uint8_t> wanted_port;
    if (!address_form && !parse_bus_port(text, &bus, &wanted_port)) {
        return px4::userland::Result<ResolvedDevice>::failure(
            px4::userland::Error::INVALID_ARGUMENT);
    }
    libusb_device** list = nullptr;
    const ssize_t count = libusb_get_device_list(context, &list);
    if (count < 0) return px4::userland::Result<ResolvedDevice>::failure(
        px4::userland::Error::USB_IO);
    ResolvedDevice matched{};
    bool found = false;
    bool any_on_bus = false;
    for (ssize_t i = 0; i < count; ++i) {
        libusb_device* device = list[i];
        if (static_cast<std::uint8_t>(libusb_get_bus_number(device)) != bus) continue;
        any_on_bus = true;
        std::uint8_t ports[8]{};
        const int path_count = libusb_get_port_numbers(
            device, ports, static_cast<int>(sizeof(ports)));
        if (path_count <= 0) continue;
        std::vector<std::uint8_t> port_path(ports, ports + path_count);
        const std::uint8_t device_address =
            static_cast<std::uint8_t>(libusb_get_device_address(device));
        if (address_form) {
            if (device_address != address) continue;
            matched.location = asicen::UsbLocation{bus, address};
            matched.port_path = std::move(port_path);
            found = true;
            break;
        }
        if (port_path == wanted_port) {
            matched.location = asicen::UsbLocation{bus, device_address};
            matched.port_path = std::move(port_path);
            found = true;
            break;
        }
    }
    libusb_free_device_list(list, 1);
    if (!found) {
        return px4::userland::Result<ResolvedDevice>::failure(
            any_on_bus ? px4::userland::Error::NOT_FOUND
                       : px4::userland::Error::NOT_READY);
    }
    return px4::userland::Result<ResolvedDevice>::success(std::move(matched));
}

int lock_runtime() {
    return asicen::acquire_enclosure_lock("/tmp/asicen-userland-enclosure.lock");
}

// Loads firmware into a loader device addressed by usb-path or fd. The
// caller asserts the resolved device is a loader (0x1738:0x5211/0x5216).
// Returns 0 on full transfer, 2 for argument problems, 70 on USB/firmware
// I/O failure, 3 when the addressed device is not a loader.
int load_loader_firmware(libusb_context* context, const Options& options,
                         const std::string& firmware_path) {
    const asicen::DeviceProfile* model = options.expected_model;
    if (model == nullptr) {
        std::fprintf(stderr, "--firmware requires --model to verify the loader image\n");
        return 2;
    }
    std::vector<std::uint8_t> firmware;
    const asicen::LoaderFirmwareRead read = asicen::read_verified_loader_firmware_file(
        firmware_path, model->model_id, &firmware);
    if (read != asicen::LoaderFirmwareRead::Ok) {
        std::fprintf(stderr, "firmware: %s\n", read == asicen::LoaderFirmwareRead::OpenFailed
            ? "cannot open file" : read == asicen::LoaderFirmwareRead::WrongSize
            ? "wrong size" : read == asicen::LoaderFirmwareRead::FingerprintMismatch
            ? "fingerprint mismatch" : "unsupported model");
        return 70;
    }
    asicen::LibusbDevice device;
    const char* opened = nullptr;
    if (options.file_descriptor_count != 0U) {
        opened = device.open(context, options.file_descriptors[0]) == 0 ? "fd" : nullptr;
    } else {
        asicen::UsbLocation location{};
        const auto resolved = resolve_device(context, options.usb_paths[0]);
        if (resolved) {
            location = resolved.value().location;
            opened = device.open(context, location) == 0 ? "usb-path" : nullptr;
        }
    }
    if (opened == nullptr) { std::fprintf(stderr, "firmware: cannot open addressed device\n"); return 3; }

    libusb_device_descriptor desc{};
    if (libusb_get_device_descriptor(device.device(), &desc) != 0) {
        std::fprintf(stderr, "firmware: descriptor read failed\n");
        return 70;
    }
    const bool loader = desc.idVendor == 0x1738 &&
        (desc.idProduct == 0x5211U || desc.idProduct == 0x5216U);
    if (!loader) {
        std::fprintf(stderr, "firmware: addressed device is not an ASICEN loader\n");
        return 3;
    }
    const int driver = device.kernel_driver_active(0);
    if (driver > 0) { std::fprintf(stderr, "firmware: interface 0 has a kernel driver\n"); return 70; }
    if (driver < 0 && driver != LIBUSB_ERROR_NOT_SUPPORTED) {
        std::fprintf(stderr, "firmware: kernel_driver_active: %s\n", libusb_error_name(driver));
        return 70;
    }
    const int claim = device.claim_interface(0);
    if (claim < 0) { std::fprintf(stderr, "firmware: claim interface 0: %s\n", libusb_error_name(claim)); return 70; }
    const std::vector<asicen::LoaderTransfer> plan =
        asicen::build_loader_transfer_plan(model->model_id);
    if (plan.empty()) { std::fprintf(stderr, "firmware: no transfer plan for model\n"); return 70; }
    for (std::size_t i = 0; i < plan.size(); ++i) {
        const asicen::LoaderTransfer& transfer = plan[i];
        const bool final_transfer = (i + 1 == plan.size());
        const int rc = device.vendor_out(transfer.request, transfer.value,
            transfer.index, firmware.data() + transfer.blob_offset,
            transfer.length, 1000);
        if (rc == LIBUSB_ERROR_NO_DEVICE && final_transfer) {
            std::fprintf(stderr, "firmware: final AC returned NO_DEVICE; acceptance ambiguous\n");
            return 70;
        }
        if (rc < 0) { std::fprintf(stderr, "firmware: control transfer failed: %s\n", libusb_error_name(rc)); return 70; }
        if (rc != transfer.length) { std::fprintf(stderr, "firmware: short transfer\n"); return 70; }
    }
    std::fprintf(stderr, "firmware: loaded %zu loader transfers; device will re-enumerate\n", plan.size());
    return 0;
}

// After a loader transfer the device re-enumerates as a runtime. Polls the
// topology until a supported runtime appears, returning the lowest observed
// bus/address or NOT_FOUND if no runtime shows up within the deadline.
px4::userland::Result<asicen::UsbLocation> wait_for_runtime(
    libusb_context* context,
    const volatile std::sig_atomic_t* stop_flag) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    asicen::UsbLocation primary_location{};
    bool first_observed = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (stop_flag != nullptr && *stop_flag != 0)
            return px4::userland::Result<asicen::UsbLocation>::failure(
                px4::userland::Error::NOT_READY);
        libusb_device** list = nullptr;
        const ssize_t count = libusb_get_device_list(context, &list);
        if (count >= 0) {
            for (ssize_t i = 0; i < count; ++i) {
                libusb_device* d = list[i];
                libusb_device_descriptor desc{};
                if (libusb_get_device_descriptor(d, &desc) != 0) continue;
                if (asicen::find_profile(desc.idVendor, desc.idProduct) == nullptr) continue;
                const std::uint8_t bus = static_cast<std::uint8_t>(libusb_get_bus_number(d));
                const std::uint8_t address = static_cast<std::uint8_t>(libusb_get_device_address(d));
                if (!first_observed) {
                    primary_location = asicen::UsbLocation{bus, address};
                    first_observed = true;
                } else {
                    // Prefer an address that reuses the original bus; the sibling
                    // follows the same topology, so the lowest bus/address pair is
                    // the earliest re-enumerated runtime.
                    if (bus < primary_location.bus ||
                        (bus == primary_location.bus && address < primary_location.address))
                        primary_location = asicen::UsbLocation{bus, address};
                }
            }
            libusb_free_device_list(list, 1);
        }
        if (first_observed) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (!first_observed) return px4::userland::Result<asicen::UsbLocation>::failure(
        px4::userland::Error::NOT_FOUND);
    return px4::userland::Result<asicen::UsbLocation>::success(primary_location);
}
}  // namespace

int run_asicend_hardware(int argc, char** argv) {
    Options options;
    if (!parse_arguments(argc, argv, &options)) {
        std::fprintf(stderr, "usage: asicend --usb-path BUS:ADDRESS|BUS-PORT "
                             "[--usb-path BUS:ADDRESS|BUS-PORT] [--model MODEL] [--runtime-dir PATH] "
                             "[--instance TOKEN] [--group] [--allow-lnb-power] [--firmware PATH]\n"
                             "       asicend --fd FD [--fd FD] [--model MODEL] "
                             "[--runtime-dir PATH] [--instance TOKEN] [--group] "
                             "[--allow-lnb-power] [--firmware PATH]\n");
        return 2;
    }
    stop_requested = 0;
    struct sigaction action{};
    action.sa_handler = signal_handler;
    sigemptyset(&action.sa_mask);
    ::sigaction(SIGINT, &action, nullptr);
    ::sigaction(SIGTERM, &action, nullptr);
    ::signal(SIGPIPE, SIG_IGN);
    const int lock_fd = lock_runtime();
    if (lock_fd < 0) {
        std::fprintf(stderr, "ASICEN enclosure is already owned or lock path is unsafe\n");
        return 4;
    }
    libusb_context* context = nullptr;
    const int init_rc = libusb_init(&context);
    if (init_rc != 0) {
        std::fprintf(stderr, "libusb_init: %s\n", libusb_error_name(init_rc));
        ::close(lock_fd); return 70;
    }
// When --firmware transferred a loader, the runtime re-enumerates at the
    // same topology but a different address. Hold the observed runtime so the
    // claim below prefers it over re-resolving a possibly-stale BUS:ADDRESS.
    bool have_firmware_runtime = false;
    asicen::UsbLocation firmware_runtime{};
    if (!options.firmware_path.empty()) {
        // px4-compatible --firmware: upload the verified loader image, then
        // wait for the runtime to re-enumerate at the same topology before
        // claiming. A non-loader addressed device is treated as a warm runtime
        // (firmware already present) and claim proceeds without an upload.
        const int loaded = load_loader_firmware(context, options, options.firmware_path);
        if (loaded == 3) {
            std::fprintf(stderr, "firmware: addressed device is already runtime; skipping upload\n");
        } else if (loaded != 0) {
            ::close(lock_fd); libusb_exit(context);
            return loaded;
        } else {
            const auto runtime = wait_for_runtime(context, &stop_requested);
            if (!runtime) {
                std::fprintf(stderr, "firmware: runtime did not re-enumerate: %s\n",
                             px4::userland::error_string(runtime.error()));
                ::close(lock_fd); libusb_exit(context);
                return runtime.error() == px4::userland::Error::NOT_FOUND ? 3 : 70;
            }
            have_firmware_runtime = true;
            firmware_runtime = runtime.value();
        }
    }
    int result = 70;
    {
        std::unique_ptr<asicen::LibusbW3u3Hardware> hardware;
        if (options.file_descriptor_count != 0U) {
            hardware = std::make_unique<asicen::LibusbW3u3Hardware>(
                context, options.file_descriptors[0],
                options.file_descriptor_count >= 2U ? options.file_descriptors[1] : -1,
                options.expected_model);
        } else if (have_firmware_runtime) {
            // The primary runtime re-enumerated after firmware transfer. Resolve
            // its port topology by the observed address, then pair the sibling
            // from the requested usb-path topology.
            const std::string address_text =
                std::to_string(firmware_runtime.bus) + ":" +
                std::to_string(firmware_runtime.address);
            const auto primary = resolve_device(context, address_text);
            if (!primary) {
                std::fprintf(stderr, "usb-path: %s\n",
                             px4::userland::error_string(primary.error()));
                ::close(lock_fd); libusb_exit(context);
                return primary.error() == px4::userland::Error::INVALID_ARGUMENT ? 2 : 3;
            }
            asicen::UsbLocation primary_location_ = primary.value().location;
            std::vector<std::uint8_t> primary_path_ = primary.value().port_path;
            asicen::UsbLocation sibling_location_{};
            std::vector<std::uint8_t> sibling_path_;
            if (options.usb_path_count >= 2U) {
                const auto sibling = resolve_device(context, options.usb_paths[1]);
                if (!sibling) {
                    std::fprintf(stderr, "usb-path: %s\n",
                                 px4::userland::error_string(sibling.error()));
                    ::close(lock_fd); libusb_exit(context);
                    return sibling.error() == px4::userland::Error::INVALID_ARGUMENT ? 2 : 3;
                }
                sibling_location_ = sibling.value().location;
                sibling_path_ = sibling.value().port_path;
                if (primary_location_.bus != sibling_location_.bus ||
                    primary_path_ == sibling_path_ ||
                    primary_path_.back() != 1U || sibling_path_.back() != 2U) {
                    std::fprintf(stderr, "refusing noncanonical primary/sibling port assignment\n");
                    ::close(lock_fd); libusb_exit(context);
                    return 2;
                }
            }
            hardware = std::make_unique<asicen::LibusbW3u3Hardware>(
                context, primary_location_, sibling_location_,
                std::move(primary_path_), std::move(sibling_path_), options.expected_model);
        } else {
            const auto primary = resolve_device(context, options.usb_paths[0]);
            if (!primary) {
                std::fprintf(stderr, "usb-path: %s\n",
                             px4::userland::error_string(primary.error()));
                ::close(lock_fd); libusb_exit(context);
                return primary.error() == px4::userland::Error::INVALID_ARGUMENT ? 2 : 3;
            }
            asicen::UsbLocation primary_location = primary.value().location;
            std::vector<std::uint8_t> primary_path = primary.value().port_path;
            asicen::UsbLocation sibling_location{};
            std::vector<std::uint8_t> sibling_path;
            if (options.usb_path_count >= 2U) {
                const auto sibling = resolve_device(context, options.usb_paths[1]);
                if (!sibling) {
                    std::fprintf(stderr, "usb-path: %s\n",
                                 px4::userland::error_string(sibling.error()));
                    ::close(lock_fd); libusb_exit(context);
                    return sibling.error() == px4::userland::Error::INVALID_ARGUMENT ? 2 : 3;
                }
                sibling_location = sibling.value().location;
                sibling_path = sibling.value().port_path;
                if (primary_location.bus != sibling_location.bus ||
                    primary_path == sibling_path ||
                    primary_path.back() != 1U || sibling_path.back() != 2U) {
                    std::fprintf(stderr, "refusing noncanonical primary/sibling port assignment\n");
                    ::close(lock_fd); libusb_exit(context);
                    return 2;
                }
            }
            hardware = std::make_unique<asicen::LibusbW3u3Hardware>(
                context, primary_location, sibling_location,
                std::move(primary_path), std::move(sibling_path), options.expected_model);
        }
        hardware->set_allow_lnb_power(options.allow_lnb_power);
        const auto claimed = hardware->claim();
        if (!claimed) {
            std::fprintf(stderr, "hardware model/topology/claim refused (dual-function models require sibling paths): %s\n",
                         px4::userland::error_string(claimed.error()));
            result = claimed.error() == px4::userland::Error::BUSY ? 4 : 3;
        } else if (options.probe_satellite) {
            const auto probe = hardware->probe_satellite(
                options.satellite_rf_khz, options.have_satellite_slot,
                options.satellite_slot, &stop_requested);
            std::fprintf(stderr,
                "satellite-probe status=%s rf_khz=%u lock=%u nonempty_slots=%u selected=%u\n",
                asicen::satellite_operation_result_name(probe.result),
                options.satellite_rf_khz, probe.locked ? 1U : 0U,
                static_cast<unsigned>(probe.nonempty_tsid_slots),
                probe.selected_slot ? 1U : 0U);
            result = probe.result == asicen::SatelliteOperationResult::Completed ? 0 :
                     probe.result == asicen::SatelliteOperationResult::Cancelled ? 130 : 70;
            const auto stopped = hardware->shutdown();
            if (!stopped) {
                std::fprintf(stderr, "satellite-probe cleanup failed\n");
                result = 70;
            }
            const auto released = hardware->release();
            if (!released) {
                std::fprintf(stderr, "USB claim release failed\n");
                result = 70;
            }
        } else if (options.probe_card) {
            const auto probe = hardware->probe_card(&stop_requested);
            std::fprintf(stderr,
                "card-probe status=%s atr_valid=%u atr_length=%zu "
                "response_length=%zu sw=%04x\n",
                px4::userland::error_string(probe.error),
                probe.atr_valid ? 1U : 0U, probe.atr_length,
                probe.response_length, probe.status_word);
            result = probe.error == px4::userland::Error::OK ? 0 :
                     probe.error == px4::userland::Error::TIMEOUT ? 130 : 70;
            const auto stopped = hardware->shutdown();
            if (!stopped) {
                std::fprintf(stderr, "card-probe cleanup failed\n");
                result = 70;
            }
            const auto released = hardware->release();
            if (!released) {
                std::fprintf(stderr, "USB claim release failed\n");
                result = 70;
            }
        } else if (options.card_only) {
            std::fprintf(stderr,
                "asicend card-only mode: tuner operations are unsupported\n");
            result = asicen::run_card_only_server(
                *hardware, options.runtime_dir.c_str(), options.instance.c_str(),
                options.group, &stop_requested);
            const auto stopped = hardware->shutdown();
            if (!stopped) {
                std::fprintf(stderr, "card-only hardware cleanup failed\n");
                result = 70;
            }
            const auto released = hardware->release();
            if (!released) {
                std::fprintf(stderr, "USB claim release failed\n");
                result = 70;
            }
        } else {
            result = asicen::run_live_card_stream_server(
                *hardware,
                options.runtime_dir.empty() ? nullptr : options.runtime_dir.c_str(),
                options.instance.c_str(), options.group, &stop_requested);
            const auto stopped = hardware->shutdown();
            if (!stopped) {
                std::fprintf(stderr, "asicend hardware cleanup failed: %s\n",
                             px4::userland::error_string(stopped.error()));
                result = 70;
            }
          const auto released = hardware->release();
          if (!released) {
              std::fprintf(stderr, "USB claim release failed\n");
              result = 70;
          }
        }
    }
    libusb_exit(context);
    ::close(lock_fd);
    return result;
}
