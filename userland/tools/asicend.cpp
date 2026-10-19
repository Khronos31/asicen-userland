// SPDX-License-Identifier: GPL-2.0-or-later
// asicend: real-hardware daemon.  Mirrors px4d: native (--device usb:) or fd
// modes are selected by their options, and read-only discovery uses
// --list-json.  The isolated mock service lives in the separate asicend-mock
// binary; this binary never runs a mock backend.
#include "asicen/product_profile.h"
#include "asicen/device_profile.h"
#include "asicen/enclosure_grouping.h"
#include "asicend_args.h"
#include "asicend_list_format.h"

#include <dtv/userland/json.h>

#include <array>
#if defined(_WIN32)
#include "px4_windows_args.h"
#endif
#ifdef ASICEN_ENABLE_LIBUSB
#include "asicen/libusb_transport.h"
#include <libusb.h>
#endif

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef ASICEN_ENABLE_LIBUSB
int run_asicend_hardware(int argc, char** argv);
#endif

namespace {

void usage(FILE* output)
{
#ifdef ASICEN_ENABLE_LIBUSB
    std::fprintf(output,
        "usage: asicend --list | --list-json | --version\n"
        "       asicend --models\n"
        "       asicend -d|--device usb:BUS-PORT[,BUS-PORT]\n"
        "                 [--runtime-dir PATH] --instance TOKEN "
        "[--group] [--allow-lnb-power] [-F|--firmware PATH]\n"
        "       asicend --fd FD [--fd FD]\n"
        "                 [--runtime-dir PATH] [--instance TOKEN] "
        "[--group] [--allow-lnb-power] [-F|--firmware PATH]\n"
        "  -d, --device usb:BUS-PORT[,BUS-PORT]  enclosure USB function positions:\n"
        "               one for single-function models, both .1 and .2 functions\n"
        "               for dual-function models; order does not assign roles.\n"
        "               serial: matches nothing (ASICEN has no serial descriptor).\n"
        "  --fd         granted USB descriptors; up to two, primary first.\n"
        "               Without --instance, use the observed usb-BUS-PORT identity.\n"
        "  --allow-lnb-power  permit explicit ISDB-S 15 V requests; default off\n"
        "  -F, --firmware  shared loader image; transferred only to loader\n"
        "               functions. The model is taken from the runtime VID:PID\n"
        "               that re-enumerates at the same position.\n"
        "  --probe-satellite RF_KHZ [--slot 0..7]  bounded legacy satellite diagnostic\n"
        "  --probe-card  bounded legacy card diagnostic\n"
        "  --card-only   serve the card reader without tuner operations\n"
        "  --models     list supported source-backed models and exit\n"
        "  --list       print connected supported enclosures and exit;\n"
        "  --list-json  print the same data as compact JSON and exit;\n"
        "               read-only, needs neither firmware nor a daemon\n"
#if defined(_WIN32)
        "  --exit-on-stdin-eof  exit with cleanup when stdin reaches EOF\n"
#endif
    );
#else
    std::fprintf(output,
        "usage: asicend --models | --list-json | --version\n"
        "The hardware backend is unavailable in this libusb-OFF build.\n");
#endif
}

int list_models()
{
    namespace w = dtv::userland::wire;
    // The list is a static set of supported product models, independent of
    // what USB hardware happens to be connected or loaded right now.
    std::array<w::SupportedModel, 5U> records{};
    if (asicen::profile_count() != records.size()) return 70;
    for (std::size_t i = 0U; i < records.size(); ++i) {
        const auto& profile = asicen::profiles()[i];
        const std::size_t count = std::strlen(profile.model);
        if (count == 0U || count > records[i].name.bytes.size()) return 70;
        records[i].name.length = static_cast<std::uint16_t>(count);
        std::memcpy(records[i].name.bytes.data(), profile.model, count);
        // A shared loader is not a unique model identifier. Both the loader
        // and the model-specific runtime USB IDs are supported.
        records[i].usb_ids.count = 2U;
        records[i].usb_ids.items[0] = {0x1738U, 0x5211U};
        records[i].usb_ids.items[1] = {profile.vid, profile.pid};
    }
    const w::SupportedModels models{{records.data(), records.size()}};
    const auto size = w::models_text_size(models);
    if (!size) return 70;
    std::vector<std::uint8_t> text(size.value());
    const auto written = w::encode_models_text(models, {text.data(), text.size()});
    if (!written || written.value() != text.size() ||
        std::fwrite(text.data(), 1U, text.size(), stdout) != text.size() ||
        std::fflush(stdout) != 0) {
        std::fprintf(stderr, "models: write to stdout failed\n");
        return 70;
    }
    return 0;
}

#ifdef ASICEN_ENABLE_LIBUSB
int list_devices(bool json)
{
    libusb_context* context = nullptr;
    const int initialized = asicen::initialize_libusb_context(&context);
    if (initialized != 0) {
        std::fprintf(stderr, "enumeration failed: %s\n",
                     px4::userland::error_string(asicen::map_libusb_error(initialized)));
        return asicen::cli::exit_status(asicen::map_libusb_error(initialized));
    }
    libusb_device** list = nullptr;
    const ssize_t count = libusb_get_device_list(context, &list);
    if (count < 0) {
        std::fprintf(stderr, "enumeration failed: %s\n",
                     px4::userland::error_string(asicen::map_libusb_error(static_cast<int>(count))));
        asicen::release_libusb_context(context);
        return asicen::cli::exit_status(asicen::map_libusb_error(static_cast<int>(count)));
    }
    std::vector<asicen::UsbFunctionObservation> observations;
    for (ssize_t i = 0; i < count; ++i) {
        libusb_device_descriptor descriptor{};
        libusb_device* device = list[i];
        if (libusb_get_device_descriptor(device, &descriptor) != 0) continue;
        if (asicen::find_profile(descriptor.idVendor, descriptor.idProduct) == nullptr &&
            !(descriptor.idVendor == 0x1738U &&
              (descriptor.idProduct == 0x5211U || descriptor.idProduct == 0x5216U))) continue;
        asicen::UsbFunctionObservation observation;
        observation.vid = descriptor.idVendor;
        observation.pid = descriptor.idProduct;
        observation.bus = libusb_get_bus_number(device);
        observation.address = libusb_get_device_address(device);
        if (auto* parent = libusb_get_parent(device)) {
            libusb_device_descriptor hub{};
            if (libusb_get_device_descriptor(parent, &hub) == 0)
                observation.parent_genesys_hub =
                    hub.idVendor == 0x05e3U && hub.idProduct == 0x0608U;
        }
        std::uint8_t ports[8]{};
        const int port_count = libusb_get_port_numbers(device, ports, sizeof(ports));
        if (port_count > 0 && port_count <= static_cast<int>(sizeof(ports)))
            observation.port_path.assign(ports, ports + port_count);
        observations.push_back(std::move(observation));
    }
    libusb_free_device_list(list, 1);
    asicen::release_libusb_context(context);
    const std::string output = json
        ? asicen::cli::format_discovery_json(observations, ASICEN_PRODUCT_VERSION)
        : asicen::cli::format_device_list(observations, false);
    if (json && output.empty()) {
        std::fprintf(stderr, "list-json: unable to represent USB discovery\n");
        return 2;
    }
    if ((!output.empty() &&
         std::fwrite(output.data(), 1U, output.size(), stdout) != output.size()) ||
        std::fflush(stdout) != 0) {
        std::fprintf(stderr, "list: write to stdout failed\n");
        return 70;
    }
    return 0;
}
#endif

}  // namespace

int main(int argc, char** argv)
{
#if defined(_WIN32)
    std::vector<std::string> owned = px4::userland::cli::windows_argv_utf8(argc, argv);
    std::vector<char*> views;
    views.reserve(owned.size());
    for (std::string& value : owned) views.push_back(value.data());
    argc = static_cast<int>(views.size());
    argv = views.data();
#endif
    const auto arguments = asicen::cli::parse_daemon_arguments(
        argc, const_cast<const char* const*>(argv));
    if (!arguments.valid) {
        std::fprintf(stderr, "argument error: %.*s\n",
                     static_cast<int>(arguments.error.size()), arguments.error.data());
        usage(stderr);
        return arguments.error_status;
    }
    if (arguments.help) {
        usage(stdout);
        return 0;
    }
    if (arguments.version) {
        std::printf("asicend %s\n", ASICEN_PRODUCT_VERSION);
        return 0;
    }
    if (arguments.models) return list_models();
    if (arguments.list_json) {
#ifdef ASICEN_ENABLE_LIBUSB
        return list_devices(true);
#else
        const std::string output =
            asicen::cli::format_discovery_json({}, ASICEN_PRODUCT_VERSION);
        if (arguments.list_json && output.empty()) return 2;
        if ((!output.empty() &&
             std::fwrite(output.data(), 1U, output.size(), stdout) != output.size()) ||
            std::fflush(stdout) != 0) {
            std::fprintf(stderr, "list: write to stdout failed\n");
            return 70;
        }
        return 0;
#endif
    }
#ifdef ASICEN_ENABLE_LIBUSB
    return run_asicend_hardware(argc, argv);
#else
    std::fprintf(stderr, "hardware backend unavailable in this libusb-OFF build\n");
    return 3;
#endif
}
