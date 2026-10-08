// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/hardware_ownership.h"
#include "asicen/protocol.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

class FakeFunction final : public asicen::UsbFunctionClaim {
public:
    explicit FakeFunction(asicen::UsbFunctionSnapshot snapshot,
                           std::vector<std::string>* events,
                           std::string name)
        : state(std::move(snapshot)), events(events), name(std::move(name)) {}

    asicen::UsbFunctionSnapshot snapshot() const override { return state; }
    int claim_interface0() override {
        events->push_back("claim-" + name);
        if (claim_result == 0) claimed = true;
        return claim_result;
    }
    int release_interface0() noexcept override {
        events->push_back("release-" + name);
        if (release_result != 0) return release_result;
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

asicen::UsbFunctionSnapshot primary_snapshot() {
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

asicen::UsbFunctionSnapshot sibling_snapshot() {
    auto s = primary_snapshot();
    s.address = 6;
    s.port_path = {1, 2, 2};
    s.endpoint82_in_alt0 = false;
    return s;
}

void valid_runtime_claims_and_releases_in_reverse_order() {
    std::vector<std::string> events;
    FakeFunction primary(primary_snapshot(), &events, "primary");
    FakeFunction sibling(sibling_snapshot(), &events, "sibling");
    {
        asicen::EnclosureOwnership owner;
        check(owner.claim_w3u3(primary, sibling, {1, 2, 1}, {1, 2, 2}) ==
                  asicen::OwnershipError::none,
              "runtime pair should claim");
        check(owner.owns_both() && primary.claimed && sibling.claimed,
              "both interfaces are held");
    }
    check(events == std::vector<std::string>{"claim-primary", "claim-sibling",
                                             "release-sibling", "release-primary"},
          "claim order and reverse release order");
}

void known_loader_sibling_is_reserved_and_claimed() {
    std::vector<std::string> events;
    FakeFunction primary(primary_snapshot(), &events, "primary");
    auto sibling_state = sibling_snapshot();
    sibling_state.vendor_id = 0x1738;
    sibling_state.product_id = 0x5216;
    FakeFunction sibling(sibling_state, &events, "sibling");
    asicen::EnclosureOwnership owner;
    check(owner.claim_w3u3(primary, sibling, {1, 2, 1}, {1, 2, 2}) ==
              asicen::OwnershipError::none,
          "known loader sibling is accepted");
    check(owner.owns_both(), "loader sibling remains reserved");
}

void primary_endpoint81_capability_is_optional_and_measured() {
    check(asicen::bulk_endpoint_for_local(0U) == 0x81U &&
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
    check(owner.claim_w3u3(primary, sibling, {1, 2, 1}, {1, 2, 2}) ==
              asicen::OwnershipError::none,
          "legacy lane-1 endpoint remains the baseline claim contract");
    check(owner.primary_supports_bulk_endpoint(0x82U),
          "lane 1 capability follows the original contract");
    check(owner.primary_supports_bulk_endpoint(0x81U),
          "lane 0 is reported only when endpoint 81 is present and bulk");
    owner.release();

    primary_state.endpoint81_bulk_in_alt0 = false;
    FakeFunction no_lane0(primary_state, &events, "primary-no-lane0");
    FakeFunction sibling_again(sibling_snapshot(), &events, "sibling-again");
    asicen::EnclosureOwnership owner_without_lane0;
    check(owner_without_lane0.claim_w3u3(no_lane0, sibling_again,
                                         {1, 2, 1}, {1, 2, 2}) ==
              asicen::OwnershipError::none,
          "lane 0 absence does not invalidate existing lane-1 ownership");
    check(!owner_without_lane0.primary_supports_bulk_endpoint(0x81U),
          "non-bulk lane 0 is unavailable");
}

void receiver_lane_reservation_rejects_cross_lane_ownership() {
    asicen::ReceiverLaneReservation reservation;
    check(reservation.reserve(0U) == asicen::ReceiverReservationResult::reserved,
          "primary satellite lane reserves shared frontend state");
    check(reservation.reserve(1U) == asicen::ReceiverReservationResult::busy,
          "terrestrial lane cannot interleave while primary owns shared state");
    check(reservation.reserve(0U) == asicen::ReceiverReservationResult::already_owned,
          "same lease lane can be observed idempotently");
    check(reservation.release(1U) == false,
          "non-owner cannot clear the active lane reservation");
    check(reservation.release(0U), "owner releases the lane after cleanup");
    check(reservation.reserve(1U) == asicen::ReceiverReservationResult::reserved,
          "terrestrial lane can acquire after prior release");
    check(reservation.reserve(2U) == asicen::ReceiverReservationResult::invalid,
          "unsupported receiver cannot reserve shared frontend state");
}

void wrong_topology_or_busy_interfaces_fail_before_claim() {
    const auto run = [](asicen::UsbFunctionSnapshot p,
                        asicen::UsbFunctionSnapshot s,
                        asicen::OwnershipError expected) {
        std::vector<std::string> events;
        FakeFunction primary(std::move(p), &events, "primary");
        FakeFunction sibling(std::move(s), &events, "sibling");
        asicen::EnclosureOwnership owner;
        check(owner.claim_w3u3(primary, sibling, {1, 2, 1}, {1, 2, 2}) == expected,
              "reject invalid candidate");
        check(events.empty(), "invalid candidate rejected before any interface claim");
    };
    auto p = primary_snapshot();
    p.port_path = {1, 9, 9};
    run(p, sibling_snapshot(), asicen::OwnershipError::primary_mismatch);
    p = primary_snapshot();
    p.endpoint82_in_alt0 = false;
    run(p, sibling_snapshot(), asicen::OwnershipError::primary_mismatch);
    p = primary_snapshot();
    p.endpoint82_bulk_in_alt0 = false;
    run(p, sibling_snapshot(), asicen::OwnershipError::primary_mismatch);
    p = primary_snapshot();
    p.interface0_kernel_driver = true;
    run(p, sibling_snapshot(), asicen::OwnershipError::interface_unavailable);
    auto s = sibling_snapshot();
    s.product_id = 0x5217;
    run(primary_snapshot(), s, asicen::OwnershipError::sibling_mismatch);
    s = sibling_snapshot();
    s.active_alt0 = -1;
    run(primary_snapshot(), s, asicen::OwnershipError::interface_unavailable);
    s = sibling_snapshot();
    s.port_path = {1, 3, 2};
    run(primary_snapshot(), s, asicen::OwnershipError::sibling_mismatch);
    s = sibling_snapshot();
    s.port_path = {1, 2, 1};
    run(primary_snapshot(), s, asicen::OwnershipError::sibling_mismatch);
    s = sibling_snapshot();
    s.port_path = {1, 2, 0};
    run(primary_snapshot(), s, asicen::OwnershipError::sibling_mismatch);
}

void failed_release_remains_visible_and_blocks_reclaim() {
    std::vector<std::string> events;
    FakeFunction primary(primary_snapshot(), &events, "primary");
    FakeFunction sibling(sibling_snapshot(), &events, "sibling");
    asicen::EnclosureOwnership owner;
    check(owner.claim_w3u3(primary, sibling, {1, 2, 1}, {1, 2, 2}) ==
              asicen::OwnershipError::none,
          "initial claim succeeds");
    sibling.release_result = -1;
    check(owner.release() == asicen::OwnershipError::release_failed,
          "failed sibling release is reported");
    check(!owner.owns_both(), "only sibling claim remains held");
    check(owner.claim_w3u3(primary, sibling, {1, 2, 1}, {1, 2, 2}) ==
              asicen::OwnershipError::release_failed,
          "cannot reuse session while prior release is unresolved");
    sibling.release_result = 0;
    check(owner.release() == asicen::OwnershipError::none,
          "release can be retried");
}

void second_claim_failure_rolls_back_first_without_writes() {
    std::vector<std::string> events;
    FakeFunction primary(primary_snapshot(), &events, "primary");
    FakeFunction sibling(sibling_snapshot(), &events, "sibling");
    sibling.claim_result = -6;
    asicen::EnclosureOwnership owner;
    check(owner.claim_w3u3(primary, sibling, {1, 2, 1}, {1, 2, 2}) ==
              asicen::OwnershipError::sibling_claim_failed,
          "second claim failure is returned");
    check(!owner.owns_both() && !primary.claimed && !sibling.claimed,
          "partial ownership is rolled back");
    check(events == std::vector<std::string>{"claim-primary", "claim-sibling",
                                             "release-primary"},
          "no vendor writes exist in ownership path; only claim rollback occurs");
}

void invalid_paths_reject_before_claim() {
    std::vector<std::string> events;
    FakeFunction primary(primary_snapshot(), &events, "primary");
    FakeFunction sibling(sibling_snapshot(), &events, "sibling");
    asicen::EnclosureOwnership owner;
    check(owner.claim_w3u3(primary, sibling, {}, {1, 2, 2}) ==
              asicen::OwnershipError::invalid_path,
          "empty path rejected");
    check(owner.claim_w3u3(primary, sibling, {1, 2, 1}, {1, 2, 1}) ==
              asicen::OwnershipError::invalid_path,
          "duplicate paths rejected");
    check(events.empty(), "path errors cause no claim calls");
}

}  // namespace

int main() {
    valid_runtime_claims_and_releases_in_reverse_order();
    known_loader_sibling_is_reserved_and_claimed();
    primary_endpoint81_capability_is_optional_and_measured();
    receiver_lane_reservation_rejects_cross_lane_ownership();
    wrong_topology_or_busy_interfaces_fail_before_claim();
    second_claim_failure_rolls_back_first_without_writes();
    failed_release_remains_visible_and_blocks_reclaim();
    invalid_paths_reject_before_claim();
    std::cout << "hardware ownership tests passed\n";
    return 0;
}
