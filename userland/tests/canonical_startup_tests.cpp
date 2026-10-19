// SPDX-License-Identifier: GPL-2.0-only
// Offline checks for the adopted daemon startup: `--device usb:` positions,
// warm runtime without transfer, shared cold loader plan, runtime model from
// the same position, and the existing claim/release order for that model.
#include "asicend_args.h"
#include "asicend_identity.h"
#include "asicen/firmware.h"
#include "asicen/hardware_ownership.h"
#include "asicen/loader_firmware.h"
#include "asicen/startup_readiness.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <utility>
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

using asicen::cli::SelectedFunction;
using asicen::cli::SelectedState;
using asicen::cli::StartupKind;
using Error = px4::userland::Error;

asicen::cli::DaemonArguments parse(std::initializer_list<const char*> options, bool mock = false)
{
    std::vector<const char*> arguments{"asicend"};
    arguments.insert(arguments.end(), options.begin(), options.end());
    return asicen::cli::parse_daemon_arguments(
        static_cast<int>(arguments.size()), arguments.data(), mock);
}

bool rejected(const asicen::cli::DaemonArguments& arguments, int status)
{
    return !arguments.valid && arguments.error_status == status;
}

bool device_grammar_assigns_roles_from_positions()
{
    for (const char* selector : {"usb:1-2.1,1-2.2", "usb:1-2.2,1-2.1"}) {
        for (const char* option : {"--device", "-d"}) {
            const auto parsed = parse({option, selector, "--instance", "test"});
            CHECK(parsed.valid && parsed.usb_path_count == 2U);
            CHECK(parsed.usb_paths[0] == "1-2.1" && parsed.usb_paths[1] == "1-2.2");
        }
    }
    // Text order is not numeric order; roles still come from the last port.
    const auto wide = parse({"-d", "usb:1-10.2,1-10.1", "--instance", "test"});
    CHECK(wide.valid && wide.usb_paths[0] == "1-10.1" && wide.usb_paths[1] == "1-10.2");
    const auto single = parse({"--device", "usb:3-7", "--instance", "test", "-F", "fw.bin"});
    CHECK(single.valid && single.usb_path_count == 1U && single.usb_paths[0] == "3-7" &&
          single.firmware == "fw.bin" && single.expected_model == nullptr);
    const auto upper = parse({"--device", "usb:255-255.255", "--instance", "test",
                              "--firmware", "fw.bin"});
    CHECK(upper.valid && upper.firmware == "fw.bin");
    CHECK(parse({"--device", "usb:1-1.2.3.4.5.6.7.8", "--instance", "test"}).valid);
    // Existing FD startup, instance, runtime and group options are unchanged.
    CHECK(parse({"--fd", "3", "--fd", "4", "--firmware", "fw.bin"}).valid);
    const auto full = parse({"-d", "usb:1-2.1,1-2.2", "--instance", "living",
                             "--runtime-dir", "/run/x", "--group"});
    CHECK(full.valid && full.instance == "living" && full.runtime_directory == "/run/x" &&
          full.group);
    return true;
}

bool device_grammar_rejections()
{
    // Core syntax failures are argument errors.
    for (const char* selector : {"usb:", "usb:1:2", "usb:0-1", "usb:1-0", "usb:01-2",
                                 "usb:1-2.1,1-2.1", "usb:1-2,", "1-2", "serial:",
                                 "usb:1-2 ", "USB:1-2"}) {
        CHECK(rejected(parse({"--device", selector, "--instance", "test"}), 2));
    }
    // Valid selectors that no ASICEN function can satisfy fail as "no target".
    for (const char* selector : {"serial:ABC123", "serial:12345678901234",
                                 "usb:1-256", "usb:256-1", "usb:1-1.2.3.4.5.6.7.8.9",
                                 "usb:1-2.1,1-2.2,1-2.3"}) {
        CHECK(rejected(parse({"--device", selector, "--instance", "test"}), 3));
    }
    // Positions that are not one enclosure's .1/.2 pair are refused before USB.
    for (const char* selector : {"usb:1-2.1,1-3.2", "usb:1-2.1,2-2.2", "usb:1-2.1,1-2.3",
                                 "usb:1-2.2,1-2.3", "usb:1-2,1-3", "usb:1-2.1,1-2.1.2",
                                 "usb:1-2.1.1,1-2.2.2"}) {
        CHECK(rejected(parse({"--device", selector, "--instance", "test"}), 2));
    }
    // Syntax and combination errors take precedence over the serial result.
    CHECK(rejected(parse({"--device", "serial:ABC"}), 2));
    CHECK(rejected(parse({"--device", "serial:ABC", "--fd", "3"}), 2));
    // Removed options have no alias.
    CHECK(rejected(parse({"--usb-path", "1-2", "--instance", "test"}), 2));
    CHECK(rejected(parse({"--usb-path", "1:2", "--instance", "test"}), 2));
    CHECK(rejected(parse({"--device", "usb:1-2", "--instance", "test", "--model", "w3u3"}), 2));
    CHECK(rejected(parse({"--model", "w3u3"}), 2));
    CHECK(rejected(parse({"--device", "usb:1-2", "-d", "usb:1-3", "--instance", "test"}), 2));
    CHECK(rejected(parse({"--device", "usb:1-2", "--instance", "test",
                          "--firmware", "a", "-F", "b"}), 2));
    CHECK(rejected(parse({"--device", "--instance", "test"}), 2));
    CHECK(rejected(parse({"--device", "usb:1-2", "--fd", "3", "--instance", "test"}), 2));
    CHECK(rejected(parse({"--instance", "test"}), 2));
    // The mock still selects its model and never accepts device/firmware.
    CHECK(parse({"--model", "w3u2"}, true).valid &&
          parse({"--model", "w3u2"}, true).expected_model != nullptr);
    for (const char* option : {"--device", "-d", "--firmware", "-F"})
        CHECK(rejected(parse({option, "usb:1-2"}, true), 2));
    return true;
}

bool shared_loader_plan_and_image_policy(const char* scratch)
{
    const auto& common = asicen::common_loader_firmware_manifest();
    const auto* windows = asicen::loader_firmware_manifest(asicen::ModelId::W3u3V2);
    CHECK(windows == &common && common.size == 16384U && common.start_address == 0x55d6U);
    CHECK(common.sha256[0] == 0x5dU && common.sha256[1] == 0xb3U &&
          common.sha256[30] == 0x6aU && common.sha256[31] == 0x2aU);
    const auto plan = asicen::build_common_loader_transfer_plan();
    const auto reference = asicen::build_loader_transfer_plan(asicen::ModelId::W3u3V2);
    CHECK(plan.size() == 20U && reference.size() == plan.size());
    for (std::size_t i = 0; i < plan.size(); ++i) {
        CHECK(plan[i].index == 0x55d6U && plan[i].length == 512U &&
              plan[i].request == (i + 1U == plan.size() ? 0xacU : 0xabU));
        CHECK(plan[i].request == reference[i].request && plan[i].value == reference[i].value &&
              plan[i].blob_offset == reference[i].blob_offset);
    }
    if (scratch == nullptr) return true;
    for (const std::size_t size : {std::size_t{16384U}, std::size_t{16383U}}) {
        std::FILE* file = std::fopen(scratch, "wb");
        CHECK(file != nullptr);
        const std::vector<unsigned char> zeros(size, 0U);
        CHECK(std::fwrite(zeros.data(), 1U, zeros.size(), file) == zeros.size());
        CHECK(std::fclose(file) == 0);
        const auto loaded = asicen::FirmwareProvider(scratch).load();
        CHECK(!loaded && loaded.error() == Error::FIRMWARE_REJECTED);
    }
    CHECK(std::remove(scratch) == 0);
    const auto missing = asicen::FirmwareProvider(scratch).load();
    CHECK(!missing && missing.error() == Error::NOT_FOUND);
    return true;
}

SelectedFunction function(std::uint16_t vid, std::uint16_t pid)
{
    return asicen::cli::classify_selected_function(vid, pid);
}

bool warm_runtime_never_transfers()
{
    const auto* w3u2 = asicen::find_profile(asicen::ModelId::W3u2);
    const auto* w3u3 = asicen::find_profile(asicen::ModelId::W3u3);
    const auto* s3u = asicen::find_profile(asicen::ModelId::S3u);
    for (const bool firmware : {true, false}) {
        std::array<SelectedFunction, 2U> pair{{function(0x0b06, 0x0005),
                                               function(0x0b06, 0x0005)}};
        auto decision = asicen::cli::decide_startup(pair.data(), 2U, firmware);
        CHECK(decision.kind == StartupKind::warm && decision.profile == w3u3);
        pair = {{function(0x0b06, 0x0004), function(0x0b06, 0x0004)}};
        decision = asicen::cli::decide_startup(pair.data(), 2U, firmware);
        CHECK(decision.kind == StartupKind::warm && decision.profile == w3u2);
        const SelectedFunction one = function(0x0b06, 0x0001);
        decision = asicen::cli::decide_startup(&one, 1U, firmware);
        CHECK(decision.kind == StartupKind::warm && decision.profile == s3u);
    }
    // Without firmware a warm primary keeps the existing loader-sibling reservation.
    std::array<SelectedFunction, 2U> reserved{{function(0x0b06, 0x0005),
                                               function(0x1738, 0x5211)}};
    auto decision = asicen::cli::decide_startup(reserved.data(), 2U, false);
    CHECK(decision.kind == StartupKind::warm && decision.profile == w3u3);
    // With firmware only the loader sibling is cold; the primary decided the model.
    decision = asicen::cli::decide_startup(reserved.data(), 2U, true);
    CHECK(decision.kind == StartupKind::cold && decision.profile == w3u3);
    return true;
}

bool cold_loader_has_no_model_until_reenumeration()
{
    for (const std::uint16_t pid : {std::uint16_t{0x5211U}, std::uint16_t{0x5216U}}) {
        const SelectedFunction loader = function(0x1738, pid);
        CHECK(loader.state == SelectedState::loader && loader.profile == nullptr);
    }
    std::array<SelectedFunction, 2U> cold{{function(0x1738, 0x5211),
                                           function(0x1738, 0x5211)}};
    auto decision = asicen::cli::decide_startup(cold.data(), 2U, true);
    CHECK(decision.kind == StartupKind::cold && decision.profile == nullptr);
    // A declared sibling may be absent while the primary loader is provisioned.
    cold[1] = SelectedFunction{};
    decision = asicen::cli::decide_startup(cold.data(), 2U, true);
    CHECK(decision.kind == StartupKind::cold && decision.profile == nullptr);
    decision = asicen::cli::decide_startup(cold.data(), 1U, false);
    CHECK(decision.kind == StartupKind::refused && decision.error == Error::UNSUPPORTED);
    const SelectedFunction unknown = function(0x1234, 0x5678);
    CHECK(unknown.state == SelectedState::unknown);
    std::array<SelectedFunction, 2U> mixed{{function(0x1738, 0x5211), unknown}};
    decision = asicen::cli::decide_startup(mixed.data(), 2U, true);
    CHECK(decision.kind == StartupKind::refused && decision.error == Error::UNSUPPORTED);
    const SelectedFunction absent{};
    decision = asicen::cli::decide_startup(&absent, 1U, true);
    CHECK(decision.kind == StartupKind::refused && decision.error == Error::NOT_FOUND);
    CHECK(asicen::cli::decide_startup(cold.data(), 3U, true).kind == StartupKind::refused);
    return true;
}

bool runtime_model_comes_from_the_same_position()
{
    using Observation = asicen::UsbFunctionObservation;
    const std::vector<std::uint8_t> primary{2U, 1U};
    const auto at = [&](std::vector<Observation> observations) {
        return asicen::cli::find_runtime_at_position(observations, 1U, primary);
    };
    auto found = at({{0x0b06, 0x0004, 1, {2, 1}, 9}});
    CHECK(found.error == Error::OK && found.profile == asicen::find_profile(asicen::ModelId::W3u2));
    found = at({{0x0b06, 0x0005, 1, {2, 2}, 7}, {0x0b06, 0x0005, 1, {2, 1}, 9}});
    CHECK(found.error == Error::OK && found.index == 1U &&
          found.profile == asicen::find_profile(asicen::ModelId::W3u3));
    // Another position, another bus, or a loader still present are not ready.
    CHECK(at({{0x0b06, 0x0005, 1, {2, 2}, 9}}).error == Error::NOT_FOUND);
    CHECK(at({{0x0b06, 0x0005, 2, {2, 1}, 9}}).error == Error::NOT_FOUND);
    CHECK(at({{0x0b06, 0x0005, 1, {2, 1, 1}, 9}}).error == Error::NOT_FOUND);
    CHECK(at({{0x1738, 0x5211, 1, {2, 1}, 9}, {0x0b06, 0x0005, 1, {3, 1}, 4}}).error ==
          Error::NOT_FOUND);
    CHECK(at({}).error == Error::NOT_FOUND);
    // Unknown IDs at the position are refused; duplicates are ambiguous.
    CHECK(at({{0x1234, 0x5678, 1, {2, 1}, 9}}).error == Error::UNSUPPORTED);
    CHECK(at({{0x0b06, 0x0004, 1, {2, 1}, 9}, {0x0b06, 0x0005, 1, {2, 1}, 10}}).error ==
          Error::BUSY);
    CHECK(asicen::cli::find_runtime_at_position({}, 1U, {}).error == Error::INVALID_ARGUMENT);

    // Readiness over a staged re-enumeration: loader, then the runtime.
    std::vector<asicen::UploadedRuntimeIdentity> uploaded{{0U, 1U, primary, 0U, 0U}};
    CHECK(uploaded[0].vendor_id == 0U && uploaded[0].product_id == 0U);
    const std::vector<std::vector<Observation>> stages{
        {{0x1738, 0x5211, 1, {2, 1}, 9}},
        {},
        {{0x0b06, 0x0005, 1, {2, 1}, 11}}};
    std::chrono::steady_clock::time_point now{};
    std::size_t polls = 0U;
    const asicen::DeviceProfile* observed = nullptr;
    const auto probe_stage = [&](const asicen::UploadedRuntimeIdentity& identity) {
        const auto& stage = stages[polls < stages.size() ? polls : stages.size() - 1U];
        ++polls;
        const auto result = asicen::cli::find_runtime_at_position(stage, identity.bus,
                                                                  identity.port_path);
        if (result.error == Error::OK) observed = result.profile;
        return result.error;
    };
    const Error ready = asicen::wait_for_uploaded_runtimes(
        uploaded, probe_stage, [&] { return now; },
        [&](std::chrono::milliseconds duration) { now += duration; }, [] { return false; });
    CHECK(ready == Error::OK && polls == 3U &&
          observed == asicen::find_profile(asicen::ModelId::W3u3));

    // A runtime appearing only at another position never satisfies readiness.
    now = {};
    const Error elsewhere = asicen::wait_for_uploaded_runtimes(
        uploaded,
        [&](const asicen::UploadedRuntimeIdentity& identity) {
            return asicen::cli::find_runtime_at_position(
                {{0x0b06, 0x0005, 1, {2, 2}, 11}}, identity.bus, identity.port_path).error;
        },
        [&] { return now; },
        [&](std::chrono::milliseconds duration) { now += duration; }, [] { return false; });
    CHECK(elsewhere == Error::TIMEOUT);
    now = {};
    const Error unknown = asicen::wait_for_uploaded_runtimes(
        uploaded,
        [&](const asicen::UploadedRuntimeIdentity& identity) {
            return asicen::cli::find_runtime_at_position(
                {{0x0b06, 0x0099, 1, {2, 1}, 11}}, identity.bus, identity.port_path).error;
        },
        [&] { return now; },
        [&](std::chrono::milliseconds duration) { now += duration; }, [] { return false; });
    CHECK(unknown == Error::UNSUPPORTED && now == std::chrono::steady_clock::time_point{});
    return true;
}

class FakeFunction final : public asicen::UsbFunctionClaim {
public:
    FakeFunction(asicen::UsbFunctionSnapshot snapshot, std::vector<std::string>* events,
                 std::string name)
        : state(std::move(snapshot)), events(events), name(std::move(name))
    {
    }

    asicen::UsbFunctionSnapshot snapshot() const override { return state; }
    int claim_interface0() override
    {
        events->push_back("claim-" + name);
        return 0;
    }
    int release_interface0() noexcept override
    {
        events->push_back("release-" + name);
        return 0;
    }

    asicen::UsbFunctionSnapshot state;
    std::vector<std::string>* events;
    std::string name;
};

asicen::UsbFunctionSnapshot runtime_snapshot(std::uint16_t pid, std::uint8_t last,
                                             std::uint8_t address)
{
    asicen::UsbFunctionSnapshot s{};
    s.vendor_id = 0x0b06;
    s.product_id = pid;
    s.bus = 1;
    s.address = address;
    s.port_path = {2, last};
    s.interface0_present = true;
    s.kernel_driver_state_known = true;
    s.active_alt0 = 0;
    s.endpoint82_in_alt0 = last == 1U;
    s.endpoint82_bulk_in_alt0 = last == 1U;
    return s;
}

bool observed_profile_keeps_claim_order_and_refusals()
{
    const auto observed = asicen::cli::find_runtime_at_position(
        {{0x0b06, 0x0004, 1, {2, 1}, 11}}, 1U, {2U, 1U});
    CHECK(observed.error == Error::OK && observed.profile != nullptr);
    std::vector<std::string> events;
    {
        FakeFunction primary(runtime_snapshot(0x0004, 1U, 11U), &events, "primary");
        FakeFunction sibling(runtime_snapshot(0x0004, 2U, 12U), &events, "sibling");
        asicen::EnclosureOwnership owner;
        CHECK(owner.claim_profile(*observed.profile, primary, &sibling, {2, 1}, {2, 2},
                                  asicen::SiblingIdentityPolicy::runtime_required) ==
              asicen::OwnershipError::none);
    }
    CHECK((events == std::vector<std::string>{"claim-primary", "claim-sibling", "release-sibling",
                                              "release-primary"}));
    // A sibling reporting another model or still a loader is refused before any claim.
    for (const std::uint16_t pid : {std::uint16_t{0x0005U}, std::uint16_t{0x5211U}}) {
        events.clear();
        FakeFunction primary(runtime_snapshot(0x0004, 1U, 11U), &events, "primary");
        auto other = runtime_snapshot(pid, 2U, 12U);
        if (pid == 0x5211U) other.vendor_id = 0x1738U;
        FakeFunction sibling(other, &events, "sibling");
        asicen::EnclosureOwnership owner;
        CHECK(owner.claim_profile(*observed.profile, primary, &sibling, {2, 1}, {2, 2},
                                  asicen::SiblingIdentityPolicy::runtime_required) ==
              asicen::OwnershipError::sibling_mismatch);
        CHECK(events.empty());
    }
    // The .2 function can never become the primary.
    events.clear();
    {
        FakeFunction primary(runtime_snapshot(0x0004, 2U, 12U), &events, "primary");
        FakeFunction sibling(runtime_snapshot(0x0004, 1U, 11U), &events, "sibling");
        asicen::EnclosureOwnership owner;
        CHECK(owner.claim_profile(*observed.profile, primary, &sibling, {2, 2}, {2, 1},
                                  asicen::SiblingIdentityPolicy::runtime_required) !=
              asicen::OwnershipError::none);
    }
    CHECK(events.empty());
    return true;
}

}  // namespace

int main(int argc, char** argv)
{
    const char* scratch = argc >= 2 ? argv[1] : nullptr;
    if (!device_grammar_assigns_roles_from_positions() || !device_grammar_rejections() ||
        !shared_loader_plan_and_image_policy(scratch) || !warm_runtime_never_transfers() ||
        !cold_loader_has_no_model_until_reenumeration() ||
        !runtime_model_comes_from_the_same_position() ||
        !observed_profile_keeps_claim_order_and_refusals()) {
        return 1;
    }
    std::puts("canonical startup checks passed (no hardware access)");
    return 0;
}
