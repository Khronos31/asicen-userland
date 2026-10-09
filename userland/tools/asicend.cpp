// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/product_profile.h"
#include "asicen/device_profile.h"
#include "asicen/px4_mock_backend.h"
#include "asicen/enclosure_lock.h"
#include "asicen/enclosure_grouping.h"
#include "px4/control_server.h"
#include "px4/posix_tuner_nonce.h"
#ifdef ASICEN_ENABLE_LIBUSB
#include <libusb.h>
#endif

#include <signal.h>
#include <unistd.h>

#include <chrono>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

int run_asicend_research(int argc, char** argv);
#ifdef ASICEN_ENABLE_LIBUSB
int run_asicend_hardware(int argc, char** argv);
#endif

namespace {

volatile sig_atomic_t stop_requested = 0;

void signal_handler(int) { stop_requested = 1; }

class MockTime final : public px4::userland::TunerServiceTime {
public:
    std::uint64_t monotonic_ms() noexcept override
    {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    void sleep_ms(std::uint32_t ms) noexcept override
    { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
};

bool valid_instance(const std::string& value)
{
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

int acquire_enclosure_lock()
{
    return asicen::acquire_enclosure_lock("/tmp/asicen-userland-enclosure.lock");
}

void print_usage(FILE* output)
{
#ifdef ASICEN_ENABLE_LIBUSB
    std::fprintf(output,
        "usage: asicend --mock [--model MODEL] [--runtime-dir PATH] [--instance TOKEN]\n"
        "                 [--group] [--allow-lnb-power]\n"
        "       asicend --hardware --primary BUS:ADDR --primary-port BUS-PORT\n"
        "               [--sibling BUS:ADDR --sibling-port BUS-PORT]\n"
        "               [--model MODEL] [--runtime-dir PATH] [--instance TOKEN]\n"
        "               [--group] [--allow-lnb-power]\n"
        "       asicend --hardware --primary-fd FD [--sibling-fd FD]\n"
        "               [--model MODEL] [--runtime-dir PATH] [--instance TOKEN]\n"
        "               [--group] [--allow-lnb-power]\n"
        "       asicend --list | --list-json | --models\n"
        "Hardware mode uses source-guarded model dispatch; see --models and model-support.md.\n");
#else
    std::fprintf(output,
        "usage: asicend --mock [--model MODEL] [--runtime-dir PATH] [--instance TOKEN]\n"
        "                 [--group] [--allow-lnb-power]\n"
        "       asicend --list | --list-json | --models\n"
        "Hardware mode is unavailable in this libusb-OFF build.\n");
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
    return 0;
}

#ifdef ASICEN_ENABLE_LIBUSB
// Formats one observed USB function as a compact JSON object using the px4
// device-observation schema (serial is absent on ASICEN, so it stays null).
void append_usb_observation(std::string& out,
                            const asicen::UsbFunctionObservation& obs) {
    out += "{\"serial\":null,\"bus\":";
    out += std::to_string(obs.bus);
    out += ",\"address\":null,\"port\":\"";
    for (std::size_t k = 0U; k < obs.port_path.size(); ++k) {
        if (k != 0U) out.push_back('.');
        out += std::to_string(obs.port_path[k]);
    }
    out += "\"}";
}

// Emits the px4 receiver-records list for a profile. W3U2/W3U3/V2 expose one
// ISDB-S and one ISDB-T lane per runtime function; S3U/S3U2 use a combined
// terrestrial-capable frontend. LNB control is source-backed on the W3U
// family and otherwise unsupported.
void append_receivers(std::string& out, const asicen::DeviceProfile& profile) {
    bool first = true;
    const bool lnb_control = profile.model_id == asicen::ModelId::W3u2 ||
                             profile.model_id == asicen::ModelId::W3u3 ||
                             profile.model_id == asicen::ModelId::W3u3V2;
    for (std::uint8_t receiver = 0U;
         receiver < profile.enclosure_receiver_count; ++receiver) {
        if (!first) out.push_back(',');
        first = false;
        const std::uint8_t function = receiver / 2U;
        const std::uint8_t lane = receiver % 2U;
        const char* system = profile.combined_isdb_ts
            ? "isdb-t"
            : (lane == 0U ? "isdb-s" : "isdb-t");
        out += "{\"receiver\":";
        out += std::to_string(receiver);
        out += ",\"device\":";
        out += std::to_string(function);
        out += ",\"local\":";
        out += std::to_string(lane);
        out += ",\"system\":\"";
        out += system;
        out += "\",\"lnb_15v_supported\":";
        out += lnb_control ? "true" : "false";
        out += "}";
    }
}
#endif

int list_devices(bool json)
{
#ifdef ASICEN_ENABLE_LIBUSB
    if (!json) {
        std::puts("Device enumeration is not implemented; hardware selection requires explicit paths.");
        return 0;
    }
    libusb_context* context = nullptr;
    if (libusb_init(&context) != 0) {
        std::fprintf(stderr, "libusb_init failed\n");
        return 70;
    }
    libusb_device** list = nullptr;
    const ssize_t count = libusb_get_device_list(context, &list);
    if (count < 0) {
        std::fprintf(stderr, "libusb_get_device_list failed\n");
        libusb_exit(context);
        return 70;
    }

    // Collect all observations for supported runtime models and loaders.
    // Grouping is topology-based: two runtime functions with the same hub
    // parent form one enclosure; loaders and lone functions are reported
    // ungrouped under the px4 --list-json schema.
    std::vector<std::vector<asicen::UsbFunctionObservation>> dual(asicen::profile_count());
    std::vector<asicen::UsbFunctionObservation> single;
    for (ssize_t i = 0; i < count; ++i) {
        libusb_device_descriptor desc{};
        libusb_device* device = list[i];
        if (libusb_get_device_descriptor(device, &desc) != 0) continue;
        asicen::UsbFunctionObservation obs;
        obs.vid = desc.idVendor;
        obs.pid = desc.idProduct;
        obs.bus = static_cast<std::uint8_t>(libusb_get_bus_number(device));
        std::uint8_t ports[8]{};
        const int nports = libusb_get_port_numbers(device, ports,
                                                   static_cast<int>(sizeof(ports)));
        if (nports < 0 || nports > 255) continue;
        obs.port_path.assign(ports, ports + nports);
        const asicen::DeviceProfile* profile =
            asicen::find_profile(desc.idVendor, desc.idProduct);
        if (profile == nullptr &&
            !(desc.idVendor == 0x1738 &&
              (desc.idProduct == 0x5211U || desc.idProduct == 0x5216U)))
            continue;
        if (profile == nullptr) {
            single.push_back(obs);
        } else {
            const std::size_t index =
                static_cast<std::size_t>(profile - asicen::profiles());
            if (profile->expected_runtime_functions >= 2U)
                dual[index].push_back(obs);
            else
                single.push_back(obs);
        }
    }
    libusb_free_device_list(list, 1);

    std::string out = "{\"enclosures\":[";
    bool first_group = true;
    // Pair dual-function observations by shared hub parent.
    for (std::size_t pi = 0U; pi < asicen::profile_count(); ++pi) {
        const auto& profile = asicen::profiles()[pi];
        if (dual[pi].empty()) continue;
        const auto pairs = asicen::group_dual_function_enclosures(
            dual[pi], profile.vid, profile.pid);
        std::vector<bool> used(dual[pi].size(), false);
        for (const auto& pair : pairs) {
            const auto& primary = dual[pi][pair.first];
            const auto& sibling = dual[pi][pair.second];
            used[pair.first] = used[pair.second] = true;
            if (!first_group) out.push_back(',');
            first_group = false;
            std::string usb;
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%04x:%04x",
                          static_cast<unsigned>(primary.vid),
                          static_cast<unsigned>(primary.pid));
            out += "{\"serial\":null,\"model\":";
            out += '"';
            out += profile.model;
            out += '"';
            out += ",\"usb\":\"";
            out += buf;
            out += "\",\"status\":\"ready\",\"serial_unique\":true,\"devices\":[";
            append_usb_observation(out, primary);
            out.push_back(',');
            append_usb_observation(out, sibling);
            out += "],\"candidates\":[],\"receivers\":[";
            append_receivers(out, profile);
            out += "]}";
        }
        // Lone (unpaired) dual-capable functions are ungrouped.
        for (std::size_t i = 0U; i < dual[pi].size(); ++i) {
            if (used[i]) continue;
            single.push_back(dual[pi][i]);
        }
    }
    out += "],\"ungrouped_usb_devices\":[";
    bool first_rejected = true;
    for (const auto& obs : single) {
        std::string usb;
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%04x:%04x",
                      static_cast<unsigned>(obs.vid), static_cast<unsigned>(obs.pid));
        const asicen::DeviceProfile* p = asicen::find_profile(obs.vid, obs.pid);
        const char* model = p != nullptr ? p->model : "ASICEN firmware loader";
        if (!first_rejected) out.push_back(',');
        first_rejected = false;
        out += "{\"serial\":null,\"model\":\"";
        out += model;
        out += "\",\"usb\":\"";
        out += buf;
        out += "\",\"status\":\"";
        out += p != nullptr ? "ready" : "loader";
        out += "\",";
        append_usb_observation(out, obs);
        out += "}";
    }
    out += "]}\n";
    std::fputs(out.c_str(), stdout);
    libusb_exit(context);
    return 0;
#else
    if (json) std::puts("{\"enclosures\":[],\"ungrouped_usb_devices\":[]}");
    else std::puts("No ASICEN hardware backend is enabled (mock-only build).");
    return 0;
#endif
}

}  // namespace

int main(int argc, char** argv)
{
    bool has_mock = false;
    bool has_research_socket = false;
    bool has_hardware = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--mock") == 0) has_mock = true;
        if (std::strcmp(argv[i], "--socket") == 0) has_research_socket = true;
        if (std::strcmp(argv[i], "--hardware") == 0) has_hardware = true;
    }
    if (has_hardware) {
#ifdef ASICEN_ENABLE_LIBUSB
        return run_asicend_hardware(argc, argv);
#else
        std::fprintf(stderr, "hardware backend unavailable in this libusb-OFF build\n");
        return 3;
#endif
    }
    if (has_mock && has_research_socket) return run_asicend_research(argc, argv);
    bool mock = false;
    bool list = false;
    bool list_json = false;
    bool models = false;
    bool allow_lnb_power = false;
    bool group = false;
    const asicen::DeviceProfile* selected_model = asicen::find_profile(asicen::ModelId::W3u3);
    std::string runtime_directory;
    std::string instance = "default";
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--help" || arg == "-h") { print_usage(stdout); return 0; }
        if (arg == "--mock") { mock = true; continue; }
        if (arg == "--models") { models = true; continue; }
        if (arg == "--allow-lnb-power") { allow_lnb_power = true; continue; }
        if (arg == "--group") { group = true; continue; }
        if (arg == "--model" && i + 1 < argc) {
            selected_model = asicen::find_profile_by_model(argv[++i]);
            if (selected_model == nullptr) { std::fprintf(stderr, "unknown ASICEN model\n"); return 2; }
            continue;
        }
        if (arg == "--list") { list = true; continue; }
        if (arg == "--list-json") { list_json = true; continue; }
        if ((arg == "--runtime-dir" || arg == "--instance") && i + 1 < argc) {
            const std::string value(argv[++i]);
            if (arg == "--runtime-dir") runtime_directory = value;
            else instance = value;
            continue;
        }
        if (arg == "--usb-path" && i + 1 < argc) {
            std::fprintf(stderr, "USB topology selection requires the disabled hardware backend\n");
            return 3;
        }
        print_usage(stderr);
        return 2;
    }
    if (models) return list_models();
    if (list || list_json) return list_devices(list_json);
    if (!mock) {
        std::fprintf(stderr, "hardware backend disabled; pass --mock for the isolated mock service\n");
        return 3;
    }
    if (!valid_instance(instance) || runtime_directory.size() >= 400U) {
        std::fprintf(stderr, "invalid runtime directory or instance token\n");
        return 2;
    }
    const int enclosure_lock = acquire_enclosure_lock();
    if (enclosure_lock < 0) {
        std::fprintf(stderr, "ASICEN enclosure is already owned or lock path is unsafe\n");
        return 4;
    }

    asicen::MockTunerBackend tuner_backend(*selected_model);
    tuner_backend.set_allow_lnb_power(allow_lnb_power);
    asicen::MockTunerStream stream;
    asicen::UnsupportedCardBackend card_backend;
    asicen::UnsupportedCardSession card_session;
    px4::userland::CardService card_service(card_backend, card_session);
    px4::userland::ipc::posix::PosixTunerNonceSource nonce_source;
    MockTime time;
    px4::userland::TunerService tuner_service(
        tuner_backend, nonce_source, time, nullptr, nullptr, &stream);
    const px4::userland::ipc::posix::EndpointConfig endpoint{
        runtime_directory.empty() ? nullptr : runtime_directory.c_str(),
        instance.c_str(), px4::userland::ipc::posix::kControlEndpointName,
        group ? px4::userland::ipc::posix::EndpointAccess::shared_group
              : px4::userland::ipc::posix::EndpointAccess::private_user};
    auto server = px4::userland::ipc::posix::PosixControlServer::create(
        endpoint, card_service, tuner_service, {}, true,
        asicen::profile::usb_present_mask(selected_model->enclosure_receiver_count), &stream,
        selected_model->enclosure_receiver_count, selected_model->combined_isdb_ts);
    if (!server) {
        std::fprintf(stderr, "asicend: %s\n", px4::userland::error_string(server.error()));
        ::close(enclosure_lock);
        return server.error() == px4::userland::Error::BUSY ? 4 : 70;
    }

    struct sigaction action{};
    action.sa_handler = signal_handler;
    sigemptyset(&action.sa_mask);
    ::sigaction(SIGINT, &action, nullptr);
    ::sigaction(SIGTERM, &action, nullptr);
    ::signal(SIGPIPE, SIG_IGN);
    std::fprintf(stderr, "asicend ready backend=mock-only model=%s serial=none receivers=%u endpoint=%s\n",
                 selected_model->model_key, static_cast<unsigned>(selected_model->enclosure_receiver_count),
                 server.value()->endpoint_path());
    px4::userland::Error loop_error = px4::userland::Error::OK;
    while (!stop_requested) {
        const auto polled = server.value()->poll_once(px4::userland::Timeout{100U});
        if (!polled) { loop_error = polled.error(); break; }
    }
    const auto stopped = server.value()->shutdown();
    if (!stopped && loop_error == px4::userland::Error::OK) loop_error = stopped.error();
    server.value().reset();
    ::close(enclosure_lock);
    if (loop_error != px4::userland::Error::OK) {
        std::fprintf(stderr, "asicend shutdown: %s\n", px4::userland::error_string(loop_error));
        return 70;
    }
    return 0;
}
