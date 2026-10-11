// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicend_args.h"
#include "asicend_identity.h"
#include "px4d_signals.h"
#if defined(_WIN32)
#include "px4d_windows_stdin.h"
#endif
#include "asicen/card_only_service.h"
#include "asicen/libusb_hardware_backend.h"
#include "asicen/libusb_transport.h"
#include "asicen/loader_firmware.h"
#include "asicen/firmware.h"
#include "px4/posix_ipc.h"
#include "px4/platform_sleep.h"

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
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace {
using Options = asicen::cli::DaemonArguments;
using asicen::cli::exit_status;
using asicen::cli::parse_bus_port;

void sleep_ms(std::uint32_t milliseconds) noexcept
{
#if defined(_WIN32)
    (void)px4::userland::platform::sleep_milliseconds(milliseconds);
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
#endif
}

int satellite_exit_status(asicen::SatelliteOperationResult result) noexcept
{
    using asicen::SatelliteOperationResult;
    switch (result) {
    case SatelliteOperationResult::Completed: return 0;
    case SatelliteOperationResult::InvalidArgument: return 2;
    case SatelliteOperationResult::Cancelled: return 3;
    case SatelliteOperationResult::DeadlineExceeded: return 5;
    case SatelliteOperationResult::FailedTransfer:
    case SatelliteOperationResult::ShortTransfer:
    case SatelliteOperationResult::VerificationFailed: return 7;
    }
    return 70;
}

class CleanupNotification final {
public:
    ~CleanupNotification() { px4::userland::px4d::notify_cleanup_complete(); }
};

#if defined(_WIN32)
void on_stdin_eof(void*) noexcept { px4::userland::px4d::request_stop(); }
#endif

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
    const bool address_form = asicen::cli::parse_bus_address(text, &bus, &address);
    std::vector<std::uint8_t> wanted_port;
    if (!address_form && !parse_bus_port(text, &bus, &wanted_port)) {
        return px4::userland::Result<ResolvedDevice>::failure(
            px4::userland::Error::INVALID_ARGUMENT);
    }
    libusb_device** list = nullptr;
    const ssize_t count = libusb_get_device_list(context, &list);
    if (count < 0) return px4::userland::Result<ResolvedDevice>::failure(
        asicen::map_libusb_error(static_cast<int>(count)));
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
        if (path_count <= 0 || path_count > static_cast<int>(sizeof(ports))) continue;
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

px4::userland::Result<std::string> observed_identity(
    libusb_context* context, const Options& options, std::size_t index = 0U)
{
    asicen::LibusbDevice device;
    int opened = 0;
    if (options.file_descriptor_count != 0U) {
        opened = device.open(context, options.file_descriptors[index]);
    } else {
        const auto resolved = resolve_device(context, options.usb_paths[index]);
        if (!resolved) return px4::userland::Result<std::string>::failure(resolved.error());
        opened = device.open(context, resolved.value().location);
    }
    if (opened != 0)
        return px4::userland::Result<std::string>::failure(asicen::map_libusb_acquisition_error(opened));
    libusb_device_descriptor descriptor{};
    const int read = device.descriptor(&descriptor);
    if (read != 0)
        return px4::userland::Result<std::string>::failure(asicen::map_libusb_error(read));
    const asicen::DeviceProfile* profile = asicen::find_profile(descriptor.idVendor, descriptor.idProduct);
    const bool loader = descriptor.idVendor == 0x1738U &&
        (descriptor.idProduct == 0x5211U || descriptor.idProduct == 0x5216U);
    if (profile == nullptr && loader) profile = options.expected_model;
    if (profile == nullptr || (options.expected_model != nullptr &&
        profile->model_id != options.expected_model->model_id))
        return px4::userland::Result<std::string>::failure(px4::userland::Error::UNSUPPORTED);
    asicen::UsbFunctionObservation observation;
    observation.bus = libusb_get_bus_number(device.device());
    std::uint8_t ports[8]{};
    const int count = libusb_get_port_numbers(device.device(), ports, sizeof(ports));
    if (count <= 0 || count > static_cast<int>(sizeof(ports)))
        return px4::userland::Result<std::string>::failure(px4::userland::Error::NOT_READY);
    observation.port_path.assign(ports, ports + count);
    const std::string identity = asicen::cli::topology_identity(
        observation, profile->expected_runtime_functions == 2U);
    if (identity.empty())
        return px4::userland::Result<std::string>::failure(px4::userland::Error::NOT_READY);
    return px4::userland::Result<std::string>::success(identity);
}

// Loads firmware into a loader device addressed by usb-path or fd. The
// caller asserts the resolved device is a loader (0x1738:0x5211/0x5216).
// On a successful transfer, loader receives the loader's observed bus and
// port path so the re-enumerated runtime can be matched to it.
// Public failures retain the PX4 error classes; warm runtime is an internal
// state, distinct from NOT_FOUND or a successful transfer.
struct LoaderIdentity {
    std::uint8_t bus = 0;
    std::vector<std::uint8_t> port_path;
};

// Internal-only return from load_loader_firmware meaning "the addressed device
// is not a loader; firmware is already present". Distinct from the public
// NOT_FOUND (3) so the daemon can continue claiming the warm runtime.
constexpr int kLoadFirmwareWarm = 1;

// Sends the observed sibling-restoring GPIO request: vendor IN, request Gpio
// (0x08), value 0x4040 (value 40 / mask 40), index 0, length 1. Recovered from
// HARDWARE-VALIDATION.md: after a primary runtime boots, this re-enumerates the
// sibling USB function as a loader so it can be provisioned independently.
// Returns 0 when the device answers, otherwise the mapped USB error class.
int restore_sibling_loader(asicen::LibusbDevice& device) {
    const asicen::ControlTransfer gpio_restore{
        0U, asicen::Request::Gpio, 0x4040U, 0U, 1U,
        asicen::Direction::In, 1000U};
    std::uint8_t response = 0U;
    const int rc = device.control(gpio_restore, &response);
    if (rc != 1) {
        std::fprintf(stderr, "firmware: sibling GPIO restore failed: %s\n",
                     libusb_error_name(rc < 0 ? rc : LIBUSB_ERROR_OTHER));
        return exit_status(asicen::map_libusb_error(rc < 0 ? rc : LIBUSB_ERROR_IO));
    }
    return 0;
}

// Pushes the verified loader image to one claimed loader, preserving USB and
// firmware error classes rather than reporting an internal error for each.
int transfer_firmware_to_loader(asicen::LibusbDevice& device,
                                const asicen::DeviceProfile& model,
                                const asicen::FirmwareImage& firmware) {
    const int driver = device.kernel_driver_active(0);
    if (driver > 0) { std::fprintf(stderr, "firmware: interface 0 has a kernel driver\n"); return 4; }
    if (driver < 0 && driver != LIBUSB_ERROR_NOT_SUPPORTED) {
        std::fprintf(stderr, "firmware: kernel_driver_active: %s\n", libusb_error_name(driver));
        return exit_status(asicen::map_libusb_error(driver));
    }
    const int claim = device.claim_interface(0);
    if (claim < 0) { std::fprintf(stderr, "firmware: claim interface 0: %s\n", libusb_error_name(claim)); return exit_status(asicen::map_libusb_acquisition_error(claim)); }
    const std::vector<asicen::LoaderTransfer> plan =
        asicen::build_loader_transfer_plan(model.model_id);
    if (plan.empty()) { std::fprintf(stderr, "firmware: no transfer plan for model\n"); return 10; }
    for (std::size_t i = 0; i < plan.size(); ++i) {
        if (px4::userland::px4d::stop_requested()) return 3;
        const asicen::LoaderTransfer& transfer = plan[i];
        const bool final_transfer = (i + 1 == plan.size());
        const int rc = device.vendor_out(transfer.request, transfer.value,
            transfer.index, firmware.data() + transfer.blob_offset,
            transfer.length, 1000);
        if (rc == LIBUSB_ERROR_NO_DEVICE && final_transfer) {
            std::fprintf(stderr, "firmware: final AC returned NO_DEVICE; acceptance ambiguous\n");
            return 7;
        }
        if (rc < 0) { std::fprintf(stderr, "firmware: control transfer failed: %s\n", libusb_error_name(rc)); return exit_status(asicen::map_libusb_error(rc)); }
        if (rc != transfer.length) { std::fprintf(stderr, "firmware: short transfer\n"); return 7; }
    }
    return 0;
}

// Re-enumeration must preserve the explicitly observed model and port path.
// Address changes are expected; widening to a different enclosure is not.
px4::userland::Result<asicen::UsbLocation> resolve_runtime_at_parent(
    libusb_context* context, std::uint8_t bus,
    const std::vector<std::uint8_t>& loader_port_path,
    const asicen::DeviceProfile* expected_model)
{
    if (expected_model == nullptr || loader_port_path.empty())
        return px4::userland::Result<asicen::UsbLocation>::failure(
            px4::userland::Error::INVALID_ARGUMENT);
    libusb_device** list = nullptr;
    const ssize_t count = libusb_get_device_list(context, &list);
    if (count < 0) return px4::userland::Result<asicen::UsbLocation>::failure(
        asicen::map_libusb_error(static_cast<int>(count)));
    asicen::UsbLocation selected{};
    std::size_t matches = 0U;
    for (ssize_t i = 0; i < count; ++i) {
        libusb_device* device = list[i];
        if (libusb_get_bus_number(device) != bus) continue;
        libusb_device_descriptor descriptor{};
        if (libusb_get_device_descriptor(device, &descriptor) != 0) continue;
        std::uint8_t ports[8]{};
        const int port_count = libusb_get_port_numbers(device, ports, sizeof(ports));
        if (port_count <= 0 || port_count > static_cast<int>(sizeof(ports))) continue;
        const asicen::UsbFunctionObservation observation{
            descriptor.idVendor, descriptor.idProduct, bus,
            std::vector<std::uint8_t>(ports, ports + port_count),
            libusb_get_device_address(device)};
        if (!asicen::cli::runtime_matches_loader(
                observation, *expected_model, bus, loader_port_path)) continue;
        selected = {bus, libusb_get_device_address(device)};
        ++matches;
    }
    libusb_free_device_list(list, 1);
    if (matches == 1U)
        return px4::userland::Result<asicen::UsbLocation>::success(selected);
    return px4::userland::Result<asicen::UsbLocation>::failure(
        matches > 1U ? px4::userland::Error::BUSY : px4::userland::Error::NOT_FOUND);
}

// Loads firmware into every addressed device (dual-function models expose two
// loader functions and both must be provisioned before the runtime pair is
// complete). A device that opens but is not a loader is a warm runtime and is
// skipped. loader_identity receives the primary (port ending in 1) loader's
// topology so the re-enumerated runtime can be matched to it.
// Returns 0 when at least one loader was transferred, kLoadFirmwareWarm when all
// addressed devices were already runtime, or the mapped public error class.
int load_loader_firmware(libusb_context* context, const Options& options,
                         const asicen::FirmwareImage& firmware,
                         LoaderIdentity* loader_identity) {
    const asicen::DeviceProfile* model = options.expected_model;
    if (model == nullptr) {
        std::fprintf(stderr, "--firmware requires --model to verify the loader image\n");
        return 2;
    }
    const std::size_t target_count = options.file_descriptor_count != 0U
        ? options.file_descriptor_count : options.usb_path_count;
    bool any_transferred = false;
    bool any_warm = false;
    // Dual-function models need a second stage: after provisioning the primary
    // loader, the runtime asks the device (GPIO restore) to re-enumerate the
    // sibling as a loader so it too can be provisioned independently. This
    // mirrors the observed bring-up in HARDWARE-VALIDATION.md.
    const bool dual_functions = target_count == 2U &&
        options.file_descriptor_count == 0U;
    for (std::size_t i = 0; i < target_count; ++i) {
        asicen::LibusbDevice device;
        int opened = LIBUSB_ERROR_NOT_FOUND;
        px4::userland::Error open_error = px4::userland::Error::NOT_FOUND;
        if (options.file_descriptor_count != 0U) {
            opened = device.open(context, options.file_descriptors[i]);
            open_error = asicen::map_libusb_acquisition_error(opened);
        } else {
            // After a GPIO sibling restore the loader re-enumerates with a
            // fresh address; retry resolution so the fresh sibling is used.
            const bool is_sibling_release = dual_functions && i == 1U;
            const auto resolve_deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(10);
            px4::userland::Result<ResolvedDevice> resolved =
                px4::userland::Result<ResolvedDevice>::failure(
                    px4::userland::Error::NOT_FOUND);
            while (std::chrono::steady_clock::now() < resolve_deadline) {
                if (px4::userland::px4d::stop_requested()) return 3;
                resolved = resolve_device(context, options.usb_paths[i]);
                if (resolved) break;
                if (resolved.error() != px4::userland::Error::NOT_FOUND &&
                    resolved.error() != px4::userland::Error::NOT_READY) break;
                if (!is_sibling_release) break;
                sleep_ms(250U);
            }
            if (resolved) {
                opened = device.open(context, resolved.value().location);
                open_error = asicen::map_libusb_acquisition_error(opened);
            } else {
                open_error = is_sibling_release &&
                    std::chrono::steady_clock::now() >= resolve_deadline
                        ? px4::userland::Error::TIMEOUT : resolved.error();
            }
        }
        if (opened != 0) {
            std::fprintf(stderr, "firmware: cannot resolve or open the addressed device: %s\n",
                         px4::userland::error_string(open_error));
            return exit_status(open_error);
        }
        libusb_device_descriptor desc{};
        const int described = device.descriptor(&desc);
        if (described != 0) {
            std::fprintf(stderr, "firmware: descriptor read failed: %s\n", libusb_error_name(described));
            return exit_status(asicen::map_libusb_error(described));
        }
        const bool loader = desc.idVendor == 0x1738 &&
            (desc.idProduct == 0x5211U || desc.idProduct == 0x5216U);
        if (!loader) {
            if (desc.idVendor != model->vid || desc.idProduct != model->pid) {
                std::fprintf(stderr, "firmware: addressed runtime does not match --model\n");
                return 3;
            }
            std::fprintf(stderr, "firmware: addressed device is not an ASICEN loader; treating as warm runtime\n");
            any_warm = true;
            continue;
        }
        // Record the primary loader's topology (prefer the port ending in 1).
        if (loader_identity != nullptr) {
            std::uint8_t ports[8]{};
            const int nports = libusb_get_port_numbers(
                device.device(), ports, static_cast<int>(sizeof(ports)));
            std::vector<std::uint8_t> path;
            if (nports > 0 && nports <= static_cast<int>(sizeof(ports)))
                path.assign(ports, ports + nports);
            const bool primary = !path.empty() && path.back() == 1U;
            if (loader_identity->port_path.empty() || primary) {
                loader_identity->bus =
                    static_cast<std::uint8_t>(libusb_get_bus_number(device.device()));
                loader_identity->port_path = std::move(path);
            }
        }
        const int rc = transfer_firmware_to_loader(device, *model, firmware);
        if (rc != 0) return rc;
        any_transferred = true;
        if (!dual_functions) continue;
        // Stage two: after the first (primary) transfer the device resolves as
        // a runtime. Ask it to re-enumerate the sibling as a loader, then wait
        // for the second path to become a loader again before provisioning it.
        // The sibling path is re-resolved by topology (not stale address).
        if (i == 0U) {
            // Wait for the primary runtime to appear so we can address it.
            if (loader_identity == nullptr || loader_identity->port_path.empty()) {
                std::fprintf(stderr, "firmware: primary topology unavailable for sibling restore\n");
                return 70;
            }
            asicen::LibusbDevice primary_runtime;
            asicen::UsbLocation runtime_loc{};
            bool runtime_found = false;
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(8);
            while (std::chrono::steady_clock::now() < deadline) {
                if (px4::userland::px4d::stop_requested()) return 3;
                const auto candidate = resolve_runtime_at_parent(
                    context, loader_identity->bus, loader_identity->port_path, model);
                if (candidate.has_value()) {
                    runtime_loc = candidate.value();
                    runtime_found = true;
                    break;
                }
                sleep_ms(200U);
            }
            if (!runtime_found) {
                std::fprintf(stderr, "firmware: primary runtime did not appear for sibling restore\n");
                return 5;
            }
            const int opened_runtime = primary_runtime.open(context, runtime_loc);
            if (opened_runtime != 0) {
                std::fprintf(stderr, "firmware: cannot open primary runtime for sibling restore: %s\n",
                             libusb_error_name(opened_runtime));
                return exit_status(asicen::map_libusb_acquisition_error(opened_runtime));
            }
            const int claimed_runtime = primary_runtime.claim_interface(0);
            if (claimed_runtime != 0) {
                std::fprintf(stderr, "firmware: cannot claim primary runtime for sibling restore: %s\n",
                             libusb_error_name(claimed_runtime));
                return exit_status(asicen::map_libusb_acquisition_error(claimed_runtime));
            }
            const int restore_rc = restore_sibling_loader(primary_runtime);
            if (restore_rc != 0) return restore_rc;
        }
        // The sibling loader re-enumerates at the second --usb-path topology.
        // Loop again; resolve_device on usb_paths[1] finds the fresh loader.
    }
    if (any_transferred) {
        std::fprintf(stderr, "firmware: loader transfer complete for %zu function(s)\n",
                     target_count);
        return 0;
    }
    if (any_warm) return kLoadFirmwareWarm;
    return 3;
}

// Poll one observed topology until its runtime identity appears or shutdown
// is requested. Every poll starts fresh; stale candidates never survive it.
px4::userland::Result<asicen::UsbLocation> wait_for_runtime(
    libusb_context* context, const asicen::DeviceProfile* expected_model,
    const LoaderIdentity& loader)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        if (px4::userland::px4d::stop_requested())
            return px4::userland::Result<asicen::UsbLocation>::failure(
                px4::userland::Error::NOT_READY);
        const auto candidate = resolve_runtime_at_parent(
            context, loader.bus, loader.port_path, expected_model);
        if (candidate || candidate.error() != px4::userland::Error::NOT_FOUND)
            return candidate;
        sleep_ms(250U);
    }
    return px4::userland::Result<asicen::UsbLocation>::failure(
        px4::userland::Error::TIMEOUT);
}
}  // namespace

int run_asicend_hardware(int argc, char** argv) {
    Options options = asicen::cli::parse_daemon_arguments(
        argc, const_cast<const char* const*>(argv));
    if (!options.valid) {
        std::fprintf(stderr, "argument error: %.*s\n",
                     static_cast<int>(options.error.size()), options.error.data());
        return 2;
    }
    auto firmware = px4::userland::Result<asicen::FirmwareImage>::failure(
        px4::userland::Error::NOT_READY);
    if (!options.firmware.empty()) {
        firmware = asicen::FirmwareProvider(options.firmware, options.expected_model->model_id).load();
        if (!firmware) {
            std::fprintf(stderr, "firmware: %s\n", px4::userland::error_string(firmware.error()));
            return exit_status(firmware.error());
        }
    }
    if (!px4::userland::px4d::install_signal_handlers()) {
        std::fprintf(stderr, "signal setup failed\n");
        return 70;
    }
    CleanupNotification cleanup_notification;
#if defined(_WIN32)
    px4::userland::cli::StdinEofMonitor eof_monitor;
    if (options.exit_on_stdin_eof &&
        !eof_monitor.start(::GetStdHandle(STD_INPUT_HANDLE), &on_stdin_eof, nullptr)) {
        std::fprintf(stderr, "stdin monitor setup failed\n");
        return 70;
    }
#endif
    asicen::LibusbContext owned_context;
    const int initialized = owned_context.initialize(options.file_descriptor_count != 0U);
    if (initialized != 0) {
        std::fprintf(stderr, "libusb_init: %s\n", libusb_error_name(initialized));
        return exit_status(asicen::map_libusb_error(initialized));
    }
    libusb_context* context = owned_context.get();
    const auto identity = observed_identity(context, options);
    if (!identity) {
        std::fprintf(stderr, "enclosure identity: %s\n", px4::userland::error_string(identity.error()));
        return exit_status(identity.error());
    }
    if (options.instance.empty()) options.instance = identity.value();
    // Freeze address selectors to the observed topology before a firmware
    // upload can change addresses. Every selected function must belong to the
    // same enclosure; an absent sibling is allowed only at its exact declared
    // port while a source-backed loader restore is pending.
    const std::size_t selected_count = options.file_descriptor_count != 0U
        ? options.file_descriptor_count : options.usb_path_count;
    for (std::size_t index = 0U; index < selected_count; ++index) {
        const auto selected = observed_identity(context, options, index);
        if (!selected) {
            asicen::UsbFunctionObservation pending;
            if (index == 0U || options.firmware.empty() ||
                options.file_descriptor_count != 0U || options.expected_model == nullptr ||
                (selected.error() != px4::userland::Error::NOT_FOUND &&
                 selected.error() != px4::userland::Error::NOT_READY) ||
                !parse_bus_port(options.usb_paths[index], &pending.bus, &pending.port_path) ||
                asicen::cli::topology_identity(pending,
                    options.expected_model->expected_runtime_functions == 2U) != identity.value()) {
                std::fprintf(stderr, "enclosure identity: %s\n",
                             px4::userland::error_string(selected.error()));
                return exit_status(selected.error());
            }
        } else if (selected.value() != identity.value()) {
            std::fprintf(stderr, "refusing functions from different enclosures\n");
            return 2;
        }
        if (options.usb_path_count != 0U) {
            const auto resolved = resolve_device(context, options.usb_paths[index]);
            if (resolved) {
                asicen::UsbFunctionObservation observation;
                observation.bus = resolved.value().location.bus;
                observation.port_path = resolved.value().port_path;
                options.usb_paths[index] = asicen::cli::port_string(observation);
            }
            std::uint8_t bus = 0U;
            std::vector<std::uint8_t> ports;
            if (!parse_bus_port(options.usb_paths[index], &bus, &ports) ||
                (selected_count == 2U && ports.back() != index + 1U)) {
                std::fprintf(stderr, "refusing noncanonical primary/sibling port assignment\n");
                return 2;
            }
        }
    }
    const px4::userland::ipc::posix::EndpointConfig endpoint{
        options.runtime_directory.empty() ? nullptr : options.runtime_directory.c_str(),
        options.instance.empty() ? identity.value().c_str() : options.instance.c_str(),
        px4::userland::ipc::posix::kControlEndpointName,
        options.group ? px4::userland::ipc::posix::EndpointAccess::shared_group
                      : px4::userland::ipc::posix::EndpointAccess::private_user};
    auto enclosure_lease = px4::userland::ipc::posix::SerialEndpointLease::acquire_identity(
        endpoint, identity.value());
    if (!enclosure_lease) {
        std::fprintf(stderr, "enclosure endpoint: %s\n",
                     px4::userland::error_string(enclosure_lease.error()));
        return exit_status(enclosure_lease.error());
    }
    // Native selectors are now observed port paths, so address changes after
    // firmware transfer cannot redirect the later hardware claim.
    if (!options.firmware.empty()) {
        // px4-compatible --firmware: upload the verified loader image, then
        // wait for the runtime to re-enumerate at the same topology before
        // claiming. A non-loader addressed device is treated as a warm runtime
        // (firmware already present) and claim proceeds without an upload.
        LoaderIdentity loader_identity;
        const int loaded = load_loader_firmware(context, options, firmware.value(),
                                                &loader_identity);
        if (loaded == kLoadFirmwareWarm) {
            // warm runtime: addressed device is not a loader, firmware is present
            std::fprintf(stderr, "firmware: addressed device is already runtime; skipping upload\n");
        } else if (loaded != 0) {

            return loaded;
        } else if (options.file_descriptor_count != 0U) {
            // fd mode cannot follow a re-enumerating device: after a loader
            // upload the granted fd points at the detached loader. Termux must
            // provision in two steps (upload + re-grant), then re-run warm.
            std::fprintf(stderr, "firmware: --fd cannot follow the loader re-enumeration; "
                                 "upload separately, re-grant the runtime fd, then run again\n");

            return 3;
        } else {
            const auto runtime = wait_for_runtime(context, options.expected_model,
                                                  loader_identity);
            if (!runtime) {
                std::fprintf(stderr, "firmware: runtime did not re-enumerate: %s\n",
                             px4::userland::error_string(runtime.error()));
                return exit_status(runtime.error());
            }
        }
    }
    int result = 70;
    {
        std::unique_ptr<asicen::LibusbW3u3Hardware> hardware;
        if (options.file_descriptor_count != 0U) {
            hardware.reset(new (std::nothrow) asicen::LibusbW3u3Hardware(
                context, options.file_descriptors[0],
                options.file_descriptor_count >= 2U ? options.file_descriptors[1] : -1,
                options.expected_model));
        } else {
            const auto primary = resolve_device(context, options.usb_paths[0]);
            if (!primary) {
                std::fprintf(stderr, "usb-path: %s\n",
                             px4::userland::error_string(primary.error()));
                return exit_status(primary.error());
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
                    return exit_status(sibling.error());
                }
                sibling_location = sibling.value().location;
                sibling_path = sibling.value().port_path;
                if (primary_location.bus != sibling_location.bus ||
                    primary_path == sibling_path ||
                    primary_path.back() != 1U || sibling_path.back() != 2U) {
                    std::fprintf(stderr, "refusing noncanonical primary/sibling port assignment\n");

                    return 2;
                }
            }
            hardware.reset(new (std::nothrow) asicen::LibusbW3u3Hardware(
                context, primary_location, sibling_location,
                std::move(primary_path), std::move(sibling_path), options.expected_model));
        }
        if (!hardware) return 70;
        hardware->set_allow_lnb_power(options.allow_lnb_power);
        const auto claimed = hardware->claim();
        if (!claimed) {
            std::fprintf(stderr, "hardware model/topology/claim refused (dual-function models require sibling paths): %s\n",
                         px4::userland::error_string(claimed.error()));
            result = exit_status(claimed.error());
        } else if (options.probe_satellite) {
            const auto probe = hardware->probe_satellite(
                options.satellite_rf_khz, options.have_satellite_slot,
                options.satellite_slot, nullptr, &px4::userland::px4d::stop_requested);
            std::fprintf(stderr,
                "satellite-probe status=%s rf_khz=%u lock=%u nonempty_slots=%u selected=%u\n",
                asicen::satellite_operation_result_name(probe.result),
                options.satellite_rf_khz, probe.locked ? 1U : 0U,
                static_cast<unsigned>(probe.nonempty_tsid_slots),
                probe.selected_slot ? 1U : 0U);
            result = satellite_exit_status(probe.result);
        } else if (options.probe_card) {
            const auto probe = hardware->probe_card(nullptr, &px4::userland::px4d::stop_requested);
            std::fprintf(stderr,
                "card-probe status=%s atr_valid=%u atr_length=%zu "
                "response_length=%zu sw=%04x\n",
                px4::userland::error_string(probe.error),
                probe.atr_valid ? 1U : 0U, probe.atr_length,
                probe.response_length, probe.status_word);
            result = exit_status(probe.error);
        } else if (options.card_only) {
            std::fprintf(stderr,
                "asicend card-only mode: tuner operations are unsupported\n");
            result = asicen::run_card_only_server(
                *hardware,
                options.runtime_directory.empty() ? nullptr : options.runtime_directory.c_str(),
                options.instance.c_str(),
                options.group, nullptr, &px4::userland::px4d::stop_requested);
        } else {
            result = asicen::run_live_card_stream_server(
                *hardware,
                options.runtime_directory.empty() ? nullptr : options.runtime_directory.c_str(),
                options.instance.c_str(), options.group, nullptr,
                &px4::userland::px4d::stop_requested);
        }
        if (claimed) {
            const auto cleanup = hardware->shutdown();
            if (!cleanup) {
                std::fprintf(stderr, "shutdown cleanup: %s\n",
                             px4::userland::error_string(cleanup.error()));
                if (result == 0) result = exit_status(cleanup.error());
            }
            const auto released = hardware->release();
            if (!released) {
                std::fprintf(stderr, "USB claim release: %s\n",
                             px4::userland::error_string(released.error()));
                if (result == 0) result = exit_status(released.error());
            }
        }
    }
    return result;
}
