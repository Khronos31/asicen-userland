// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/hardware_ownership.h"
#include "asicen/protocol.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#include <array>
#include <atomic>
#include <thread>

namespace {

bool expected_assertion_failure = false;

bool check(bool condition, const char* message)
{
    if (!condition) {
        if (!expected_assertion_failure) {
            std::cerr << "FAIL: " << message << '\n';
        }
        return false;
    }
    return true;
}

#define CHECK(...)                                                                                 \
    do {                                                                                           \
        if (!check(__VA_ARGS__)) {                                                                 \
            return false;                                                                          \
        }                                                                                          \
    } while (false)

class FakeFunction final : public asicen::UsbFunctionClaim {
public:
    explicit FakeFunction(asicen::UsbFunctionSnapshot snapshot, std::vector<std::string>* events,
                          std::string name)
        : state(std::move(snapshot)), events(events), name(std::move(name))
    {
    }

    asicen::UsbFunctionSnapshot snapshot() const override { return state; }
    int claim_interface0() override
    {
        events->push_back("claim-" + name);
        if (claim_result == 0) {
            claimed = true;
        }
        return claim_result;
    }
    int release_interface0() noexcept override
    {
        events->push_back("release-" + name);
        if (release_result != 0) {
            return release_result;
        }
        claimed = false;
        return 0;
    }

    asicen::UsbFunctionSnapshot state;
    std::vector<std::string>* events;
    std::string name;
    int claim_result = 0;
    int release_result = 0;
    bool claimed = false;
};

asicen::UsbFunctionSnapshot primary_snapshot()
{
    asicen::UsbFunctionSnapshot s{};
    s.vendor_id = 0x0b06;
    s.product_id = 0x0005;
    s.bus = 1;
    s.address = 4;
    s.port_path = {1, 2, 1};
    s.interface0_present = true;
    s.kernel_driver_state_known = true;
    s.active_alt0 = 0;
    s.endpoint82_in_alt0 = true;
    s.endpoint82_bulk_in_alt0 = true;
    return s;
}

asicen::UsbFunctionSnapshot sibling_snapshot()
{
    auto s = primary_snapshot();
    s.address = 6;
    s.port_path = {1, 2, 2};
    s.endpoint82_in_alt0 = false;
    return s;
}

bool valid_runtime_claims_and_releases_in_reverse_order()
{
    std::vector<std::string> events;
    FakeFunction primary(primary_snapshot(), &events, "primary");
    FakeFunction sibling(sibling_snapshot(), &events, "sibling");
    {
        asicen::EnclosureOwnership owner;
        CHECK(owner.claim_w3u3(primary, sibling, {1, 2, 1}, {1, 2, 2}) ==
                  asicen::OwnershipError::none,
              "runtime pair should claim");
        CHECK(owner.owns_both() && primary.claimed && sibling.claimed, "both interfaces are held");
    }
    CHECK(events == std::vector<std::string>{"claim-primary", "claim-sibling", "release-sibling",
                                             "release-primary"},
          "claim order and reverse release order");
    return true;
}

bool assertion_failure_unwinds_claimed_interfaces()
{
    std::vector<std::string> events;
    FakeFunction primary(primary_snapshot(), &events, "primary");
    FakeFunction sibling(sibling_snapshot(), &events, "sibling");
    const auto fail_in_scope = [&]() {
        asicen::EnclosureOwnership owner;
        CHECK(owner.claim_w3u3(primary, sibling, {1, 2, 1}, {1, 2, 2}) ==
                  asicen::OwnershipError::none,
              "failure fixture owns both interfaces");
        expected_assertion_failure = true;
        CHECK(false, "deliberate assertion failure after acquisition");
        return true;
    };
    const bool passed = fail_in_scope();
    expected_assertion_failure = false;
    CHECK(!passed && !primary.claimed && !sibling.claimed,
          "CHECK failure returns through every automatic owner destructor");
    CHECK(events == std::vector<std::string>{"claim-primary", "claim-sibling", "release-sibling",
                                             "release-primary"},
          "assertion failure preserves total reverse-release cleanup order");
    return true;
}

bool known_loader_sibling_is_reserved_and_claimed()
{
    std::vector<std::string> events;
    FakeFunction primary(primary_snapshot(), &events, "primary");
    auto sibling_state = sibling_snapshot();
    sibling_state.vendor_id = 0x1738;
    sibling_state.product_id = 0x5216;
    FakeFunction sibling(sibling_state, &events, "sibling");
    asicen::EnclosureOwnership owner;
    CHECK(owner.claim_w3u3(primary, sibling, {1, 2, 1}, {1, 2, 2}) == asicen::OwnershipError::none,
          "known loader sibling is accepted");
    CHECK(owner.owns_both(), "loader sibling remains reserved");
    return true;
}

bool primary_endpoint81_capability_is_optional_and_measured()
{
    CHECK(asicen::bulk_endpoint_for_local(0U) == 0x81U &&
              asicen::bulk_endpoint_for_local(1U) == 0x82U &&
              asicen::bulk_endpoint_for_local(2U) == 0U,
          "local zero and one map to the proven bulk IN endpoints");
    std::vector<std::string> events;
    auto primary_state = primary_snapshot();
    primary_state.endpoint81_in_alt0 = true;
    primary_state.endpoint81_bulk_in_alt0 = true;
    FakeFunction primary(primary_state, &events, "primary");
    FakeFunction sibling(sibling_snapshot(), &events, "sibling");
    asicen::EnclosureOwnership owner;
    CHECK(owner.claim_w3u3(primary, sibling, {1, 2, 1}, {1, 2, 2}) == asicen::OwnershipError::none,
          "legacy lane-1 endpoint remains the baseline claim contract");
    CHECK(owner.primary_supports_bulk_endpoint(0x82U),
          "lane 1 capability follows the original contract");
    CHECK(owner.primary_supports_bulk_endpoint(0x81U),
          "lane 0 is reported only when endpoint 81 is present and bulk");
    owner.release();

    primary_state.endpoint81_bulk_in_alt0 = false;
    FakeFunction no_lane0(primary_state, &events, "primary-no-lane0");
    FakeFunction sibling_again(sibling_snapshot(), &events, "sibling-again");
    asicen::EnclosureOwnership owner_without_lane0;
    CHECK(owner_without_lane0.claim_w3u3(no_lane0, sibling_again, {1, 2, 1}, {1, 2, 2}) ==
              asicen::OwnershipError::none,
          "lane 0 absence does not invalidate existing lane-1 ownership");
    CHECK(!owner_without_lane0.primary_supports_bulk_endpoint(0x81U),
          "non-bulk lane 0 is unavailable");
    return true;
}

bool receiver_lane_reservation_rejects_cross_lane_ownership()
{
    asicen::ReceiverLaneReservation reservation;
    CHECK(reservation.reserve(0U) == asicen::ReceiverReservationResult::reserved,
          "primary satellite lane reserves shared frontend state");
    CHECK(reservation.reserve(1U) == asicen::ReceiverReservationResult::busy,
          "terrestrial lane cannot interleave while primary owns shared state");
    CHECK(reservation.reserve(0U) == asicen::ReceiverReservationResult::already_owned,
          "same lease lane can be observed idempotently");
    CHECK(reservation.release(1U) == false, "non-owner cannot clear the active lane reservation");
    CHECK(reservation.release(0U), "owner releases the lane after cleanup");
    CHECK(reservation.reserve(1U) == asicen::ReceiverReservationResult::reserved,
          "terrestrial lane can acquire after prior release");
    CHECK(reservation.reserve(2U) == asicen::ReceiverReservationResult::invalid,
          "unsupported receiver cannot reserve shared frontend state");
    return true;
}

bool supported_lane_release_orders_and_concurrent_acquisition()
{
    using asicen::ReceiverReservationResult;
    for (const std::uint8_t first : {0U, 1U}) {
        const auto second = static_cast<std::uint8_t>(first ^ 1U);
        asicen::ReceiverLaneReservation reservation;
        CHECK(reservation.reserve(first) == ReceiverReservationResult::reserved,
              "each supported lane can be the first owner");
        CHECK(!reservation.release(second) && reservation.owns(first),
              "reversed close order cannot release the surviving owner");
        CHECK(reservation.release(first) && !reservation.release(first),
              "duplicate release has no additional ownership effect");
        CHECK(reservation.reserve(second) == ReceiverReservationResult::reserved &&
                  reservation.release(second),
              "each supported release permutation permits the next lane to reopen");
    }
    for (unsigned round = 0U; round < 32U; ++round) {
        asicen::ReceiverLaneReservation reservation;
        std::atomic<unsigned> ready{0U};
        std::atomic<bool> begin{false};
        std::array<ReceiverReservationResult, 2> results{};
        std::array<std::thread, 2> contenders;
        for (std::uint8_t lane = 0U; lane < contenders.size(); ++lane) {
            contenders[lane] = std::thread([&, lane] {
                ready.fetch_add(1U);
                while (!begin.load()) {
                    std::this_thread::yield();
                }
                results[lane] = reservation.reserve(lane);
            });
        }
        while (ready.load() != contenders.size()) {
            std::this_thread::yield();
        }
        begin.store(true);
        for (auto& contender : contenders) {
            contender.join();
        }
        const std::uint8_t winner = results[0] == ReceiverReservationResult::reserved ? 0U : 1U;
        const auto loser = static_cast<std::uint8_t>(winner ^ 1U);
        CHECK(results[winner] == ReceiverReservationResult::reserved &&
                  results[loser] == ReceiverReservationResult::busy && reservation.owns(winner),
              "concurrent supported lanes produce exactly one stable owner");
        CHECK(!reservation.release(loser) && reservation.owns(winner),
              "losing concurrent opener cannot release the winner");
        CHECK(reservation.release(winner) &&
                  reservation.reserve(loser) == ReceiverReservationResult::reserved &&
                  reservation.release(loser),
              "joined contention has no leaked reservation and permits reacquisition");
    }
    return true;
}

bool wrong_topology_or_busy_interfaces_fail_before_claim()
{
    const auto run = [](asicen::UsbFunctionSnapshot p, asicen::UsbFunctionSnapshot s,
                        asicen::OwnershipError expected) {
        std::vector<std::string> events;
        FakeFunction primary(std::move(p), &events, "primary");
        FakeFunction sibling(std::move(s), &events, "sibling");
        asicen::EnclosureOwnership owner;
        CHECK(owner.claim_w3u3(primary, sibling, {1, 2, 1}, {1, 2, 2}) == expected,
              "reject invalid candidate");
        CHECK(events.empty(), "invalid candidate rejected before any interface claim");
        return true;
    };
    auto p = primary_snapshot();
    p.port_path = {1, 9, 9};
    CHECK(run(p, sibling_snapshot(), asicen::OwnershipError::primary_mismatch), "topology subcase");
    p = primary_snapshot();
    p.endpoint82_in_alt0 = false;
    CHECK(run(p, sibling_snapshot(), asicen::OwnershipError::primary_mismatch), "topology subcase");
    p = primary_snapshot();
    p.endpoint82_bulk_in_alt0 = false;
    CHECK(run(p, sibling_snapshot(), asicen::OwnershipError::primary_mismatch), "topology subcase");
    p = primary_snapshot();
    p.interface0_kernel_driver = true;
    CHECK(run(p, sibling_snapshot(), asicen::OwnershipError::interface_unavailable),
          "topology subcase");
    auto s = sibling_snapshot();
    s.product_id = 0x5217;
    CHECK(run(primary_snapshot(), s, asicen::OwnershipError::sibling_mismatch), "topology subcase");
    s = sibling_snapshot();
    s.active_alt0 = -1;
    CHECK(run(primary_snapshot(), s, asicen::OwnershipError::interface_unavailable),
          "topology subcase");
    s = sibling_snapshot();
    s.port_path = {1, 3, 2};
    CHECK(run(primary_snapshot(), s, asicen::OwnershipError::sibling_mismatch), "topology subcase");
    s = sibling_snapshot();
    s.port_path = {1, 2, 1};
    CHECK(run(primary_snapshot(), s, asicen::OwnershipError::sibling_mismatch), "topology subcase");
    s = sibling_snapshot();
    s.port_path = {1, 2, 0};
    CHECK(run(primary_snapshot(), s, asicen::OwnershipError::sibling_mismatch), "topology subcase");
    return true;
}

bool failed_release_remains_visible_and_blocks_reclaim()
{
    std::vector<std::string> events;
    FakeFunction primary(primary_snapshot(), &events, "primary");
    FakeFunction sibling(sibling_snapshot(), &events, "sibling");
    asicen::EnclosureOwnership owner;
    CHECK(owner.claim_w3u3(primary, sibling, {1, 2, 1}, {1, 2, 2}) == asicen::OwnershipError::none,
          "initial claim succeeds");
    sibling.release_result = -1;
    CHECK(owner.release() == asicen::OwnershipError::release_failed,
          "failed sibling release is reported");
    CHECK(!owner.owns_both(), "only sibling claim remains held");
    CHECK(owner.claim_w3u3(primary, sibling, {1, 2, 1}, {1, 2, 2}) ==
              asicen::OwnershipError::release_failed,
          "cannot reuse session while prior release is unresolved");
    sibling.release_result = 0;
    CHECK(owner.release() == asicen::OwnershipError::none, "release can be retried");
    return true;
}

bool second_claim_failure_rolls_back_first_without_writes()
{
    std::vector<std::string> events;
    FakeFunction primary(primary_snapshot(), &events, "primary");
    FakeFunction sibling(sibling_snapshot(), &events, "sibling");
    sibling.claim_result = -6;
    asicen::EnclosureOwnership owner;
    CHECK(owner.claim_w3u3(primary, sibling, {1, 2, 1}, {1, 2, 2}) ==
              asicen::OwnershipError::sibling_claim_failed,
          "second claim failure is returned");
    CHECK(!owner.owns_both() && !primary.claimed && !sibling.claimed,
          "partial ownership is rolled back");
    CHECK(events == std::vector<std::string>{"claim-primary", "claim-sibling", "release-primary"},
          "no vendor writes exist in ownership path; only claim rollback occurs");
    return true;
}

bool invalid_paths_reject_before_claim()
{
    std::vector<std::string> events;
    FakeFunction primary(primary_snapshot(), &events, "primary");
    FakeFunction sibling(sibling_snapshot(), &events, "sibling");
    asicen::EnclosureOwnership owner;
    CHECK(owner.claim_w3u3(primary, sibling, {}, {1, 2, 2}) == asicen::OwnershipError::invalid_path,
          "empty path rejected");
    CHECK(owner.claim_w3u3(primary, sibling, {1, 2, 1}, {1, 2, 1}) ==
              asicen::OwnershipError::invalid_path,
          "duplicate paths rejected");
    CHECK(events.empty(), "path errors cause no claim calls");
    return true;
}

bool single_function_profiles_claim_only_their_runtime_identity()
{
    for (const auto model : {asicen::ModelId::S3u, asicen::ModelId::S3u2}) {
        const auto& profile = *asicen::find_profile(model);
        auto state = primary_snapshot();
        state.product_id = profile.pid;
        state.port_path = {4};  // No fictitious enclosure hub is required.
        state.endpoint81_in_alt0 = true;
        state.endpoint81_bulk_in_alt0 = true;
        if (profile.combined_isdb_ts) {
            state.endpoint82_in_alt0 = false;
            state.endpoint82_bulk_in_alt0 = false;
        }
        std::vector<std::string> events;
        FakeFunction primary(state, &events, "primary");
        FakeFunction sibling(sibling_snapshot(), &events, "sibling");
        asicen::EnclosureOwnership owner;
        CHECK(owner.claim_profile(profile, primary, &sibling, {4}, {5}) ==
                  asicen::OwnershipError::invalid_path,
              "single-function profile rejects a supplied sibling");
        CHECK(events.empty(), "single-function mismatch makes no claims");
        CHECK(owner.claim_profile(profile, primary, nullptr, {4}) == asicen::OwnershipError::none,
              "one-function profile claims without an invented sibling");
        CHECK(owner.owns_required_functions() && !owner.owns_both(),
              "one claim satisfies the single-function model");
        CHECK(owner.primary_supports_bulk_endpoint(0x81U),
              "S3U combined lane zero capability is measured");
        CHECK(owner.primary_supports_bulk_endpoint(0x82U) == !profile.combined_isdb_ts,
              "endpoint82 is not inferred for the combined S3U");
        CHECK(owner.release() == asicen::OwnershipError::none, "single-function release succeeds");
        CHECK(!owner.owns_required_functions() && !owner.primary_supports_bulk_endpoint(0x81U),
              "release clears ownership and capabilities");
        CHECK(events == std::vector<std::string>{"claim-primary", "release-primary"},
              "only the requested single interface is touched");
    }
    return true;
}

bool dual_models_require_same_pid_and_source_backed_loader_identity()
{
    const auto& w3u2 = *asicen::find_profile(asicen::ModelId::W3u2);
    auto p = primary_snapshot();
    p.product_id = w3u2.pid;
    auto s = sibling_snapshot();
    s.product_id = w3u2.pid;
    std::vector<std::string> events;
    FakeFunction primary(p, &events, "primary");
    FakeFunction sibling(s, &events, "sibling");
    asicen::EnclosureOwnership owner;
    CHECK(owner.claim_profile(w3u2, primary, &sibling, p.port_path, s.port_path) ==
              asicen::OwnershipError::none,
          "W3U2 same-runtime-PID pair claims");
    CHECK(owner.owns_both() && owner.owns_required_functions(),
          "both W3U2 functions stay reserved");
    owner.release();
    events.clear();
    sibling.state.product_id = 0x0005U;
    CHECK(owner.claim_profile(w3u2, primary, &sibling, p.port_path, s.port_path) ==
              asicen::OwnershipError::sibling_mismatch,
          "W3U3 sibling is not accepted as a W3U2 function");
    CHECK(events.empty(), "cross-model sibling is rejected before claims");
    sibling.state.vendor_id = 0x1738U;
    sibling.state.product_id = 0x5211U;
    CHECK(owner.claim_profile(w3u2, primary, &sibling, p.port_path, s.port_path) ==
              asicen::OwnershipError::none,
          "official W3U2 loader INF5211 may be reserved");
    owner.release();
    events.clear();
    sibling.state.product_id = 0x5216U;
    CHECK(owner.claim_profile(w3u2, primary, &sibling, p.port_path, s.port_path) ==
              asicen::OwnershipError::sibling_mismatch,
          "W3U3 historical5216 allowance is not broadened without W3U2 evidence");
    CHECK(events.empty(), "unproven loader identity causes no claim");
    return true;
}

bool forged_profiles_and_wrong_single_endpoint_fail_closed()
{
    auto profile = *asicen::find_profile(asicen::ModelId::S3u);
    std::vector<std::string> events;
    auto state = primary_snapshot();
    state.product_id = profile.pid;
    FakeFunction primary(state, &events, "primary");
    asicen::EnclosureOwnership owner;
    CHECK(owner.claim_profile(profile, primary, nullptr, state.port_path) ==
              asicen::OwnershipError::primary_mismatch,
          "S3U requires actual bulk endpoint81, not W3U3 endpoint82");
    profile.expected_runtime_functions = 2U;
    CHECK(owner.claim_profile(profile, primary, nullptr, state.port_path) ==
              asicen::OwnershipError::unsupported_profile,
          "caller cannot forge known model topology");
    CHECK(events.empty(), "invalid model or lane never reaches claim");
    return true;
}

}  // namespace

int main()
{
    if (!supported_lane_release_orders_and_concurrent_acquisition()) {
        return 1;
    }
    if (!assertion_failure_unwinds_claimed_interfaces()) {
        return 1;
    }
    if (!single_function_profiles_claim_only_their_runtime_identity()) {
        return 1;
    }
    if (!dual_models_require_same_pid_and_source_backed_loader_identity()) {
        return 1;
    }
    if (!forged_profiles_and_wrong_single_endpoint_fail_closed()) {
        return 1;
    }
    if (!valid_runtime_claims_and_releases_in_reverse_order()) {
        return 1;
    }
    if (!known_loader_sibling_is_reserved_and_claimed()) {
        return 1;
    }
    if (!primary_endpoint81_capability_is_optional_and_measured()) {
        return 1;
    }
    if (!receiver_lane_reservation_rejects_cross_lane_ownership()) {
        return 1;
    }
    if (!wrong_topology_or_busy_interfaces_fail_before_claim()) {
        return 1;
    }
    if (!second_claim_failure_rolls_back_first_without_writes()) {
        return 1;
    }
    if (!failed_release_remains_visible_and_blocks_reclaim()) {
        return 1;
    }
    if (!invalid_paths_reject_before_claim()) {
        return 1;
    }
    std::cout << "hardware ownership tests passed\n";
    return 0;
}
