// SPDX-License-Identifier: GPL-2.0-only
#include "asicend_args.h"
#include "asicend_list_format.h"
#include "asicend_selector.h"
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
    for (const char* option : {"--help", "-h", "--version", "--list-json", "--models"}) {
        CHECK(parse({option}).valid);
        CHECK(!parse({option, option}).valid);
        CHECK(!parse({option, "--group"}).valid);
    }
    CHECK(!parse({"--list"}).valid &&
          parse({"--list"}).error == "unknown argument");
    CHECK(!parse({"--list", "--group"}).valid);
    CHECK(!parse({"--list"}, true).valid);
    CHECK(parse({"-h"}).help && parse({"--help"}).help);
    CHECK(parse({"--version"}).version);
    CHECK(!parse({"--version"}, true).valid);
    CHECK(!parse({"-l"}).valid);
    CHECK(!parse({"--socket", "/unused"}, true).valid &&
          !parse({"--socket", "/unused"}).valid);
    CHECK(!parse({"--usb-path", "1:2", "--instance", "test"}).valid);
    CHECK(parse({"--device", "usb:1-2", "--instance", "test"}).valid);
    CHECK(parse({"--device", "usb:1-2", "--instance", "test",
                 "--firmware", "loader.bin"}).valid);
    CHECK(parse({"--device", "usb:1-2.1,1-2.2", "--instance", "test"}).valid);
    CHECK(parse({"--device", "serial:abc", "--instance", "test"}).valid);
    CHECK(!parse({"--device", "usb:01-2", "--instance", "test"}).valid);
    CHECK(!parse({"--device", "usb:1-2,1-2", "--instance", "test"}).valid);
    CHECK(!parse({"--device", "usb:1-2.1,1-2.2,1-2.3", "--instance", "test"}).valid);
    CHECK(!parse({"--device", "usb:1-2", "--device", "usb:1-3",
                  "--instance", "test"}).valid);
    CHECK(!parse({"--device", "usb:1-2", "--fd", "3", "--instance", "test"}).valid);
    CHECK(parse({"--fd", "3", "--instance", "test"}).valid);
    CHECK(parse({"--fd", "3"}).valid && parse({"--fd", "3"}).instance.empty());
    CHECK(parse({}, true).valid && parse({}, true).instance == "default");
    for (const char* selector : {"usb:0-1", "usb:1-0", "usb:256-1",
                                 "usb:1-256", "usb:1-1.", "usb:1-1..2",
                                 "usb:1-1.2.3.4.5.6.7.8.9"})
        CHECK(!parse({"--device", selector, "--instance", "test"}).valid);
    std::uint8_t bus = 0U;
    std::uint8_t address = 0U;
    CHECK(asicen::cli::parse_bus_address("010:008", &bus, &address) &&
          bus == 10U && address == 8U);
    std::vector<std::uint8_t> ports;
    CHECK(asicen::cli::parse_bus_port("010-008.001", &bus, &ports) &&
          bus == 10U && ports == std::vector<std::uint8_t>({8U, 1U}));
    CHECK(!parse({"--device", "usb:1-2", "--instance", "test", "--runtime-dir", ""}).valid);
    CHECK(!parse({"--device", "usb:1-2", "--instance", "test", "--firmware", "--group"}).valid);
    CHECK(!parse({"--instance", "test", "--model", "s3u", "--model", "s3u"}, true).valid);
    CHECK(!parse({"--instance", "test", "--group", "--group"}, true).valid);
    CHECK(!parse({"--instance", "test", "--runtime-dir", "x", "--runtime-dir", "x"}, true).valid);
    CHECK(!parse({"--instance", "12345678901234"}, true).valid);
    return true;
}

bool selector_grouping()
{
    using asicen::UsbFunctionObservation;
    const auto* w3u3 = asicen::find_profile_by_model("w3u3");
    CHECK(w3u3 != nullptr);
    const auto parsed = dtv::userland::identity::parse_device_selector(
        "usb:1-2.2.1,1-2.2.2");
    CHECK(parsed.has_value());
    UsbFunctionObservation primary{0x1738U, 0x5211U, 1U, {2U, 2U, 1U}, 9U};
    primary.parent_hub_vid = 0x05e3U;
    primary.parent_hub_pid = 0x0608U;
    primary.parent_hub_known = true;
    UsbFunctionObservation sibling = primary;
    sibling.port_path.back() = 2U;
    sibling.address = 10U;
    auto selected = asicen::cli::select_enclosure(parsed.value(), {primary, sibling},
                                                   nullptr, true);
    CHECK(selected && selected.value().model == nullptr &&
          selected.value().functions.size() == 2U &&
          selected.value().functions[0].port_path.back() == 1U &&
          selected.value().functions[1].port_path.back() == 2U);
    const auto one = dtv::userland::identity::parse_device_selector("usb:1-2.2.1");
    CHECK(one.has_value());
    auto duplicate = primary;
    duplicate.address = 12U;
    selected = asicen::cli::select_enclosure(one.value(), {primary, duplicate}, nullptr, true);
    CHECK(!selected && selected.error() == px4::userland::Error::NOT_READY);
    selected = asicen::cli::select_enclosure(one.value(), {primary}, nullptr, true);
    CHECK(selected && selected.value().functions.size() == 2U);
    auto ungrouped = primary;
    ungrouped.parent_hub_pid = 0x0610U;
    selected = asicen::cli::select_enclosure(one.value(), {ungrouped}, nullptr, true);
    CHECK(selected && selected.value().functions.size() == 1U);
    const auto runtime = dtv::userland::identity::parse_device_selector("usb:1-2.2.1");
    CHECK(runtime.has_value());
    UsbFunctionObservation runtime_primary{w3u3->vid, w3u3->pid, 1U, {2U, 2U, 1U}, 11U};
    runtime_primary.parent_hub_vid = 0x05e3U;
    runtime_primary.parent_hub_pid = 0x0608U;
    runtime_primary.parent_hub_known = true;
    UsbFunctionObservation runtime_sibling = runtime_primary;
    runtime_sibling.port_path.back() = 2U;
    selected = asicen::cli::select_enclosure(runtime.value(),
        {runtime_primary, runtime_sibling}, nullptr, false);
    CHECK(selected && selected.value().model == w3u3 &&
          selected.value().functions.size() == 2U);
    const auto serial = dtv::userland::identity::parse_device_selector("serial:abc");
    CHECK(serial.has_value() && !asicen::cli::select_enclosure(
        serial.value(), {runtime_primary}, nullptr, false));
    return true;
}

bool list_formats_share_observed_identity()
{
    using asicen::UsbFunctionObservation;
    UsbFunctionObservation first{0x0b06, 0x0005, 1, {2, 3, 1}, 17};
    first.parent_hub_vid = 0x05e3;
    first.parent_hub_pid = 0x0608;
    first.parent_hub_known = true;
    UsbFunctionObservation second{0x0b06, 0x0005, 1, {2, 3, 2}, 18};
    second.parent_hub_vid = 0x05e3;
    second.parent_hub_pid = 0x0608;
    second.parent_hub_known = true;
    const std::vector<UsbFunctionObservation> pair{
        first, second};
    const std::string json = asicen::cli::format_device_list(pair, true);
    CHECK(json.find("\"schema\":{\"name\":\"userland.discovery\",\"major\":1,\"minor\":0}") != std::string::npos);
    CHECK(json.find("\"serial_unique\":null,\"status\":\"ready\"") != std::string::npos);
    CHECK(json.find("\"model\":\"PX-W3U3\"") != std::string::npos);
    CHECK(json.find("\"id\":1,\"usb\":\"0b06:0005\",\"serial\":null,\"bus\":1,\"address\":17,\"port\":\"1-2.3.1\",\"receivers\":[{\"receiver\":0,\"device\":1,\"local\":0,\"systems\":[\"ISDB-S\"]}") != std::string::npos);
    CHECK(json.find("\"id\":2,\"usb\":\"0b06:0005\",\"serial\":null,\"bus\":1,\"address\":18,\"port\":\"1-2.3.2\",\"receivers\":[{\"receiver\":2,\"device\":2,\"local\":0,\"systems\":[\"ISDB-S\"]}") != std::string::npos);
    CHECK(json.find("\"lnb_15v_supported\":true,\"readers\":[{\"reader\":0,\"device\":1}]") != std::string::npos);
    const std::string text = asicen::cli::format_device_list(pair, false);
    CHECK(text.find("status=ready receivers=4 serial_unique=false") != std::string::npos &&
          text.find("dev1_bus=1 dev1_address=17 dev1_port=1-2.3.1") != std::string::npos);
    const auto* model = asicen::find_profile(asicen::ModelId::W3u3);
    CHECK(model != nullptr && asicen::cli::runtime_matches_loader(pair[0], *model, 1U, {2U, 3U, 1U}));
    CHECK(!asicen::cli::runtime_matches_loader(pair[1], *model, 1U, {2U, 3U, 1U}) &&
          !asicen::cli::runtime_matches_loader(pair[1], *model, 2U, {2U, 3U, 2U}) &&
          !asicen::cli::runtime_matches_loader(pair[1], *model, 1U, {3U, 1U}) &&
          !asicen::cli::runtime_matches_loader(pair[1], *model, 1U, {}));
    auto other_model = pair[1];
    other_model.pid = 0x0006U;
    CHECK(!asicen::cli::runtime_matches_loader(other_model, *model, 1U, {2U, 3U, 1U}));
    const std::string single = asicen::cli::format_device_list({pair[1]}, true);
    CHECK(single.find("\"status\":\"incomplete\"") != std::string::npos &&
          single.find("\"receiver\":2,\"device\":2") != std::string::npos &&
          single.find("\"receiver\":0,\"device\":1") == std::string::npos);
    auto ambiguous = pair;
    auto duplicate_observation = first;
    duplicate_observation.address = 19;
    ambiguous.push_back(duplicate_observation);
    const std::string duplicate_json = asicen::cli::format_device_list(ambiguous, true);
    CHECK(duplicate_json.find("\"status\":\"duplicate\"") != std::string::npos &&
          duplicate_json.find("\"devices\":[],\"candidates\":[{") != std::string::npos);
    UsbFunctionObservation loader{0x1738, 0x5211, 1, {2, 3, 1}, 21};
    loader.parent_hub_vid = 0x05e3;
    loader.parent_hub_pid = 0x0608;
    loader.parent_hub_known = true;
    const std::string mixed = asicen::cli::format_discovery_json({loader, second});
    CHECK(mixed.find("\"model\":\"PX-W3U3\",\"serial\":null,\"serial_unique\":null,\"status\":\"incomplete\"") != std::string::npos &&
          mixed.find("\"usb\":\"1738:5211\"") != std::string::npos &&
          mixed.find("\"receiver\":2,\"device\":2") != std::string::npos);
    const std::string cold = asicen::cli::format_discovery_json({loader});
    CHECK(cold.find("\"model\":null") != std::string::npos &&
          cold.find("\"status\":\"incomplete\"") != std::string::npos &&
          cold.find("\"usb\":\"1738:5211\"") != std::string::npos &&
          cold.find("\"receivers\"") == std::string::npos);
    auto unsupported_port = loader;
    unsupported_port.port_path.back() = 3U;
    const std::string loader_on_other_port = asicen::cli::format_discovery_json(
        {unsupported_port});
    CHECK(loader_on_other_port.find("\"enclosures\":[]") != std::string::npos &&
          loader_on_other_port.find("\"ungrouped_usb_devices\":[{\"serial\":null,\"model\":null,\"usb\":\"1738:5211\"") != std::string::npos);
    UsbFunctionObservation unsupported_hub = first;
    unsupported_hub.parent_hub_vid = 0x05e3;
    unsupported_hub.parent_hub_pid = 0x0610;
    const std::string ungrouped_runtime = asicen::cli::format_discovery_json(
        {unsupported_hub, second});
    CHECK(ungrouped_runtime.find("\"model\":\"PX-W3U3\"") != std::string::npos &&
          ungrouped_runtime.find("\"status\":\"ready\"") == std::string::npos &&
          ungrouped_runtime.find("\"receiver\":2,\"device\":2") != std::string::npos &&
          ungrouped_runtime.find("\"receiver\":0,\"device\":1") == std::string::npos);
    UsbFunctionObservation single_a{0x0b06, 0x0001, 1, {2, 3, 1}, 22};
    single_a.parent_hub_vid = 0x05e3;
    single_a.parent_hub_pid = 0x0608;
    single_a.parent_hub_known = true;
    UsbFunctionObservation single_b{0x0b06, 0x0001, 1, {2, 3, 2}, 23};
    single_b.parent_hub_vid = 0x05e3;
    single_b.parent_hub_pid = 0x0608;
    single_b.parent_hub_known = true;
    const std::string ambiguous_models = asicen::cli::format_discovery_json(
        {single_a, single_b});
    CHECK(ambiguous_models.find("\"model\":null") != std::string::npos &&
          ambiguous_models.find("\"id\":1,\"usb\":\"0b06:0001\"") != std::string::npos &&
          ambiguous_models.find("\"id\":2,\"usb\":\"0b06:0001\"") != std::string::npos &&
          ambiguous_models.find("\"receivers\"") == std::string::npos);
    UsbFunctionObservation missing_parent = first;
    missing_parent.parent_hub_known = false;
    const std::string unknown_parent = asicen::cli::format_discovery_json(
        {missing_parent, second});
    CHECK(unknown_parent.find("\"model\":\"PX-W3U3\"") != std::string::npos &&
          unknown_parent.find("\"status\":\"ready\"") == std::string::npos &&
          unknown_parent.find("\"receiver\":2,\"device\":2") != std::string::npos &&
          unknown_parent.find("\"receiver\":0,\"device\":1") == std::string::npos);
    const std::string incomplete_scan = asicen::cli::format_discovery_json({loader}, false);
    CHECK(incomplete_scan.find("\"scan\":{\"complete\":false,\"issues\":[{\"code\":\"usb_observation_incomplete\"") != std::string::npos);
    for (const std::uint16_t pid : {0x0001U, 0x0003U}) {
        const std::string single = asicen::cli::format_device_list({{0x0b06, pid, 2, {4}, 9}}, true);
        CHECK(single.find("\"status\":\"ready\"") != std::string::npos &&
              single.find("\"port\":\"2-4\"") != std::string::npos &&
              single.find("\"receivers\":[{") != std::string::npos &&
              single.find("\"ungrouped_usb_devices\":[]") != std::string::npos);
    }
    CHECK(asicen::cli::format_device_list({}, true) ==
          "{\"schema\":{\"name\":\"userland.discovery\",\"major\":1,\"minor\":0},\"producer\":{\"product\":\"asicen\",\"version\":\"0.1.0\"},\"scan\":{\"complete\":true,\"issues\":[]},\"enclosures\":[],\"ungrouped_usb_devices\":[]}\n");
    CHECK(asicen::cli::format_device_list({}, false).empty());
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
    if (!arguments_match_reference() || !selector_grouping() ||
        !list_formats_share_observed_identity() ||
        !readiness_rechecks_first_and_last_probe_boundaries() ||
        !readiness_handles_staggered_reenumeration_and_disappearance() ||
        !readiness_timeout_cancel_error_and_shared_deadline() ||
        !readiness_tracks_only_cold_functions_for_single_dual_and_mixed_profiles()) {
        return 1;
    }
    std::puts("ASICEN tool parity tests passed (no USB access)");
    return 0;
}
