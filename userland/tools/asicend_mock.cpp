// SPDX-License-Identifier: GPL-2.0-or-later
// asicend-mock: isolated mock tuner/card service for offline testing and
// research.  It has no hardware backend and is not part of the distributed
// product; the real-hardware daemon is asicend.
#include "asicen/product_profile.h"
#include "asicen/device_profile.h"
#include "asicen/px4_mock_backend.h"
#include "asicend_args.h"
#include "px4d_signals.h"
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

namespace {

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

void print_usage(FILE* output)
{
    std::fprintf(output,
        "usage: asicend-mock [--model MODEL] [--runtime-dir PATH] [--instance TOKEN]\n"
        "                    [--group] [--allow-lnb-power]\n"
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
    if (std::ferror(stdout) != 0 || std::fflush(stdout) != 0) {
        std::fprintf(stderr, "list: write to stdout failed\n");
        return 70;
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    const auto arguments = asicen::cli::parse_daemon_arguments(
        argc, const_cast<const char* const*>(argv), true);
    if (!arguments.valid) {
        std::fprintf(stderr, "argument error: %.*s\n",
                     static_cast<int>(arguments.error.size()), arguments.error.data());
        print_usage(stderr);
        return 2;
    }
    if (arguments.help) { print_usage(stdout); return 0; }
    if (arguments.models) return list_models();
    const asicen::DeviceProfile* selected_model = arguments.expected_model != nullptr
        ? arguments.expected_model : asicen::find_profile(asicen::ModelId::W3u3);
    const std::string& runtime_directory = arguments.runtime_directory;
    const std::string& instance = arguments.instance;
    const bool group = arguments.group;
    const bool allow_lnb_power = arguments.allow_lnb_power;

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
        return asicen::cli::exit_status(server.error());
    }

    if (!px4::userland::px4d::install_signal_handlers()) {
        std::fprintf(stderr, "signal setup failed\n");
        return 70;
    }
    std::fprintf(stderr, "asicend-mock ready backend=mock-only model=%s serial=none receivers=%u endpoint=%s\n",
                 selected_model->model_key, static_cast<unsigned>(selected_model->enclosure_receiver_count),
                 server.value()->endpoint_path());
    px4::userland::Error loop_error = px4::userland::Error::OK;
    while (!px4::userland::px4d::stop_requested()) {
        const auto polled = server.value()->poll_once(px4::userland::Timeout{100U});
        if (!polled) { loop_error = polled.error(); break; }
    }
    const auto stopped = server.value()->shutdown();
    if (!stopped && loop_error == px4::userland::Error::OK) loop_error = stopped.error();
    server.value().reset();
    px4::userland::px4d::notify_cleanup_complete();
    if (loop_error != px4::userland::Error::OK) {
        std::fprintf(stderr, "asicend-mock shutdown: %s\n", px4::userland::error_string(loop_error));
        return asicen::cli::exit_status(loop_error);
    }
    return 0;
}
