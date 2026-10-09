// SPDX-License-Identifier: GPL-2.0-or-later
// asicend-mock: isolated mock tuner/card service for offline testing and
// research.  It has no hardware backend and is not part of the distributed
// product; the real-hardware daemon is asicend.
#include "asicen/product_profile.h"
#include "asicen/device_profile.h"
#include "asicen/px4_mock_backend.h"
#include "asicen/enclosure_lock.h"
#include "px4/control_server.h"
#include "px4/posix_tuner_nonce.h"

#include <signal.h>
#include <unistd.h>

#include <chrono>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

int run_asicend_research(int argc, char** argv);

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
    std::fprintf(output,
        "usage: asicend-mock [--model MODEL] [--runtime-dir PATH] [--instance TOKEN]\n"
        "                    [--group] [--allow-lnb-power]\n"
        "       asicend-mock --socket PATH  (legacy research protocol)\n"
        "  --allow-lnb-power  permit explicit ISDB-S 15 V requests; default off\n");
}

int list_models()
{
    for (std::size_t i = 0; i < asicen::profile_count(); ++i) {
        const auto& p = asicen::profiles()[i];
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

}  // namespace

int main(int argc, char** argv)
{
    bool has_research_socket = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--socket") == 0) has_research_socket = true;
    }
    if (has_research_socket) return run_asicend_research(argc, argv);

    bool models = false;
    bool allow_lnb_power = false;
    bool group = false;
    const asicen::DeviceProfile* selected_model = asicen::find_profile(asicen::ModelId::W3u3);
    std::string runtime_directory;
    std::string instance = "default";
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--help" || arg == "-h") { print_usage(stdout); return 0; }
        if (arg == "--models") { models = true; continue; }
        if (arg == "--allow-lnb-power") { allow_lnb_power = true; continue; }
        if (arg == "--group") { group = true; continue; }
        if (arg == "--model" && i + 1 < argc) {
            selected_model = asicen::find_profile_by_model(argv[++i]);
            if (selected_model == nullptr) { std::fprintf(stderr, "unknown ASICEN model\n"); return 2; }
            continue;
        }
        if ((arg == "--runtime-dir" || arg == "--instance") && i + 1 < argc) {
            const std::string value(argv[++i]);
            if (arg == "--runtime-dir") runtime_directory = value;
            else instance = value;
            continue;
        }
        print_usage(stderr);
        return 2;
    }
    if (models) return list_models();
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
        std::fprintf(stderr, "asicend-mock: %s\n", px4::userland::error_string(server.error()));
        ::close(enclosure_lock);
        return server.error() == px4::userland::Error::BUSY ? 4 : 70;
    }

    struct sigaction action{};
    action.sa_handler = signal_handler;
    sigemptyset(&action.sa_mask);
    ::sigaction(SIGINT, &action, nullptr);
    ::sigaction(SIGTERM, &action, nullptr);
    ::signal(SIGPIPE, SIG_IGN);
    std::fprintf(stderr, "asicend-mock ready backend=mock-only model=%s serial=none receivers=%u endpoint=%s\n",
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
        std::fprintf(stderr, "asicend-mock shutdown: %s\n", px4::userland::error_string(loop_error));
        return 70;
    }
    return 0;
}