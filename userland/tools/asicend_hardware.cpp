// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicend_args.h"
#include "asicend_identity.h"
#include "asicend_selector.h"
#include "loader_acquisition.h"
#include "loader_diagnostic.h"
#include "px4d_signals.h"
#include "asicend_identity_lease.h"
#if defined(_WIN32)
#include "dtv/userland/host_facade/windows_stdin.h"
#include "dtv/userland/host_card_t1/platform_sleep.h"
#endif
#include "asicen/card_only_service.h"
#include "asicen/libusb_hardware_backend.h"
#include "asicen/libusb_transport.h"
#include "asicen/loader_firmware.h"
#include "asicen/startup_readiness.h"
#include "asicen/firmware.h"
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
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {
using Options = asicen::cli::DaemonArguments;
using asicen::cli::exit_status;
using asicen::cli::parse_bus_port;
namespace loader_diag = asicen::cli::loader_diagnostic;

void sleep_ms(std::uint32_t milliseconds) noexcept
{
#if defined(_WIN32)
    (void)dtv::userland::host_card_t1::platform::sleep_milliseconds(milliseconds);
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
#endif
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

px4::userland::Result<std::vector<asicen::UsbFunctionObservation>> enumerate_selector_functions(
    libusb_context* context)
{
    libusb_device** list = nullptr;
    const ssize_t count = libusb_get_device_list(context, &list);
    if (count < 0) return px4::userland::Result<std::vector<asicen::UsbFunctionObservation>>::failure(
        asicen::map_libusb_error(static_cast<int>(count)));
    std::vector<asicen::UsbFunctionObservation> observations;
    for (ssize_t i = 0; i < count; ++i) {
        libusb_device* device = list[i];
        libusb_device_descriptor descriptor{};
        if (libusb_get_device_descriptor(device, &descriptor) != 0) continue;
        const bool loader = descriptor.idVendor == 0x1738U &&
            (descriptor.idProduct == 0x5211U || descriptor.idProduct == 0x5216U);
        if (!loader && asicen::find_profile(descriptor.idVendor, descriptor.idProduct) == nullptr)
            continue;
        asicen::UsbFunctionObservation observation;
        observation.vid = descriptor.idVendor;
        observation.pid = descriptor.idProduct;
        observation.bus = libusb_get_bus_number(device);
        observation.address = libusb_get_device_address(device);
        std::uint8_t ports[8]{};
        const int port_count = libusb_get_port_numbers(device, ports, sizeof(ports));
        if (port_count > 0 && port_count <= static_cast<int>(sizeof(ports)))
            observation.port_path.assign(ports, ports + port_count);
        libusb_device* parent = libusb_get_parent(device);
        libusb_device_descriptor parent_descriptor{};
        if (parent != nullptr && libusb_get_device_descriptor(parent, &parent_descriptor) == 0) {
            observation.parent_hub_vid = parent_descriptor.idVendor;
            observation.parent_hub_pid = parent_descriptor.idProduct;
            observation.parent_hub_known = true;
        }
        observations.push_back(std::move(observation));
    }
    libusb_free_device_list(list, 1);
    return px4::userland::Result<std::vector<asicen::UsbFunctionObservation>>::success(
        std::move(observations));
}

px4::userland::Error resolve_device_selector(libusb_context* context, Options* options)
{
    if (options == nullptr || options->device_selector.empty())
        return px4::userland::Error::INVALID_ARGUMENT;
    const auto parsed = dtv::userland::identity::parse_device_selector(options->device_selector);
    if (!parsed) return px4::userland::Error::INVALID_ARGUMENT;
    const auto observations = enumerate_selector_functions(context);
    if (!observations) return observations.error();
    const auto selected = asicen::cli::select_enclosure(
        parsed.value(), observations.value(), options->expected_model,
        !options->firmware.empty());
    if (!selected) return selected.error();
    if (selected.value().model == nullptr && options->firmware.empty())
        return px4::userland::Error::NOT_READY;
    if (selected.value().functions.empty() ||
        selected.value().functions.size() > options->usb_paths.size())
        return px4::userland::Error::INVALID_ARGUMENT;
    if (options->expected_model == nullptr) options->expected_model = selected.value().model;
    options->usb_path_count = selected.value().functions.size();
    for (std::size_t i = 0U; i < options->usb_path_count; ++i) {
        options->usb_paths[i] = asicen::cli::port_string(selected.value().functions[i]);
        if (options->usb_paths[i].empty()) return px4::userland::Error::NOT_READY;
    }
    return px4::userland::Error::OK;
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
    if ((!loader && profile == nullptr) || (loader && options.expected_model == nullptr &&
        options.firmware.empty()) || (options.expected_model != nullptr &&
        profile->model_id != options.expected_model->model_id))
        return px4::userland::Result<std::string>::failure(px4::userland::Error::UNSUPPORTED);
    asicen::UsbFunctionObservation observation;
    observation.bus = libusb_get_bus_number(device.device());
    std::uint8_t ports[8]{};
    int count = libusb_get_port_numbers(device.device(), ports, sizeof(ports));
    if (count == 0 && options.file_descriptor_count != 0U) {
        std::uint8_t connection_bus = 0U;
        count = device.connection_info(&connection_bus, ports, sizeof(ports));
        if (count > 0 && connection_bus != observation.bus)
            return px4::userland::Result<std::string>::failure(px4::userland::Error::NOT_READY);
    }
    if (count <= 0 || count > static_cast<int>(sizeof(ports))) {
        if (options.file_descriptor_count != 0U)
            return px4::userland::Result<std::string>::success({});
        return px4::userland::Result<std::string>::failure(px4::userland::Error::NOT_READY);
    }
    observation.port_path.assign(ports, ports + count);
    const bool dual = profile != nullptr
        ? profile->expected_runtime_functions == 2U
        : options.usb_path_count == 2U;
    const std::string identity = asicen::cli::topology_identity(
        observation, dual);
    if (identity.empty())
        return px4::userland::Result<std::string>::failure(px4::userland::Error::NOT_READY);
    return px4::userland::Result<std::string>::success(identity);
}

px4::userland::Error infer_file_descriptor_model(libusb_context* context,
                                                   Options* options)
{
    if (context == nullptr || options == nullptr)
        return px4::userland::Error::INVALID_ARGUMENT;
    const asicen::DeviceProfile* inferred_model = nullptr;
    for (std::size_t index = 0U; index < options->file_descriptor_count; ++index) {
        asicen::LibusbDevice device;
        const int opened = device.open(context, options->file_descriptors[index]);
        if (opened != 0) return asicen::map_libusb_acquisition_error(opened);
        libusb_device_descriptor descriptor{};
        const int described = device.descriptor(&descriptor);
        if (described != 0) return asicen::map_libusb_error(described);
        if (descriptor.idVendor == 0x1738U &&
            (descriptor.idProduct == 0x5211U || descriptor.idProduct == 0x5216U)) {
            continue;
        }
        const auto* profile = asicen::find_profile(descriptor.idVendor, descriptor.idProduct);
        if (profile == nullptr || !asicen::profile_runtime_supported(*profile) ||
            (inferred_model != nullptr && inferred_model->model_id != profile->model_id)) {
            return px4::userland::Error::UNSUPPORTED;
        }
        inferred_model = profile;
    }
    options->expected_model = inferred_model;
    return px4::userland::Error::OK;
}

px4::userland::Result<std::vector<std::uint8_t>> observed_port_path(
    libusb_context* context, const Options& options, std::size_t index)
{
    asicen::LibusbDevice device;
    int opened = 0;
    if (options.file_descriptor_count != 0U) {
        opened = device.open(context, options.file_descriptors[index]);
    } else {
        const auto resolved = resolve_device(context, options.usb_paths[index]);
        if (!resolved)
            return px4::userland::Result<std::vector<std::uint8_t>>::failure(resolved.error());
        opened = device.open(context, resolved.value().location);
    }
    if (opened != 0)
        return px4::userland::Result<std::vector<std::uint8_t>>::failure(
            asicen::map_libusb_acquisition_error(opened));
    std::uint8_t ports[8]{};
    int count = libusb_get_port_numbers(device.device(), ports, sizeof(ports));
    if (count == 0 && options.file_descriptor_count != 0U) {
        std::uint8_t bus = 0U;
        count = device.connection_info(&bus, ports, sizeof(ports));
        if (count > 0 && bus != libusb_get_bus_number(device.device()))
            return px4::userland::Result<std::vector<std::uint8_t>>::failure(
                px4::userland::Error::NOT_READY);
    }
    if (count <= 0 || count > static_cast<int>(sizeof(ports)))
        return px4::userland::Result<std::vector<std::uint8_t>>::failure(
            px4::userland::Error::NOT_READY);
    return px4::userland::Result<std::vector<std::uint8_t>>::success(
        std::vector<std::uint8_t>(ports, ports + count));
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
int restore_sibling_loader(asicen::LibusbDevice& device,
                           loader_diag::Trace* trace,
                           const loader_diag::Target& target) {
    const asicen::ControlTransfer gpio_restore{
        0U, asicen::Request::Gpio, 0x4040U, 0U, 1U,
        asicen::Direction::In, 1000U};
    std::uint8_t response = 0U;
    if (trace != nullptr) trace->record(loader_diag::Stage::GpioRestoreBegin, target);
    const int rc = device.control(gpio_restore, &response);
    if (trace != nullptr) trace->record(loader_diag::Stage::GpioRestoreResult,
        target, rc, loader_diag::Domain::Libusb, rc != 1);
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
                                const asicen::FirmwareImage& firmware,
                                loader_diag::Trace* trace,
                                const loader_diag::Target& target) {
    const int driver = device.kernel_driver_active(0);
    if (trace != nullptr) trace->record(loader_diag::Stage::KernelDriverResult,
        target, driver, loader_diag::Domain::Libusb,
        driver > 0 || (driver < 0 && driver != LIBUSB_ERROR_NOT_SUPPORTED));
    if (driver > 0) { std::fprintf(stderr, "firmware: interface 0 has a kernel driver\n"); return 4; }
    if (driver < 0 && driver != LIBUSB_ERROR_NOT_SUPPORTED) {
        std::fprintf(stderr, "firmware: kernel_driver_active: %s\n", libusb_error_name(driver));
        return exit_status(asicen::map_libusb_error(driver));
    }
    if (trace != nullptr) trace->record(loader_diag::Stage::ClaimBegin, target);
    const int claim = device.claim_interface(0);
    if (trace != nullptr) trace->record(loader_diag::Stage::ClaimResult,
        target, claim, loader_diag::Domain::Libusb, claim < 0);
    if (claim < 0) { std::fprintf(stderr, "firmware: claim interface 0: %s\n", libusb_error_name(claim)); return exit_status(asicen::map_libusb_acquisition_error(claim)); }
    if (firmware.manifest() == nullptr) {
        std::fprintf(stderr, "firmware: verified loader manifest unavailable\n");
        return 10;
    }
    const std::vector<asicen::LoaderTransfer> plan =
        asicen::build_loader_transfer_plan(*firmware.manifest());
    if (plan.empty()) { std::fprintf(stderr, "firmware: no transfer plan for image\n"); return 10; }
    if (trace != nullptr) trace->record(loader_diag::Stage::UploadBegin, target);
    for (std::size_t i = 0; i < plan.size(); ++i) {
        if (px4::userland::px4d::stop_requested()) return 3;
        const asicen::LoaderTransfer& transfer = plan[i];
        const bool final_transfer = (i + 1 == plan.size());
        const auto vendor_out = [&]() {
            return device.vendor_out(transfer.request, transfer.value,
                transfer.index, firmware.data() + transfer.blob_offset,
                transfer.length, 1000);
        };
        const int rc = trace != nullptr ? trace->vendor_out(target, i + 1U, plan.size(),
            transfer.request, transfer.value, transfer.index, transfer.blob_offset,
            transfer.length, vendor_out) : vendor_out();
        if (rc == LIBUSB_ERROR_NO_DEVICE && final_transfer) {
            std::fprintf(stderr, "firmware: final AC returned NO_DEVICE; acceptance ambiguous\n");
            return 7;
        }
        if (rc < 0) { std::fprintf(stderr, "firmware: control transfer failed: %s\n", libusb_error_name(rc)); return exit_status(asicen::map_libusb_error(rc)); }
        if (rc != transfer.length) { std::fprintf(stderr, "firmware: short transfer\n"); return 7; }
    }
    if (trace != nullptr) trace->record(loader_diag::Stage::UploadSuccess, target);
    return 0;
}

// Re-enumeration must preserve the selected USB path. Address changes are
// expected; widening to a different enclosure is not.
px4::userland::Result<asicen::UsbLocation> resolve_runtime_at_parent(
    libusb_context* context, std::uint8_t bus,
    const std::vector<std::uint8_t>& loader_port_path,
    const asicen::DeviceProfile* expected_model,
    loader_diag::Target* diagnostic_target = nullptr,
    const asicen::DeviceProfile** observed_model = nullptr)
{
    if (loader_port_path.empty())
        return px4::userland::Result<asicen::UsbLocation>::failure(
            px4::userland::Error::INVALID_ARGUMENT);
    libusb_device** list = nullptr;
    const ssize_t count = libusb_get_device_list(context, &list);
    if (count < 0) return px4::userland::Result<asicen::UsbLocation>::failure(
        asicen::map_libusb_error(static_cast<int>(count)));
    asicen::UsbLocation selected{};
    const asicen::DeviceProfile* selected_model = nullptr;
    loader_diag::Target selected_diagnostic{};
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
        const auto* profile = asicen::find_profile(descriptor.idVendor, descriptor.idProduct);
        if (profile == nullptr || !asicen::profile_runtime_supported(*profile) ||
            observation.bus != bus || observation.port_path != loader_port_path ||
            (expected_model != nullptr && profile->model_id != expected_model->model_id) ||
            asicen::loader_firmware_manifest(profile->model_id) == nullptr) continue;
        selected = {bus, libusb_get_device_address(device)};
        selected_model = profile;
        if (diagnostic_target != nullptr) {
            selected_diagnostic = *diagnostic_target;
            selected_diagnostic.location_known = true;
            selected_diagnostic.bus = selected.bus;
            selected_diagnostic.address = selected.address;
            selected_diagnostic.port_count = static_cast<std::uint8_t>(port_count);
            std::copy(ports, ports + port_count, selected_diagnostic.ports.begin());
            selected_diagnostic.ids_known = true;
            selected_diagnostic.vendor_id = descriptor.idVendor;
            selected_diagnostic.product_id = descriptor.idProduct;
        }
        ++matches;
    }
    libusb_free_device_list(list, 1);
    if (matches == 1U) {
        if (diagnostic_target != nullptr) *diagnostic_target = selected_diagnostic;
        if (observed_model != nullptr) *observed_model = selected_model;
        return px4::userland::Result<asicen::UsbLocation>::success(selected);
    }
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
int load_loader_firmware(asicen::LibusbContext& owned_context, Options& options,
                         const asicen::FirmwareImage& firmware,
                         LoaderIdentity* loader_identity,
                         std::vector<asicen::UploadedRuntimeIdentity>* uploaded_identities) {
    libusb_context* context = owned_context.get();
    if (context == nullptr) return exit_status(px4::userland::Error::NOT_READY);
    const loader_diag::Mode diagnostic_mode = loader_diag::parse_mode(
        std::getenv("ASICEN_DIAGNOSTIC_LOADER"));
    if (diagnostic_mode == loader_diag::Mode::Invalid) {
        std::fprintf(stderr, "firmware: ASICEN_DIAGNOSTIC_LOADER must be 0 or 1\n");
        return 2;
    }
    // Stack-owned trace emits only when this loader call exits, after the
    // device objects have been destroyed. It never changes a USB result.
    loader_diag::Trace diagnostic(diagnostic_mode);
    loader_diag::Trace* trace = diagnostic.enabled() ? &diagnostic : nullptr;
    const asicen::DeviceProfile* model = options.expected_model;
    if (firmware.manifest() != &asicen::common_loader_firmware_manifest()) {
        std::fprintf(stderr, "firmware: verified loader image does not match requested model\n");
        return diagnostic.finish(2);
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
    bool sibling_restored = false;
    for (std::size_t i = 0; i < target_count; ++i) {
        asicen::LibusbDevice device;
        loader_diag::Target target{};
        target.role = i == 0U ? loader_diag::Role::Primary : loader_diag::Role::Sibling;
        target.selected_index = i;
        int opened = LIBUSB_ERROR_NOT_FOUND;
        px4::userland::Error open_error = px4::userland::Error::NOT_FOUND;
        if (options.file_descriptor_count != 0U) {
            if (trace != nullptr) trace->record(loader_diag::Stage::OpenBegin, target);
            opened = device.open(context, options.file_descriptors[i]);
            open_error = asicen::map_libusb_acquisition_error(opened);
            if (trace != nullptr) trace->record(loader_diag::Stage::OpenResult,
                target, opened, loader_diag::Domain::Libusb, opened != 0);
        } else {
            // GPIO restore can leave the old sibling address visible briefly.
            // Re-resolve after a transient open failure, within the same budget.
            // Open retries are restricted to a successful sibling restore;
            // warm siblings retain their existing resolution-only retries.
            namespace acquisition = asicen::cli::loader_acquisition;
            static_assert(acquisition::kOpenIo == LIBUSB_ERROR_IO &&
                          acquisition::kOpenNoDevice == LIBUSB_ERROR_NO_DEVICE &&
                          acquisition::kOpenNotFound == LIBUSB_ERROR_NOT_FOUND);
            const bool is_sibling_release = dual_functions && i == 1U;
            const auto acquired = acquisition::run(is_sibling_release,
                [] { return std::chrono::steady_clock::now(); },
                [] { return px4::userland::px4d::stop_requested(); },
                [&] {
                    // Do not attribute a failed fresh resolution to the last address.
                    target.location_known = false;
                    target.port_count = 0U;
                    if (trace != nullptr) trace->record(loader_diag::Stage::ResolveBegin, target);
                    const auto resolved = resolve_device(context, options.usb_paths[i]);
                    if (!resolved) {
                        open_error = resolved.error();
                        if (trace != nullptr) trace->record(loader_diag::Stage::ResolveResult,
                            target, static_cast<int>(open_error), loader_diag::Domain::Px4, true);
                        return acquisition::retryable_resolve_error(open_error)
                            ? acquisition::Attempt::Retry : acquisition::Attempt::Failed;
                    }
                    if (trace != nullptr) {
                        target.location_known = true;
                        target.bus = resolved.value().location.bus;
                        target.address = resolved.value().location.address;
                        target.port_count = static_cast<std::uint8_t>(resolved.value().port_path.size());
                        std::copy(resolved.value().port_path.begin(), resolved.value().port_path.end(),
                                  target.ports.begin());
                        trace->record(loader_diag::Stage::ResolveResult, target, 0, loader_diag::Domain::Px4);
                        trace->record(loader_diag::Stage::OpenBegin, target);
                    }
                    opened = device.open(context, resolved.value().location);
                    open_error = asicen::map_libusb_acquisition_error(opened);
                    if (trace != nullptr) trace->record(loader_diag::Stage::OpenResult,
                        target, opened, loader_diag::Domain::Libusb, opened != 0);
                    if (opened == 0) return acquisition::Attempt::Acquired;
                    // open() closes partial handles itself. NOT_FOUND also covers
                    // disappearance between enumeration and opening. Do not retry
                    // ACCESS, BUSY, invalid parameters or other permanent errors.
                    return sibling_restored && acquisition::retryable_open_error(opened)
                        ? acquisition::Attempt::Retry : acquisition::Attempt::Failed;
                },
                [](std::chrono::steady_clock::duration delay) {
                    sleep_ms(static_cast<std::uint32_t>(
                        std::chrono::ceil<std::chrono::milliseconds>(delay).count()));
                });
            if (acquired == acquisition::Result::Cancelled) return diagnostic.finish(3);
            if (acquired == acquisition::Result::TimedOut)
                open_error = px4::userland::Error::TIMEOUT;
        }
        if (opened != 0) {
            std::fprintf(stderr, "firmware: cannot resolve or open the addressed device: %s\n",
                         px4::userland::error_string(open_error));
            return diagnostic.finish(exit_status(open_error));
        }
        libusb_device_descriptor desc{};
        const int described = device.descriptor(&desc);
        if (trace != nullptr) {
            if (described == 0) {
                target.ids_known = true;
                target.vendor_id = desc.idVendor;
                target.product_id = desc.idProduct;
            }
            trace->record(loader_diag::Stage::DescriptorResult,
                target, described, loader_diag::Domain::Libusb, described != 0);
        }
        if (described != 0) {
            std::fprintf(stderr, "firmware: descriptor read failed: %s\n", libusb_error_name(described));
            return diagnostic.finish(exit_status(asicen::map_libusb_error(described)));
        }
        const bool loader = desc.idVendor == 0x1738 &&
            (desc.idProduct == 0x5211U || desc.idProduct == 0x5216U);
        if (!loader) {
            const auto* runtime_model = asicen::find_profile(desc.idVendor, desc.idProduct);
            if (runtime_model == nullptr ||
                !asicen::profile_runtime_supported(*runtime_model) ||
                (model != nullptr && runtime_model->model_id != model->model_id)) {
                std::fprintf(stderr, "firmware: addressed runtime does not match --model\n");
                return diagnostic.finish(3);
            }
            if (model == nullptr) {
                model = runtime_model;
                options.expected_model = model;
            }
            std::fprintf(stderr, "firmware: addressed device is not an ASICEN loader; treating as warm runtime\n");
            any_warm = true;
            continue;
        }
        libusb_config_descriptor* loader_config = nullptr;
        const int loader_config_rc = libusb_get_config_descriptor(
            device.device(), 0U, &loader_config);
        if (loader_config_rc != 0 || loader_config == nullptr) {
            if (loader_config != nullptr) libusb_free_config_descriptor(loader_config);
            std::fprintf(stderr, "firmware: loader configuration read failed: %s\n",
                         libusb_error_name(loader_config_rc));
            return diagnostic.finish(loader_config_rc == 0
                ? exit_status(px4::userland::Error::UNSUPPORTED)
                : exit_status(asicen::map_libusb_error(loader_config_rc)));
        }
        const bool loader_interface0 = loader_config->bNumInterfaces == 1U &&
            loader_config->interface[0].num_altsetting == 1 &&
            loader_config->interface[0].altsetting[0].bInterfaceNumber == 0U &&
            loader_config->interface[0].altsetting[0].bAlternateSetting == 0U;
        libusb_free_config_descriptor(loader_config);
        if (!loader_interface0) {
            std::fprintf(stderr, "firmware: loader interface profile mismatch\n");
            return diagnostic.finish(exit_status(px4::userland::Error::UNSUPPORTED));
        }
        // Freeze the selected loader topology before transfer changes its
        // address. Only successful uploads enter the final readiness set.
        std::uint8_t ports[8]{};
        int nports = libusb_get_port_numbers(
            device.device(), ports, static_cast<int>(sizeof(ports)));
        if (nports == 0 && options.file_descriptor_count != 0U) {
            std::uint8_t connection_bus = 0U;
            nports = device.connection_info(&connection_bus, ports, sizeof(ports));
            if (nports > 0 && connection_bus != libusb_get_bus_number(device.device()))
                nports = LIBUSB_ERROR_NOT_FOUND;
        }
        if ((nports <= 0 || nports > static_cast<int>(sizeof(ports))) &&
            options.file_descriptor_count == 0U) {
            if (trace != nullptr) trace->record(loader_diag::Stage::TopologyResult,
                target, nports, loader_diag::Domain::Libusb, true);
            std::fprintf(stderr, "firmware: loader topology unavailable before transfer\n");
            return diagnostic.finish(exit_status(px4::userland::Error::NOT_READY));
        }
        asicen::UploadedRuntimeIdentity uploaded{};
        uploaded.selected_index = i;
        uploaded.bus = static_cast<std::uint8_t>(libusb_get_bus_number(device.device()));
        if (nports > 0 && nports <= static_cast<int>(sizeof(ports)))
            uploaded.port_path.assign(ports, ports + nports);
        if (trace != nullptr && nports > 0 && nports <= static_cast<int>(sizeof(ports))) {
            target.location_known = true;
            target.bus = uploaded.bus;
            // libusb stores this address in its device object: no control
            // request is made, and this is never called after transfer.
            target.address = libusb_get_device_address(device.device());
            target.port_count = static_cast<std::uint8_t>(nports);
            std::copy(ports, ports + nports, target.ports.begin());
            trace->record(loader_diag::Stage::TopologyResult,
                target, nports, loader_diag::Domain::Libusb);
        }
        if (loader_identity != nullptr && !uploaded.port_path.empty()) {
            const bool primary = uploaded.port_path.back() == 1U;
            if (loader_identity->port_path.empty() || primary) {
                loader_identity->bus = uploaded.bus;
                loader_identity->port_path = uploaded.port_path;
            }
        }
        const int rc = transfer_firmware_to_loader(device, firmware, trace, target);
        if (rc != 0) return diagnostic.finish(rc);
        device.close();
        if (options.file_descriptor_count == 0U) {
            owned_context.close();
            const int initialized = owned_context.initialize(false);
            if (initialized != 0) {
                std::fprintf(stderr, "firmware: libusb context refresh failed: %s\n",
                             libusb_error_name(initialized));
                return diagnostic.finish(exit_status(asicen::map_libusb_error(initialized)));
            }
            context = owned_context.get();
        }
        if (model == nullptr) {
            if (uploaded.port_path.empty()) {
                if (options.file_descriptor_count != 0U) {
                    std::fprintf(stderr, "firmware: transfer complete; regrant runtime FD and rerun without --firmware\n");
                } else {
                    std::fprintf(stderr, "firmware: runtime topology unavailable for model inference\n");
                }
                return diagnostic.finish(exit_status(px4::userland::Error::NOT_READY));
            }
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
            px4::userland::Error runtime_error = px4::userland::Error::NOT_FOUND;
            while (std::chrono::steady_clock::now() < deadline) {
                if (px4::userland::px4d::stop_requested()) return diagnostic.finish(3);
                const asicen::DeviceProfile* runtime_model = nullptr;
                const auto runtime = resolve_runtime_at_parent(
                    context, uploaded.bus, uploaded.port_path, nullptr, nullptr, &runtime_model);
                if (runtime && runtime_model != nullptr) {
                    model = runtime_model;
                    options.expected_model = model;
                    break;
                }
                runtime_error = runtime.error();
                if (runtime_error == px4::userland::Error::BUSY) {
                    std::fprintf(stderr, "firmware: runtime model is ambiguous at selected USB location\n");
                    return diagnostic.finish(exit_status(runtime_error));
                }
                sleep_ms(200U);
            }
            if (model == nullptr) {
                std::fprintf(stderr, "firmware: runtime model did not return at the selected USB location\n");
                return diagnostic.finish(exit_status(runtime_error));
            }
        }
        uploaded.vendor_id = model->vid;
        uploaded.product_id = model->pid;
        if (uploaded_identities != nullptr) uploaded_identities->push_back(uploaded);
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
                return diagnostic.finish(70);
            }
            asicen::LibusbDevice primary_runtime;
            asicen::UsbLocation runtime_loc{};
            bool runtime_found = false;
            loader_diag::Target runtime_target{};
            runtime_target.role = loader_diag::Role::Primary;
            runtime_target.selected_index = i;
            px4::userland::Error runtime_error = px4::userland::Error::NOT_FOUND;
            if (trace != nullptr) trace->record(loader_diag::Stage::PrimaryRuntimeBegin, runtime_target);
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(8);
            while (std::chrono::steady_clock::now() < deadline) {
                if (px4::userland::px4d::stop_requested()) return diagnostic.finish(3);
                const auto candidate = resolve_runtime_at_parent(
                    context, loader_identity->bus, loader_identity->port_path, model,
                    trace != nullptr ? &runtime_target : nullptr);
                if (candidate.has_value()) {
                    runtime_loc = candidate.value();
                    runtime_found = true;
                    if (trace != nullptr) trace->record(loader_diag::Stage::PrimaryRuntimeFound,
                        runtime_target);
                    break;
                }
                if (trace != nullptr) runtime_error = candidate.error();
                sleep_ms(200U);
            }
            if (!runtime_found) {
                if (trace != nullptr) trace->record(loader_diag::Stage::PrimaryRuntimeResult,
                    runtime_target, static_cast<int>(runtime_error), loader_diag::Domain::Px4, true);
                std::fprintf(stderr, "firmware: primary runtime did not appear for sibling restore\n");
                return diagnostic.finish(5);
            }
            if (trace != nullptr) trace->record(loader_diag::Stage::RuntimeOpenBegin, runtime_target);
            const int opened_runtime = primary_runtime.open(context, runtime_loc);
            if (trace != nullptr) trace->record(loader_diag::Stage::RuntimeOpenResult,
                runtime_target, opened_runtime, loader_diag::Domain::Libusb, opened_runtime != 0);
            if (opened_runtime != 0) {
                std::fprintf(stderr, "firmware: cannot open primary runtime for sibling restore: %s\n",
                             libusb_error_name(opened_runtime));
                return diagnostic.finish(exit_status(asicen::map_libusb_acquisition_error(opened_runtime)));
            }
            if (trace != nullptr) trace->record(loader_diag::Stage::RuntimeClaimBegin, runtime_target);
            const int claimed_runtime = primary_runtime.claim_interface(0);
            if (trace != nullptr) trace->record(loader_diag::Stage::RuntimeClaimResult,
                runtime_target, claimed_runtime, loader_diag::Domain::Libusb, claimed_runtime != 0);
            if (claimed_runtime != 0) {
                std::fprintf(stderr, "firmware: cannot claim primary runtime for sibling restore: %s\n",
                             libusb_error_name(claimed_runtime));
                return diagnostic.finish(exit_status(asicen::map_libusb_acquisition_error(claimed_runtime)));
            }
            const int restore_rc = restore_sibling_loader(primary_runtime, trace, runtime_target);
            if (restore_rc != 0) return diagnostic.finish(restore_rc);
            sibling_restored = true;
        }
        // The sibling loader re-enumerates at the second --usb-path topology.
        // Loop again; resolve_device on usb_paths[1] finds the fresh loader.
    }
    if (any_transferred) {
        std::fprintf(stderr, "firmware: loader transfer complete for %zu function(s)\n",
                     target_count);
        return diagnostic.finish(0);
    }
    if (any_warm) return diagnostic.finish(kLoadFirmwareWarm);
    return diagnostic.finish(3);
}

// Validate the current runtime match for one successfully uploaded topology.
// The readiness helper re-resolves every identity on each polling pass.
px4::userland::Error probe_uploaded_runtime(
    libusb_context* context, const asicen::DeviceProfile* expected_model,
    const asicen::UploadedRuntimeIdentity& uploaded)
{
    if (expected_model == nullptr || uploaded.vendor_id != expected_model->vid ||
        uploaded.product_id != expected_model->pid)
        return px4::userland::Error::INVALID_ARGUMENT;
    const auto candidate = resolve_runtime_at_parent(
        context, uploaded.bus, uploaded.port_path, expected_model);
    return candidate ? px4::userland::Error::OK : candidate.error();
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
        firmware = asicen::FirmwareProvider(options.firmware).load();
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
    if (!options.device_selector.empty()) {
        const auto selected = resolve_device_selector(context, &options);
        if (selected != px4::userland::Error::OK) {
            std::fprintf(stderr, "device selector: %s\n", px4::userland::error_string(selected));
            return exit_status(selected);
        }
    }
    if (options.file_descriptor_count != 0U) {
        const auto inferred = infer_file_descriptor_model(context, &options);
        if (inferred != px4::userland::Error::OK) {
            std::fprintf(stderr, "file descriptor model: %s\n",
                         px4::userland::error_string(inferred));
            return exit_status(inferred);
        }
        if (options.expected_model == nullptr &&
            options.file_descriptor_count == 2U && !options.firmware.empty()) {
            std::fprintf(stderr, "firmware: loader FDs without a runtime model must be provisioned one at a time\n");
            return exit_status(px4::userland::Error::NOT_READY);
        }
    }
    if (options.file_descriptor_count == 2U && options.expected_model != nullptr &&
        options.expected_model->expected_runtime_functions == 2U) {
        const auto first_identity = observed_identity(context, options, 0U);
        const auto second_identity = observed_identity(context, options, 1U);
        const auto first_path = observed_port_path(context, options, 0U);
        const auto second_path = observed_port_path(context, options, 1U);
        if (!first_identity || !second_identity || first_identity.value().empty() ||
            first_identity.value() != second_identity.value() || !first_path || !second_path ||
            first_path.value().size() < 2U || first_path.value().size() != second_path.value().size()) {
            std::fprintf(stderr, "refusing selected FDs without one observable sibling topology\n");
            return 2;
        }
        const auto& a = first_path.value();
        const auto& b = second_path.value();
        if (!std::equal(a.begin(), a.end() - 1, b.begin()) ||
            !((a.back() == 1U && b.back() == 2U) ||
              (a.back() == 2U && b.back() == 1U))) {
            std::fprintf(stderr, "refusing selected FDs outside the same ordered sibling paths\n");
            return 2;
        }
        if (a.back() == 2U) std::swap(options.file_descriptors[0], options.file_descriptors[1]);
    }
    auto identity = observed_identity(context, options);
    if (!identity) {
        std::fprintf(stderr, "enclosure identity: %s\n", px4::userland::error_string(identity.error()));
        return exit_status(identity.error());
    }
    if (identity.value().empty()) {
        const bool pathless_single_runtime = options.expected_model != nullptr &&
            options.file_descriptor_count == 1U &&
            options.expected_model->expected_runtime_functions == 1U &&
            options.firmware.empty();
        const bool selected_loader_for_transfer = options.expected_model != nullptr &&
            options.file_descriptor_count != 0U && !options.firmware.empty() &&
            options.file_descriptor_count <= options.expected_model->expected_runtime_functions;
        if (!pathless_single_runtime &&
            (options.expected_model == nullptr || options.file_descriptor_count == 0U ||
             !selected_loader_for_transfer)) {
            std::fprintf(stderr, "enclosure identity: runtime topology unavailable; loader transfer requires selected model functions\n");
            return exit_status(px4::userland::Error::NOT_READY);
        }
        if (pathless_single_runtime) {
            // Keep the selected borrowed FD usable without presenting an
            // instance token as an observed physical identity.
        } else {
            if (options.expected_model->expected_runtime_functions == 2U &&
                options.file_descriptor_count == 2U) {
                std::fprintf(stderr, "firmware: paired loader FDs require observable sibling topology\n");
                return exit_status(px4::userland::Error::NOT_READY);
            }
            std::array<std::uint8_t, 2U> buses{};
            std::array<std::uint8_t, 2U> addresses{};
            for (std::size_t index = 0U; index < options.file_descriptor_count; ++index) {
                asicen::LibusbDevice device;
                const int opened = device.open(context, options.file_descriptors[index]);
                if (opened != 0) {
                    std::fprintf(stderr, "firmware: cannot open selected loader FD: %s\n",
                                 px4::userland::error_string(
                                     asicen::map_libusb_acquisition_error(opened)));
                    return exit_status(asicen::map_libusb_acquisition_error(opened));
                }
                libusb_device_descriptor descriptor{};
                const int described = device.descriptor(&descriptor);
                if (described != 0) {
                    std::fprintf(stderr, "firmware: descriptor read failed: %s\n",
                                 libusb_error_name(described));
                    return exit_status(asicen::map_libusb_error(described));
                }
                const bool loader = descriptor.idVendor == 0x1738U &&
                    (descriptor.idProduct == 0x5211U || descriptor.idProduct == 0x5216U);
                if (!loader) {
                    std::fprintf(stderr, "firmware: selected pathless FD is not an ASICEN loader\n");
                    return exit_status(px4::userland::Error::UNSUPPORTED);
                }
                buses[index] = static_cast<std::uint8_t>(libusb_get_bus_number(device.device()));
                addresses[index] = static_cast<std::uint8_t>(
                    libusb_get_device_address(device.device()));
                for (std::size_t previous = 0U; previous < index; ++previous) {
                    if (buses[previous] == buses[index] && addresses[previous] == addresses[index]) {
                        std::fprintf(stderr, "firmware: selected FDs refer to the same USB function\n");
                        return exit_status(px4::userland::Error::INVALID_ARGUMENT);
                    }
                }
                libusb_config_descriptor* config = nullptr;
                const int configured = libusb_get_config_descriptor(device.device(), 0U, &config);
                if (configured != 0 || config == nullptr) {
                    if (config != nullptr) libusb_free_config_descriptor(config);
                    std::fprintf(stderr, "firmware: loader configuration read failed: %s\n",
                                 libusb_error_name(configured));
                    return exit_status(configured == 0
                        ? px4::userland::Error::UNSUPPORTED
                        : asicen::map_libusb_error(configured));
                }
                const bool interface0 = config->bNumInterfaces == 1U &&
                    config->interface[0].num_altsetting == 1 &&
                    config->interface[0].altsetting[0].bInterfaceNumber == 0U &&
                    config->interface[0].altsetting[0].bAlternateSetting == 0U;
                libusb_free_config_descriptor(config);
                if (!interface0) {
                    std::fprintf(stderr, "firmware: loader interface profile mismatch\n");
                    return exit_status(px4::userland::Error::UNSUPPORTED);
                }
            }
            const int transferred = load_loader_firmware(
                owned_context, options, firmware.value(), nullptr, nullptr);
            if (transferred != 0) return transferred;
            std::fprintf(stderr, "firmware: loader transfer complete; grant the runtime USB function(s) and rerun without --firmware\n");
            return 3;
        }
    }
    if (options.instance.empty()) {
        options.instance = identity.value().empty()
            ? "fd-" + std::to_string(options.file_descriptors[0])
            : identity.value();
    }
    if (options.file_descriptor_count == 1U && options.expected_model != nullptr &&
        options.expected_model->expected_runtime_functions == 2U && options.firmware.empty()) {
        const auto primary_path = observed_port_path(context, options, 0U);
        if (!primary_path || primary_path.value().size() < 2U ||
            primary_path.value().back() != 1U) {
            std::fprintf(stderr, "sibling restore: selected FD is not at the primary USB path\n");
            return primary_path ? 2 : exit_status(primary_path.error());
        }
        asicen::LibusbDevice primary_device;
        const int opened = primary_device.open(context, options.file_descriptors[0]);
        if (opened != 0) {
            std::fprintf(stderr, "sibling restore: cannot open selected primary FD: %s\n",
                         libusb_error_name(opened));
            return exit_status(asicen::map_libusb_acquisition_error(opened));
        }
        asicen::LibusbFunctionClaim primary(primary_device, "primary");
        const asicen::UsbFunctionSnapshot snapshot = primary.snapshot();
        if (snapshot.transport_error != 0 ||
            snapshot.vendor_id != options.expected_model->vid ||
            snapshot.product_id != options.expected_model->pid ||
            !snapshot.interface0_present || !snapshot.kernel_driver_state_known ||
            snapshot.interface0_kernel_driver || snapshot.active_alt0 != 0 ||
            !snapshot.endpoint82_in_alt0 || !snapshot.endpoint82_bulk_in_alt0) {
            std::fprintf(stderr, "sibling restore: selected FD is not a usable primary runtime function\n");
            return exit_status(snapshot.transport_error != 0
                ? asicen::map_libusb_error(snapshot.transport_error)
                : px4::userland::Error::UNSUPPORTED);
        }
        const int claimed = primary.claim_interface0();
        if (claimed != 0) {
            std::fprintf(stderr, "sibling restore: interface 0 claim failed: %s\n",
                         libusb_error_name(claimed));
            return exit_status(asicen::map_libusb_acquisition_error(claimed));
        }
        loader_diag::Target target{};
        target.role = loader_diag::Role::Primary;
        target.selected_index = 0U;
        target.ids_known = true;
        target.vendor_id = snapshot.vendor_id;
        target.product_id = snapshot.product_id;
        const int restored = restore_sibling_loader(primary_device, nullptr, target);
        (void)primary.release_interface0();
        if (restored != 0) return restored;
        std::fprintf(stderr, "sibling restore: requested; grant the sibling loader FD and transfer its firmware\n");
        return 3;
    }
    // Freeze address selectors to the observed topology before a firmware
    // upload can change addresses. Every selected function must belong to the
    // same enclosure; an absent sibling is allowed only at its exact declared
    // port while a source-backed loader restore is pending.
    const std::size_t selected_count = options.file_descriptor_count != 0U
        ? options.file_descriptor_count : options.usb_path_count;
    std::vector<std::vector<std::uint8_t>> selected_fd_paths;
    for (std::size_t index = 0U; index < selected_count; ++index) {
        const auto selected = observed_identity(context, options, index);
        if (!selected) {
            asicen::UsbFunctionObservation pending;
            if (index == 0U || options.firmware.empty() ||
                options.file_descriptor_count != 0U ||
                (options.expected_model == nullptr && options.usb_path_count != 2U) ||
                (selected.error() != px4::userland::Error::NOT_FOUND &&
                 selected.error() != px4::userland::Error::NOT_READY) ||
                !parse_bus_port(options.usb_paths[index], &pending.bus, &pending.port_path) ||
                asicen::cli::topology_identity(pending,
                    options.expected_model != nullptr
                        ? options.expected_model->expected_runtime_functions == 2U
                        : options.usb_path_count == 2U) != identity.value()) {
                std::fprintf(stderr, "enclosure identity: %s\n",
                             px4::userland::error_string(selected.error()));
                return exit_status(selected.error());
            }
        } else if (selected.value().empty() && options.file_descriptor_count == 1U &&
                   options.expected_model != nullptr &&
                   options.expected_model->expected_runtime_functions == 1U &&
                   options.firmware.empty()) {
            // The granted single-function FD remains the selected target;
            // its explicit instance names only the IPC namespace.
        } else if (selected.value() != identity.value()) {
            std::fprintf(stderr, "refusing functions from different enclosures\n");
            return 2;
        }
        if (options.file_descriptor_count != 0U) {
            const auto path = observed_port_path(context, options, index);
            const bool paired = options.expected_model != nullptr &&
                options.expected_model->expected_runtime_functions == 2U;
            const bool single_loader_target = paired && selected_count == 1U &&
                !options.firmware.empty();
            const bool canonical_paired_path = !paired || (path && path.value().size() >= 2U &&
                (single_loader_target
                    ? (path.value().back() == 1U || path.value().back() == 2U)
                    : path.value().back() == (index == 0U ? 1U : 2U)));
            if ((!path || path.value().empty()) && paired) {
                std::fprintf(stderr, "refusing unknown or noncanonical USB function path\n");
                return path ? 2 : exit_status(path.error());
            }
            if (path && !path.value().empty() && !canonical_paired_path) {
                std::fprintf(stderr, "refusing unknown or noncanonical USB function path\n");
                return 2;
            }
            if (path && !path.value().empty() && !selected_fd_paths.empty()) {
                const auto& primary_path = selected_fd_paths.front();
                const auto& sibling_path = path.value();
                if (primary_path.size() != sibling_path.size() ||
                    !std::equal(primary_path.begin(), primary_path.end() - 1,
                                sibling_path.begin()) || primary_path.back() != 1U ||
                    sibling_path.back() != 2U) {
                    std::fprintf(stderr, "refusing functions outside the ordered sibling USB paths\n");
                    return 2;
                }
            }
            if (path && !path.value().empty()) selected_fd_paths.push_back(path.value());
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
    std::optional<dtv::userland::host_listener_context::IdentityLease> enclosure_lease;
    if (!identity.value().empty()) {
        dtv::userland::host_listener_context::IdentityLeaseDiagnostic lease_diagnostic{};
        auto acquired = asicen::cli::acquire_identity_lease(
            options.runtime_directory, options.group, identity.value(), &lease_diagnostic);
        if (!acquired) {
            if (lease_diagnostic.reason !=
                dtv::userland::host_listener_context::IdentityLeaseDiagnosticReason::none) {
                asicen::cli::print_identity_lease_diagnostic(stderr, lease_diagnostic,
                                                              options.group);
            } else {
                std::fprintf(stderr, "enclosure lease: %s\n",
                             px4::userland::error_string(acquired.error()));
            }
            return exit_status(acquired.error());
        }
        enclosure_lease.emplace(std::move(acquired.value()));
    }
    // Native selectors are now observed port paths, so address changes after
    // firmware transfer cannot redirect the later hardware claim.
    bool sibling_firmware_uploaded = false;
    if (!options.firmware.empty()) {
        // px4-compatible --firmware: upload the verified loader image, then
        // wait for the runtime to re-enumerate at the same topology before
        // claiming. A non-loader addressed device is treated as a warm runtime
        // (firmware already present) and claim proceeds without an upload.
        LoaderIdentity loader_identity;
        std::vector<asicen::UploadedRuntimeIdentity> uploaded_identities;
        const int loaded = load_loader_firmware(owned_context, options, firmware.value(),
                                                &loader_identity, &uploaded_identities);
        context = owned_context.get();
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
            if (options.expected_model != nullptr &&
                options.expected_model->expected_runtime_functions != selected_count) {
                std::fprintf(stderr, "device selector: selected function count does not match runtime model\n");
                return exit_status(px4::userland::Error::INVALID_ARGUMENT);
            }
            sibling_firmware_uploaded = std::any_of(
                uploaded_identities.begin(), uploaded_identities.end(),
                [](const asicen::UploadedRuntimeIdentity& identity) {
                    return identity.selected_index == 1U;
                });
            const auto readiness = asicen::wait_for_uploaded_runtimes(
                uploaded_identities,
                [context, model = options.expected_model](
                    const asicen::UploadedRuntimeIdentity& identity) {
                    return probe_uploaded_runtime(context, model, identity);
                },
                [] { return std::chrono::steady_clock::now(); },
                [](std::chrono::milliseconds interval) {
                    sleep_ms(static_cast<std::uint32_t>(interval.count()));
                },
                [] { return px4::userland::px4d::stop_requested(); });
            if (readiness != px4::userland::Error::OK) {
                std::fprintf(stderr, "firmware: runtime did not re-enumerate: %s\n",
                             px4::userland::error_string(readiness));
                return exit_status(readiness);
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
                if (primary_location.bus != sibling_location.bus || primary_path.empty() ||
                    sibling_path.empty() ||
                    primary_path == sibling_path ||
                    primary_path.back() != 1U || sibling_path.back() != 2U) {
                    std::fprintf(stderr, "refusing noncanonical primary/sibling port assignment\n");

                    return 2;
                }
            }
            hardware.reset(new (std::nothrow) asicen::LibusbW3u3Hardware(
                context, primary_location, sibling_location,
                std::move(primary_path), std::move(sibling_path), options.expected_model,
                sibling_firmware_uploaded));
        }
        if (!hardware) return 70;
        hardware->set_allow_lnb_power(options.allow_lnb_power);
        const auto claimed = hardware->claim();
        if (!claimed) {
            std::fprintf(stderr, "hardware model/topology/claim refused (dual-function models require sibling paths): %s\n",
                         px4::userland::error_string(claimed.error()));
            result = exit_status(claimed.error());
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
