// SPDX-License-Identifier: GPL-2.0-or-later
// asicend: real-hardware daemon.  Mirrors px4d: native (--usb-path) or fd
// modes are selected by their options, and read-only discovery uses
// --list-json.  The isolated mock service lives in the separate asicend-mock
// binary; this binary never runs a mock backend.
#include "asicen/product_profile.h"
#include "asicen/device_profile.h"
#include "asicen/enclosure_grouping.h"
#include "asicend_args.h"
#include "asicend_list_format.h"
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
        "usage: asicend --list | --list-json\n"
        "       asicend --models\n"
        "       asicend --usb-path BUS:ADDRESS|BUS-PORT "
        "[--usb-path BUS:ADDRESS|BUS-PORT] [--model MODEL]\n"
        "                 [--runtime-dir PATH] --instance TOKEN "
        "[--group] [--allow-lnb-power] [--firmware PATH]\n"
        "       asicend --fd FD [--fd FD] [--model MODEL]\n"
        "                 [--runtime-dir PATH] [--instance TOKEN] "
        "[--group] [--allow-lnb-power] [--firmware PATH]\n"
        "  --usb-path   native topology; up to two values, primary first then\n"
        "               sibling. Each value is BUS:ADDRESS or BUS-PORT.\n"
        "  --fd         granted USB descriptors; up to two, primary first.\n"
        "               Without --instance, use the observed usb-BUS-PORT identity.\n"
        "  --allow-lnb-power  permit explicit ISDB-S 15 V requests; default off\n"
        "  --firmware   loader image; loaded before claiming\n"
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
        "usage: asicend --models | --list | --list-json\n"
        "The hardware backend is unavailable in this libusb-OFF build.\n");
#endif
}

int list_models()
{
    for (std::size_t i = 0; i < asicen::profile_count(); ++i) {
        const auto& p = asicen::profiles()[i];
        // These are source capabilities, independent of this build's USB
        // support. The legacy S3 setters provide no verified power control;
        // mock ON/OFF simulation must not advertise physical S3 support.
        const bool lnb_control = p.model_id == asicen::ModelId::W3u2 ||
                                 p.model_id == asicen::ModelId::W3u3 ||
                                 p.model_id == asicen::ModelId::W3u3V2;
        std::printf("%s vid_pid=%04x:%04x model=\"%s\" capacity=%u functions=%u family=%s runtime=%s "
                    "lnb_control=%s lnb_15v_request=%s lnb_validation=pending\n",
            p.model_key, p.vid, p.pid, p.model,
            static_cast<unsigned>(p.enclosure_receiver_count),
            static_cast<unsigned>(p.expected_runtime_functions),
            asicen::frontend_family_name(p.frontend_family),
            asicen::profile_runtime_supported(p) ? "source-backed-experimental" : "not-enabled",
            lnb_control ? "source-backed-software" : "external-unconfirmed",
            lnb_control ? "supported" : "unsupported");
    }
    if (std::ferror(stdout) != 0 || std::fflush(stdout) != 0) {
        std::fprintf(stderr, "list: write to stdout failed\n");
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
        std::uint8_t ports[8]{};
        const int port_count = libusb_get_port_numbers(device, ports, sizeof(ports));
        if (port_count > 0 && port_count <= static_cast<int>(sizeof(ports)))
            observation.port_path.assign(ports, ports + port_count);
        observations.push_back(std::move(observation));
    }
    libusb_free_device_list(list, 1);
    asicen::release_libusb_context(context);
    const std::string output = asicen::cli::format_device_list(observations, json);
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
        return 2;
    }
    if (arguments.help) {
        usage(stdout);
        return 0;
    }
    if (arguments.models) return list_models();
    if (arguments.list || arguments.list_json) {
#ifdef ASICEN_ENABLE_LIBUSB
        return list_devices(arguments.list_json);
#else
        const std::string output = asicen::cli::format_device_list({}, arguments.list_json);
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
