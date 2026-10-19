// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicend_args.h"
#include "asicend_fresh_usb.h"
#include "asicend_identity.h"
#include "loader_acquisition.h"
#include "loader_diagnostic.h"
#include "px4d_signals.h"
#if defined(_WIN32)
#include "px4d_windows_stdin.h"
#endif
#include "asicen/card_only_service.h"
#include "asicen/libusb_hardware_backend.h"
#include "asicen/libusb_transport.h"
#include "asicen/loader_firmware.h"
#include "asicen/startup_readiness.h"
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
namespace loader_diag = asicen::cli::loader_diagnostic;

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

// Resolves one --device position (BUS-PORT; BUS:ADDRESS internally) to the opened
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

// A runtime's enclosure identity follows its own profile. A loader has no
// model yet, so its identity follows the declared function count until the
// re-enumerated runtime VID:PID decides the model. A loader primary needs
// firmware; a loader sibling may remain reserved behind a warm primary.
px4::userland::Result<std::string> observed_identity(
    libusb_context* context, const Options& options, std::size_t index = 0U,
    asicen::cli::SelectedFunction* selected_function = nullptr)
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
    const asicen::cli::SelectedFunction function =
        asicen::cli::classify_selected_function(descriptor.idVendor, descriptor.idProduct);
    const std::size_t selected_count = options.file_descriptor_count != 0U
        ? options.file_descriptor_count : options.usb_path_count;
    bool dual = false;
    if (function.state == asicen::cli::SelectedState::runtime) {
        dual = function.profile->expected_runtime_functions == 2U;
    } else if (function.state == asicen::cli::SelectedState::loader &&
               (!options.firmware.empty() || index != 0U)) {
        dual = selected_count == 2U;
    } else {
        return px4::userland::Result<std::string>::failure(px4::userland::Error::UNSUPPORTED);
    }
    if (selected_function != nullptr) *selected_function = function;
    asicen::UsbFunctionObservation observation;
    observation.bus = libusb_get_bus_number(device.device());
    std::uint8_t ports[8]{};
    const int count = libusb_get_port_numbers(device.device(), ports, sizeof(ports));
    if (count <= 0 || count > static_cast<int>(sizeof(ports)))
        return px4::userland::Result<std::string>::failure(px4::userland::Error::NOT_READY);
    observation.port_path.assign(ports, ports + count);
    const std::string identity = asicen::cli::topology_identity(observation, dual);
    if (identity.empty())
        return px4::userland::Result<std::string>::failure(px4::userland::Error::NOT_READY);
    return px4::userland::Result<std::string>::success(identity);
}

// Loads firmware into a loader device addressed by --device position or fd. The
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

// Pushes the verified shared loader image to one claimed loader, preserving USB
// and firmware error classes rather than reporting an internal error for each.
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
    const std::vector<asicen::LoaderTransfer> plan =
        asicen::build_common_loader_transfer_plan();
    if (plan.empty()) { std::fprintf(stderr, "firmware: no shared loader transfer plan\n"); return 10; }
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

// Re-enumeration must preserve the frozen loader port path. The runtime
// VID:PID found there decides the model; nothing is expected in advance.
// Address changes are expected; widening to a different enclosure is not.
// Each call observes in a fresh context, never the pre-upload one.
px4::userland::Result<asicen::UsbLocation> resolve_runtime_at_parent(
    std::uint8_t bus, const std::vector<std::uint8_t>& loader_port_path,
    const asicen::DeviceProfile** runtime_profile,
    loader_diag::Target* diagnostic_target = nullptr)
{
    if (runtime_profile == nullptr || loader_port_path.empty())
        return px4::userland::Result<asicen::UsbLocation>::failure(
            px4::userland::Error::INVALID_ARGUMENT);
    std::vector<asicen::UsbFunctionObservation> observations;
    const px4::userland::Error observed = asicen::cli::observe_bus_fresh(
        asicen::native_libusb_acquisition_api(), bus, &observations);
    if (observed != px4::userland::Error::OK)
        return px4::userland::Result<asicen::UsbLocation>::failure(observed);
    const asicen::cli::RuntimeAtPosition found =
        asicen::cli::find_runtime_at_position(observations, bus, loader_port_path);
    if (found.error != px4::userland::Error::OK)
        return px4::userland::Result<asicen::UsbLocation>::failure(found.error);
    const asicen::UsbFunctionObservation& selected = observations[found.index];
    if (diagnostic_target != nullptr) {
        diagnostic_target->location_known = true;
        diagnostic_target->bus = selected.bus;
        diagnostic_target->address = selected.address;
        diagnostic_target->port_count = static_cast<std::uint8_t>(selected.port_path.size());
        std::copy(selected.port_path.begin(), selected.port_path.end(),
                  diagnostic_target->ports.begin());
        diagnostic_target->ids_known = true;
        diagnostic_target->vendor_id = selected.vid;
        diagnostic_target->product_id = selected.pid;
    }
    *runtime_profile = found.profile;
    return px4::userland::Result<asicen::UsbLocation>::success(
        asicen::UsbLocation{selected.bus, selected.address});
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
                         LoaderIdentity* loader_identity,
                         std::vector<asicen::UploadedRuntimeIdentity>* uploaded_identities) {
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
        // After the sibling restore, each acquisition attempt resolves and
        // opens in a context created for that attempt; it outlives the device.
        asicen::LibusbContext refreshed;
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
                    libusb_context* attempt_context = context;
                    if (sibling_restored) {
                        const int fresh = asicen::cli::refresh_context(refreshed, &device);
                        if (fresh != 0) {
                            open_error = asicen::map_libusb_acquisition_error(fresh);
                            return acquisition::Attempt::Failed;
                        }
                        attempt_context = refreshed.get();
                    }
                    if (trace != nullptr) trace->record(loader_diag::Stage::ResolveBegin, target);
                    const auto resolved = resolve_device(attempt_context, options.usb_paths[i]);
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
                    opened = device.open(attempt_context, resolved.value().location);
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
        const bool loader = asicen::cli::is_loader_id(desc.idVendor, desc.idProduct);
        if (!loader) {
            if (asicen::find_profile(desc.idVendor, desc.idProduct) == nullptr) {
                std::fprintf(stderr, "firmware: addressed device is neither an ASICEN loader "
                                     "nor a known runtime\n");
                return diagnostic.finish(3);
            }
            std::fprintf(stderr, "firmware: addressed device is not an ASICEN loader; treating as warm runtime\n");
            any_warm = true;
            continue;
        }
        // Freeze the selected loader topology before transfer changes its
        // address. Only successful uploads enter the final readiness set. The
        // runtime VID:PID is left unknown until it re-enumerates there.
        std::uint8_t ports[8]{};
        const int nports = libusb_get_port_numbers(
            device.device(), ports, static_cast<int>(sizeof(ports)));
        if (nports <= 0 || nports > static_cast<int>(sizeof(ports))) {
            if (trace != nullptr) trace->record(loader_diag::Stage::TopologyResult,
                target, nports, loader_diag::Domain::Libusb, true);
            std::fprintf(stderr, "firmware: loader topology unavailable before transfer\n");
            return diagnostic.finish(exit_status(px4::userland::Error::NOT_READY));
        }
        asicen::UploadedRuntimeIdentity uploaded{};
        uploaded.selected_index = i;
        uploaded.bus = static_cast<std::uint8_t>(libusb_get_bus_number(device.device()));
        uploaded.port_path.assign(ports, ports + nports);
        if (trace != nullptr) {
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
        if (loader_identity != nullptr) {
            const bool primary = uploaded.port_path.back() == 1U;
            if (loader_identity->port_path.empty() || primary) {
                loader_identity->bus = uploaded.bus;
                loader_identity->port_path = uploaded.port_path;
            }
        }
        const int rc = transfer_firmware_to_loader(device, firmware, trace, target);
        if (rc != 0) return diagnostic.finish(rc);
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
            // The runtime is opened in a context created after it was observed;
            // the context is declared first so the handle closes before it exits.
            asicen::LibusbContext runtime_context;
            asicen::LibusbDevice primary_runtime;
            asicen::UsbLocation runtime_loc{};
            const asicen::DeviceProfile* runtime_profile = nullptr;
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
                    loader_identity->bus, loader_identity->port_path, &runtime_profile,
                    trace != nullptr ? &runtime_target : nullptr);
                if (candidate.has_value()) {
                    runtime_loc = candidate.value();
                    runtime_found = true;
                    if (trace != nullptr) trace->record(loader_diag::Stage::PrimaryRuntimeFound,
                        runtime_target);
                    break;
                }
                if (trace != nullptr) runtime_error = candidate.error();
                if (candidate.error() == px4::userland::Error::UNSUPPORTED) {
                    std::fprintf(stderr, "firmware: unknown VID:PID re-enumerated at the primary position\n");
                    return diagnostic.finish(3);
                }
                sleep_ms(200U);
            }
            if (!runtime_found) {
                if (trace != nullptr) trace->record(loader_diag::Stage::PrimaryRuntimeResult,
                    runtime_target, static_cast<int>(runtime_error), loader_diag::Domain::Px4, true);
                std::fprintf(stderr, "firmware: primary runtime did not appear for sibling restore\n");
                return diagnostic.finish(5);
            }
            // The runtime VID:PID now decides the model. Only a dual-function
            // model restores the declared sibling; no other position is probed.
            if (runtime_profile->expected_runtime_functions != 2U) {
                std::fprintf(stderr, "firmware: %s runtime has one function; refusing the "
                                     "declared sibling position\n", runtime_profile->model_key);
                return diagnostic.finish(2);
            }
            if (trace != nullptr) trace->record(loader_diag::Stage::RuntimeOpenBegin, runtime_target);
            int opened_runtime = asicen::cli::refresh_context(runtime_context, &primary_runtime);
            if (opened_runtime == 0)
                opened_runtime = primary_runtime.open(runtime_context.get(), runtime_loc);
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
        // The sibling loader re-enumerates at the declared sibling position.
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

// Observe the runtime at one successfully uploaded topology and record the
// profile of its VID:PID. The readiness helper re-resolves every identity on
// each polling pass; a loader still there is NOT_FOUND until the deadline.
px4::userland::Error probe_uploaded_runtime(
    const asicen::UploadedRuntimeIdentity& uploaded,
    const asicen::DeviceProfile** runtime_profile)
{
    const auto candidate = resolve_runtime_at_parent(
        uploaded.bus, uploaded.port_path, runtime_profile);
    return candidate ? px4::userland::Error::OK : candidate.error();
}
}  // namespace

int run_asicend_hardware(int argc, char** argv) {
    Options options = asicen::cli::parse_daemon_arguments(
        argc, const_cast<const char* const*>(argv));
    if (!options.valid) {
        std::fprintf(stderr, "argument error: %.*s\n",
                     static_cast<int>(options.error.size()), options.error.data());
        return options.error_status;
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
    std::unique_ptr<asicen::LibusbContext> owned_context(new (std::nothrow)
        asicen::LibusbContext());
    if (!owned_context) return 70;
    const int initialized = owned_context->initialize(options.file_descriptor_count != 0U);
    if (initialized != 0) {
        std::fprintf(stderr, "libusb_init: %s\n", libusb_error_name(initialized));
        return exit_status(asicen::map_libusb_error(initialized));
    }
    libusb_context* context = owned_context->get();
    std::array<asicen::cli::SelectedFunction, 2U> selected_functions{};
    const auto identity = observed_identity(context, options, 0U, &selected_functions[0]);
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
        const auto selected = observed_identity(context, options, index,
                                                &selected_functions[index]);
        if (!selected) {
            asicen::UsbFunctionObservation pending;
            selected_functions[index] = asicen::cli::SelectedFunction{};
            if (index == 0U || options.firmware.empty() ||
                options.file_descriptor_count != 0U ||
                (selected.error() != px4::userland::Error::NOT_FOUND &&
                 selected.error() != px4::userland::Error::NOT_READY) ||
                !parse_bus_port(options.usb_paths[index], &pending.bus, &pending.port_path) ||
                asicen::cli::topology_identity(pending, true) != identity.value()) {
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
    // Decide warm or cold from the observed VID:PIDs before any transfer. A
    // warm runtime decides the model now; a cold one only after it re-enumerates.
    const asicen::cli::StartupDecision startup = asicen::cli::decide_startup(
        selected_functions.data(), selected_count, !options.firmware.empty());
    if (startup.kind == asicen::cli::StartupKind::refused) {
        std::fprintf(stderr, "startup: %s\n", px4::userland::error_string(startup.error));
        return exit_status(startup.error);
    }
    if (options.usb_path_count != 0U && startup.profile != nullptr &&
        startup.profile->expected_runtime_functions != selected_count) {
        std::fprintf(stderr, "refusing %s: --device must name its %u function position(s)\n",
                     startup.profile->model_key,
                     static_cast<unsigned>(startup.profile->expected_runtime_functions));
        return 2;
    }
    const asicen::DeviceProfile* runtime_profile = startup.profile;
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
    bool sibling_firmware_uploaded = false;
    if (startup.kind == asicen::cli::StartupKind::warm && !options.firmware.empty()) {
        std::fprintf(stderr, "firmware: selected functions are already runtime; skipping upload\n");
    } else if (startup.kind == asicen::cli::StartupKind::cold) {
        // px4-compatible --firmware: upload the verified shared loader image,
        // then wait for a runtime to re-enumerate at the same topology before
        // claiming. A non-loader addressed device is treated as a warm runtime
        // (firmware already present) and claim proceeds without an upload.
        LoaderIdentity loader_identity;
        std::vector<asicen::UploadedRuntimeIdentity> uploaded_identities;
        const int loaded = load_loader_firmware(context, options, firmware.value(),
                                                &loader_identity, &uploaded_identities);
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
            sibling_firmware_uploaded = std::any_of(
                uploaded_identities.begin(), uploaded_identities.end(),
                [](const asicen::UploadedRuntimeIdentity& identity) {
                    return identity.selected_index == 1U;
                });
            std::array<const asicen::DeviceProfile*, 2U> uploaded_profiles{};
            const auto readiness = asicen::wait_for_uploaded_runtimes(
                uploaded_identities,
                [&uploaded_profiles](const asicen::UploadedRuntimeIdentity& identity) {
                    if (identity.selected_index >= uploaded_profiles.size())
                        return px4::userland::Error::INVALID_ARGUMENT;
                    return probe_uploaded_runtime(identity,
                                                  &uploaded_profiles[identity.selected_index]);
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
            // The primary's re-enumerated VID:PID decides the model unless a
            // warm primary already did. A sibling that reports another model
            // is refused by the strict claim before any interface is claimed.
            if (runtime_profile == nullptr) runtime_profile = uploaded_profiles[0];
            if (runtime_profile == nullptr) {
                std::fprintf(stderr, "firmware: primary runtime model was not observed\n");
                return 70;
            }
            std::fprintf(stderr, "firmware: runtime model %s observed at the selected position\n",
                         runtime_profile->model_key);
            if (runtime_profile->expected_runtime_functions != selected_count) {
                std::fprintf(stderr, "refusing %s: --device must name its %u function position(s)\n",
                             runtime_profile->model_key,
                             static_cast<unsigned>(runtime_profile->expected_runtime_functions));
                return 2;
            }
            // Claim in a context created after re-enumeration, not the
            // pre-upload one. No device handle refers to the old context here.
            const int refreshed = asicen::cli::refresh_context(*owned_context, nullptr);
            if (refreshed != 0) {
                std::fprintf(stderr, "libusb_init: %s\n", libusb_error_name(refreshed));
                return exit_status(asicen::map_libusb_error(refreshed));
            }
            context = owned_context->get();
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
                std::fprintf(stderr, "device: %s\n",
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
                    std::fprintf(stderr, "device: %s\n",
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
                std::move(primary_path), std::move(sibling_path), runtime_profile,
                sibling_firmware_uploaded));
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
            px4::userland::ipc::posix::EnclosurePublication publication;
            publication.runtime_directory = options.runtime_directory;
            publication.instance = options.instance;
            publication.lease_identity = identity.value();
            publication.access = options.group
                ? px4::userland::ipc::posix::EndpointAccess::shared_group
                : px4::userland::ipc::posix::EndpointAccess::private_user;
            auto owned_server = asicen::create_live_card_stream_context(
                std::move(owned_context), std::move(hardware),
                std::move(enclosure_lease.value()), publication, nullptr);
            if (!owned_server) {
                result = exit_status(owned_server.error());
            } else {
                auto owner = std::move(owned_server.value());
                const auto served = asicen::run_live_card_stream_server(
                    *owner, nullptr, &px4::userland::px4d::stop_requested,
                    std::chrono::milliseconds{15000});
                if (served.close_deadline_expired)
                    px4::userland::px4d::exit_after_close_deadline(70);
                result = served.exit_status;
                owner.reset();
            }
        }
        if (claimed && hardware) {
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
