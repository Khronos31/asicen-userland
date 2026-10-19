// SPDX-License-Identifier: GPL-2.0-only
#include "asicend_args.h"
#include "asicend_list_format.h"
#include "asicen/startup_readiness.h"

#include <cstdio>
#include <chrono>
#include <array>
#include <initializer_list>
#include <string>
#include <vector>

namespace {

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            std::fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__,         \
                         #condition);                                                       \
            return false;                                                                   \
        }                                                                                   \
    } while (false)

asicen::cli::DaemonArguments parse(std::initializer_list<const char*> options, bool mock = false)
{
    std::vector<const char*> arguments{"asicend"};
    arguments.insert(arguments.end(), options.begin(), options.end());
    return asicen::cli::parse_daemon_arguments(
        static_cast<int>(arguments.size()), arguments.data(), mock);
}

bool arguments_match_reference()
{
    for (const char* option : {"--help", "--list-json", "--models"}) {
        CHECK(parse({option}).valid);
        CHECK(!parse({option, option}).valid);
        CHECK(!parse({option, "--group"}).valid);
    }
    CHECK(!parse({"--list"}).valid);
    CHECK(!parse({"--socket", "/unused"}, true).valid &&
          !parse({"--socket", "/unused"}).valid);
    CHECK(!parse({"--device", "usb:1-2"}).valid);
    CHECK(parse({"--device", "usb:1-2", "--instance", "test"}).valid);
    CHECK(parse({"--fd", "3", "--instance", "test"}).valid);
    CHECK(parse({"--fd", "3"}).valid && parse({"--fd", "3"}).instance.empty());
    CHECK(parse({}, true).valid && parse({}, true).instance == "default");
    for (const char* selector : {"usb:0:1", "usb:1:0", "usb:256:1", "usb:1:256", "usb:0x1:2",
                                 "usb:1:2:3", "usb:0-1", "usb:1-0", "usb:1-256", "usb:1-1.",
                                 "usb:1-1..2", "usb:1-1.2.3.4.5.6.7.8.9"}) {
        CHECK(!parse({"--device", selector, "--instance", "test"}).valid);
    }
    std::uint8_t bus = 0U;
    std::uint8_t address = 0U;
    CHECK(asicen::cli::parse_bus_address("010:008", &bus, &address) &&
          bus == 10U && address == 8U);
    std::vector<std::uint8_t> ports;
    CHECK(asicen::cli::parse_bus_port("010-008.001", &bus, &ports) &&
          bus == 10U && ports == std::vector<std::uint8_t>({8U, 1U}));
    CHECK(!parse({"--device", "usb:1-2", "--fd", "3", "--instance", "test"}).valid);
    CHECK(!parse({"--device", "usb:1-2", "--instance", "test", "--runtime-dir", ""}).valid);
    CHECK(!parse({"--device", "usb:1-2", "--instance", "test", "--firmware", "--group"}).valid);
    CHECK(!parse({"--instance", "test", "--model", "s3u", "--model", "s3u"}, true).valid);
    CHECK(!parse({"--instance", "test", "--group", "--group"}, true).valid);
    CHECK(!parse({"--instance", "test", "--runtime-dir", "x", "--runtime-dir", "x"}, true).valid);
    CHECK(!parse({"--instance", "12345678901234"}, true).valid);
    return true;
}

bool list_formats_share_observed_identity()
{
    using asicen::UsbFunctionObservation;
    const std::vector<UsbFunctionObservation> pair{
        {0x0b06, 0x0005, 1, {2, 2}, 18},
        {0x0b06, 0x0005, 1, {2, 1}, 17}};
    const std::string json = asicen::cli::format_device_list(pair, true);
    CHECK(json.find("\"serial\":null") != std::string::npos &&
          json.find("\"serial_unique\":false") != std::string::npos);
    CHECK(json.find("\"identity_kind\":\"usb-topology\",\"identity\":\"usb-1-2\"") != std::string::npos);
    CHECK(json.find("\"device\":1,\"serial\":null,\"bus\":1,\"address\":17,\"port\":\"1-2.1\"") != std::string::npos);
    CHECK(json.find("\"receiver\":0,\"device\":1,\"local\":0,\"system\":\"ISDB-S\",\"lnb_15v_supported\":true") != std::string::npos);
    CHECK(json.find("\"receiver\":1,\"device\":1,\"local\":1,\"system\":\"ISDB-T\",\"lnb_15v_supported\":false") != std::string::npos);
    const std::string text = asicen::cli::format_device_list(pair, false);
    CHECK(text.find("status=ready receivers=4 serial_unique=false") != std::string::npos &&
          text.find("dev1_bus=1 dev1_address=17 dev1_port=1-2.1") != std::string::npos);
    const auto* model = asicen::find_profile(asicen::ModelId::W3u3);
    CHECK(model != nullptr && asicen::cli::runtime_matches_loader(pair[1], *model, 1U, {2U, 1U}));
    CHECK(!asicen::cli::runtime_matches_loader(pair[0], *model, 1U, {2U, 1U}) &&
          !asicen::cli::runtime_matches_loader(pair[1], *model, 2U, {2U, 1U}) &&
          !asicen::cli::runtime_matches_loader(pair[1], *model, 1U, {3U, 1U}) &&
          !asicen::cli::runtime_matches_loader(pair[1], *model, 1U, {}));
    auto other_model = pair[1];
    other_model.pid = 0x0006U;
    CHECK(!asicen::cli::runtime_matches_loader(other_model, *model, 1U, {2U, 1U}));
    CHECK(asicen::cli::format_device_list({pair[1]}, true).find("\"status\":\"incomplete\"") != std::string::npos);
    auto ambiguous = pair;
    ambiguous.push_back({0x0b06, 0x0005, 1, {2, 3}, 19});
    const std::string duplicate = asicen::cli::format_device_list(ambiguous, true);
    CHECK(duplicate.find("\"status\":\"duplicate\"") != std::string::npos &&
          duplicate.find("\"devices\":[],\"candidates\":[{") != std::string::npos);
    for (const std::uint16_t pid : {0x0001U, 0x0003U}) {
        const std::string single = asicen::cli::format_device_list({{0x0b06, pid, 2, {4}, 9}}, true);
        CHECK(single.find("\"status\":\"ready\"") != std::string::npos &&
              single.find("\"identity\":\"usb-2-4\"") != std::string::npos &&
              single.find("\"receivers\":[{") != std::string::npos &&
              single.find("\"ungrouped_usb_devices\":[]") != std::string::npos);
    }
    CHECK(asicen::cli::format_device_list({}, true) ==
          "{\"enclosures\":[],\"ungrouped_usb_devices\":[]}\n");
    CHECK(asicen::cli::format_device_list({}, false).empty());

    // The public --list-json now uses the shared schema. Only the physical
    // observation is faked here; it must never invent a loader model/serial.
    auto nested_pair = pair;
    for (auto& usb : nested_pair) usb.parent_genesys_hub = true;
    const std::string shared = asicen::cli::format_discovery_json(nested_pair, "0.1.0");
    CHECK(shared.find("\"name\":\"userland.discovery\"") != std::string::npos);
    CHECK(shared.find("\"model\":\"PX-W3U3\"") != std::string::npos);
    CHECK(shared.find("\"receiver\":3,\"local\":1,\"systems\":[\"ISDB-T\"]") != std::string::npos);
    CHECK(shared.find("\"readers\":[{") != std::string::npos);
    CHECK(shared.find("\"enclosures\":[{") != std::string::npos);
    CHECK(shared.find("\"ungrouped_usb_devices\":[]") != std::string::npos);
    // Cold: one or both loader functions create an enclosure even without a
    // resolved runtime model. No fictitious receivers or card readers.
    const std::string cold = asicen::cli::format_discovery_json(
        {{0x1738U,0x5211U,1U,{2U,1U},8U,true}}, "0.1.0");
    CHECK(cold.find("\"model\":null") != std::string::npos);
    CHECK(cold.find("\"in_loader_mode\":true") != std::string::npos);
    CHECK(cold.find("\"port\":\"1-2.1\"") != std::string::npos);
    CHECK(cold.find("\"enclosures\":[{") != std::string::npos);
    CHECK(cold.find("\"receivers\":") == std::string::npos);
    CHECK(cold.find("\"readers\":") == std::string::npos);
    const std::string both_cold = asicen::cli::format_discovery_json(
        {{0x1738U,0x5211U,1U,{2U,1U},8U,true},
         {0x1738U,0x5211U,1U,{2U,2U},9U,true}}, "0.1.0");
    CHECK(both_cold.find("\"device\":2,\"usb\":\"1738:5211\"") != std::string::npos);
    CHECK(both_cold.find("\"ungrouped_usb_devices\":[]") != std::string::npos);
    const std::string mixed = asicen::cli::format_discovery_json(
        {{0x1738U,0x5211U,1U,{2U,1U},8U,true},
         {0x0b06U,0x0005U,1U,{2U,2U},9U,true}}, "0.1.0");
    CHECK(mixed.find("\"usb\":null") != std::string::npos);
    CHECK(mixed.find("\"model\":null") != std::string::npos);
    CHECK(mixed.find("\"receivers\":") == std::string::npos);
    // Wrong hub / ports 3 and 4 must never be grouped.
    const std::string unsupported_hub = asicen::cli::format_discovery_json(
        {{0x1738U,0x5211U,1U,{2U,1U},8U,false},
         {0x1738U,0x5211U,1U,{2U,2U},9U,false}}, "0.1.0");
    CHECK(unsupported_hub.find("\"enclosures\":[]") != std::string::npos);
    const std::string ports_34 = asicen::cli::format_discovery_json(
        {{0x1738U,0x5211U,1U,{2U,3U},8U,true},
         {0x1738U,0x5211U,1U,{2U,4U},9U,true}}, "0.1.0");
    CHECK(ports_34.find("\"enclosures\":[]") != std::string::npos);
    return true;
}

using asicen::UploadedRuntimeIdentity;
using Error = px4::userland::Error;
using TestClock = std::chrono::steady_clock;

std::vector<UploadedRuntimeIdentity> uploaded_pair()
{
    return {{0U, 1U, {2U, 1U}, 0x0b06U, 0x0005U},
            {1U, 1U, {2U, 2U}, 0x0b06U, 0x0005U}};
}

bool readiness_rechecks_first_and_last_probe_boundaries()
{
    const auto identities = uploaded_pair();
    TestClock::time_point now{};
    bool cancelled = false;
    unsigned int calls = 0U;
    const Error first_cancelled = asicen::wait_for_uploaded_runtimes(
        identities,
        [&](const UploadedRuntimeIdentity&) {
            ++calls;
            cancelled = true;
            return Error::OK;
        },
        [&] { return now; }, [](std::chrono::milliseconds) {}, [&] { return cancelled; });
    CHECK(first_cancelled == Error::NOT_READY && calls == 1U);

    now = TestClock::time_point{};
    calls = 0U;
    const Error first_deadline = asicen::wait_for_uploaded_runtimes(
        identities,
        [&](const UploadedRuntimeIdentity&) {
            ++calls;
            now += std::chrono::seconds(10);
            return Error::OK;
        },
        [&] { return now; }, [](std::chrono::milliseconds) {}, [] { return false; });
    CHECK(first_deadline == Error::TIMEOUT && calls == 1U);

    now = TestClock::time_point{};
    cancelled = false;
    calls = 0U;
    const Error last_cancelled = asicen::wait_for_uploaded_runtimes(
        identities,
        [&](const UploadedRuntimeIdentity& identity) {
            ++calls;
            if (identity.selected_index == 1U) cancelled = true;
            return Error::OK;
        },
        [&] { return now; }, [](std::chrono::milliseconds) {}, [&] { return cancelled; });
    CHECK(last_cancelled == Error::NOT_READY && calls == 2U);

    now = TestClock::time_point{};
    calls = 0U;
    const Error last_deadline = asicen::wait_for_uploaded_runtimes(
        identities,
        [&](const UploadedRuntimeIdentity& identity) {
            ++calls;
            if (identity.selected_index == 1U) now += std::chrono::milliseconds(10001);
            return Error::OK;
        },
        [&] { return now; }, [](std::chrono::milliseconds) {}, [] { return false; });
    CHECK(last_deadline == Error::TIMEOUT && calls == 2U);
    return true;
}

bool readiness_handles_staggered_reenumeration_and_disappearance()
{
    auto identities = uploaded_pair();
    TestClock::time_point now{};
    unsigned int primary_probes = 0U;
    unsigned int sibling_probes = 0U;
    const Error staggered = asicen::wait_for_uploaded_runtimes(
        identities,
        [&](const UploadedRuntimeIdentity& identity) {
            if (identity.selected_index == 0U)
                return ++primary_probes < 2U ? Error::NOT_FOUND : Error::OK;
            return ++sibling_probes < 3U ? Error::NOT_FOUND : Error::OK;
        },
        [&] { return now; },
        [&](std::chrono::milliseconds duration) { now += duration; },
        [] { return false; });
    CHECK(staggered == Error::OK && primary_probes == 3U && sibling_probes == 3U);

    now = TestClock::time_point{};
    primary_probes = sibling_probes = 0U;
    const Error disappeared = asicen::wait_for_uploaded_runtimes(
        identities,
        [&](const UploadedRuntimeIdentity& identity) {
            if (identity.selected_index == 0U) {
                ++primary_probes;
                return primary_probes == 1U || primary_probes >= 4U ? Error::OK
                                                                      : Error::NOT_FOUND;
            }
            return ++sibling_probes < 3U ? Error::NOT_FOUND : Error::OK;
        },
        [&] { return now; },
        [&](std::chrono::milliseconds duration) { now += duration; },
        [] { return false; });
    CHECK(disappeared == Error::OK && primary_probes == 4U && sibling_probes == 4U);
    return true;
}

bool readiness_timeout_cancel_error_and_shared_deadline()
{
    auto identities = uploaded_pair();
    TestClock::time_point now{};
    unsigned int probes = 0U;
    CHECK(asicen::wait_for_uploaded_runtimes(
              {}, [&](const UploadedRuntimeIdentity&) { ++probes; return Error::OK; },
              [&] { return now; }, [](std::chrono::milliseconds) {}, [] { return false; }) ==
              Error::INVALID_ARGUMENT);
    CHECK(probes == 0U);

    bool initially_cancelled = true;
    const Error initial_cancel = asicen::wait_for_uploaded_runtimes(
        identities, [&](const UploadedRuntimeIdentity&) { ++probes; return Error::OK; },
        [&] { return now; }, [](std::chrono::milliseconds) {},
        [&] { return initially_cancelled; });
    CHECK(initial_cancel == Error::NOT_READY && probes == 0U);
    initially_cancelled = false;

    const Error timeout = asicen::wait_for_uploaded_runtimes(
        identities, [&](const UploadedRuntimeIdentity&) { ++probes; return Error::NOT_FOUND; },
        [&] { return now; },
        [&](std::chrono::milliseconds duration) { now += duration; }, [] { return false; });
    CHECK(timeout == Error::TIMEOUT && now.time_since_epoch() == std::chrono::seconds(10));

    now = TestClock::time_point{};
    unsigned int rounds = 0U;
    const Error cancelled = asicen::wait_for_uploaded_runtimes(
        identities, [&](const UploadedRuntimeIdentity&) { return Error::NOT_FOUND; },
        [&] { return now; },
        [&](std::chrono::milliseconds duration) { now += duration; },
        [&] { return ++rounds == 3U; });
    CHECK(cancelled == Error::NOT_READY && rounds == 3U);

    now = TestClock::time_point{};
    unsigned int calls = 0U;
    const Error busy = asicen::wait_for_uploaded_runtimes(
        identities,
        [&](const UploadedRuntimeIdentity&) {
            ++calls;
            now += std::chrono::seconds(6);
            return Error::NOT_FOUND;
        },
        [&] { return now; },
        [&](std::chrono::milliseconds duration) { now += duration; }, [] { return false; });
    CHECK(busy == Error::TIMEOUT && calls == 2U);

    now = TestClock::time_point{};
    calls = 0U;
    std::chrono::milliseconds slept{};
    const Error limited_sleep = asicen::wait_for_uploaded_runtimes(
        identities,
        [&](const UploadedRuntimeIdentity&) {
            if (calls++ == 0U) now += std::chrono::milliseconds(9900);
            return Error::NOT_FOUND;
        },
        [&] { return now; },
        [&](std::chrono::milliseconds duration) {
            slept += duration;
            now += duration;
        },
        [] { return false; });
    CHECK(limited_sleep == Error::TIMEOUT && slept == std::chrono::milliseconds(100));

    now = TestClock::time_point{};
    const Error duplicate = asicen::wait_for_uploaded_runtimes(
        identities,
        [&](const UploadedRuntimeIdentity&) { return Error::BUSY; },
        [&] { return now; },
        [&](std::chrono::milliseconds duration) { now += duration; }, [] { return false; });
    CHECK(duplicate == Error::BUSY);
    return true;
}

bool readiness_tracks_only_cold_functions_for_single_dual_and_mixed_profiles()
{
    // The injected probe tests exercise timing only. Production matching of
    // each model's VID/PID, bus, and full port path stays in the existing USB
    // resolver called by probe_uploaded_runtime().
    const auto* single = asicen::find_profile(asicen::ModelId::S3u);
    const auto* dual = asicen::find_profile(asicen::ModelId::W3u3);
    CHECK(single != nullptr && single->expected_runtime_functions == 1U && dual != nullptr &&
          dual->expected_runtime_functions == 2U);
    std::vector<UploadedRuntimeIdentity> uploaded{
        {0U, 4U, {3U}, single->vid, single->pid}};
    CHECK(asicen::wait_for_uploaded_runtimes(
              uploaded, [](const UploadedRuntimeIdentity&) { return Error::OK; },
              [] { return TestClock::time_point{}; }, [](std::chrono::milliseconds) {},
              [] { return false; }) == Error::OK);

    // The warm primary is deliberately absent from the upload set.
    uploaded = {{1U, 4U, {3U, 2U}, dual->vid, dual->pid}};
    CHECK(asicen::wait_for_uploaded_runtimes(
              uploaded, [](const UploadedRuntimeIdentity& identity) {
                  return identity.selected_index == 1U ? Error::OK : Error::INTERNAL;
              },
              [] { return TestClock::time_point{}; }, [](std::chrono::milliseconds) {},
              [] { return false; }) == Error::OK);

    // The warm sibling is omitted when only the primary was uploaded.
    uploaded = {{0U, 4U, {3U, 2U, 1U}, dual->vid, dual->pid}};
    CHECK(asicen::wait_for_uploaded_runtimes(
              uploaded, [](const UploadedRuntimeIdentity& identity) {
                  return identity.selected_index == 0U ? Error::OK : Error::INTERNAL;
              },
              [] { return TestClock::time_point{}; }, [](std::chrono::milliseconds) {},
              [] { return false; }) == Error::OK);

    uploaded = {{0U, 4U, {3U, 2U, 1U}, dual->vid, dual->pid},
                {1U, 4U, {3U, 2U, 2U}, dual->vid, dual->pid}};
    CHECK(asicen::wait_for_uploaded_runtimes(
              uploaded, [](const UploadedRuntimeIdentity&) { return Error::OK; },
              [] { return TestClock::time_point{}; }, [](std::chrono::milliseconds) {},
              [] { return false; }) == Error::OK);

    constexpr std::array<asicen::ModelId, 5> model_ids{{
        asicen::ModelId::S3u, asicen::ModelId::S3u2, asicen::ModelId::W3u2,
        asicen::ModelId::W3u3, asicen::ModelId::W3u3V2}};
    std::vector<UploadedRuntimeIdentity> model_identities;
    for (std::size_t index = 0U; index < model_ids.size(); ++index) {
        const auto* profile = asicen::find_profile(model_ids[index]);
        CHECK(profile != nullptr);
        model_identities.push_back({index, static_cast<std::uint8_t>(index + 1U),
                                    {static_cast<std::uint8_t>(index + 1U), 2U,
                                     static_cast<std::uint8_t>(index + 1U)},
                                    profile->vid, profile->pid});
    }
    std::size_t model_probes = 0U;
    CHECK(asicen::wait_for_uploaded_runtimes(
              model_identities,
              [&](const UploadedRuntimeIdentity& identity) {
                  const auto* profile = asicen::find_profile(model_ids[identity.selected_index]);
                  ++model_probes;
                  return profile != nullptr && identity.bus == identity.selected_index + 1U &&
                                 identity.port_path == std::vector<std::uint8_t>{
                                                           static_cast<std::uint8_t>(
                                                               identity.selected_index + 1U),
                                                           2U,
                                                           static_cast<std::uint8_t>(
                                                               identity.selected_index + 1U)} &&
                                 identity.vendor_id == profile->vid &&
                                 identity.product_id == profile->pid
                             ? Error::OK
                             : Error::INVALID_ARGUMENT;
              },
              [] { return TestClock::time_point{}; }, [](std::chrono::milliseconds) {},
              [] { return false; }) == Error::OK);
    CHECK(model_probes == model_ids.size());
    return true;
}

}  // namespace

int main()
{
    if (!arguments_match_reference() || !list_formats_share_observed_identity() ||
        !readiness_rechecks_first_and_last_probe_boundaries() ||
        !readiness_handles_staggered_reenumeration_and_disappearance() ||
        !readiness_timeout_cancel_error_and_shared_deadline() ||
        !readiness_tracks_only_cold_functions_for_single_dual_and_mixed_profiles()) {
        return 1;
    }
    std::puts("ASICEN tool parity tests passed (no USB access)");
    return 0;
}
