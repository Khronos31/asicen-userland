// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/hardware_ownership.h"

namespace asicen {

void EnclosureOwnership::abandon() noexcept
{
    primary_ = nullptr;
    sibling_ = nullptr;
    required_function_count_ = 0U;
    primary_endpoint81_bulk_ = false;
    primary_endpoint82_bulk_ = false;
    sibling_endpoint81_bulk_ = false;
    sibling_endpoint82_bulk_ = false;
}
namespace {

bool matches_runtime(const UsbFunctionSnapshot& s, const DeviceProfile& profile)
{
    return s.vendor_id == profile.vid && s.product_id == profile.pid;
}

bool is_permitted_sibling(const UsbFunctionSnapshot& s, const DeviceProfile& profile)
{
    if (matches_runtime(s, profile)) {
        return true;
    }
    // Official W3U2/W3U3 loader INFs bind 5211; the existing W3U3 5216
    // allowance remains reservation-only. Neither permits frontend I/O.
    // The loader allowance reserves the sibling only. It does
    // not permit frontend I/O on that function, or identify a different model.
    return s.vendor_id == 0x1738U &&
           ((profile.model_id == ModelId::W3u3 &&
             (s.product_id == 0x5211U || s.product_id == 0x5216U)) ||
            (profile.model_id == ModelId::W3u2 && s.product_id == 0x5211U));
}

bool known_topology(const DeviceProfile& profile)
{
    const auto* known = find_profile(profile.vid, profile.pid);
    return known != nullptr && known->model_id == profile.model_id &&
           known->frontend_family == profile.frontend_family &&
           known->enclosure_receiver_count == profile.enclosure_receiver_count &&
           known->expected_runtime_functions == profile.expected_runtime_functions &&
           known->local_lane_count == profile.local_lane_count &&
           known->combined_isdb_ts == profile.combined_isdb_ts;
}

bool interface_is_safe(const UsbFunctionSnapshot& s)
{
    return s.interface0_present && s.kernel_driver_state_known && !s.interface0_kernel_driver &&
           s.active_alt0 == 0;
}

bool correct_ordered_siblings(const UsbFunctionSnapshot& p, const UsbFunctionSnapshot& s)
{
    if (p.bus != s.bus || p.port_path.size() < 2U || p.port_path.size() != s.port_path.size() ||
        p.port_path.back() != 1U || s.port_path.back() != 2U) {
        return false;
    }
    for (std::size_t i = 0; i + 1U < p.port_path.size(); ++i) {
        if (p.port_path[i] != s.port_path[i]) {
            return false;
        }
    }
    return true;
}

}  // namespace

EnclosureOwnership::~EnclosureOwnership()
{
    release();
}

OwnershipError EnclosureOwnership::claim_profile(const DeviceProfile& profile,
                                                 UsbFunctionClaim& primary,
                                                 UsbFunctionClaim* sibling,
                                                 const std::vector<std::uint8_t>& primary_path,
                                                 const std::vector<std::uint8_t>& sibling_path,
                                                 SiblingIdentityPolicy sibling_policy)
{
    if (release() != OwnershipError::none) {
        return OwnershipError::release_failed;
    }
    if (!known_topology(profile) ||
        (profile.expected_runtime_functions != 1U && profile.expected_runtime_functions != 2U)) {
        return OwnershipError::unsupported_profile;
    }
    const bool paired = profile.expected_runtime_functions == 2U;
    if ((paired && (primary_path.empty() || sibling == nullptr || sibling_path.empty() ||
                    primary_path == sibling_path)) ||
        (!paired && (sibling != nullptr || !sibling_path.empty()))) {
        return OwnershipError::invalid_path;
    }

    const UsbFunctionSnapshot p = primary.snapshot();
    last_transport_error_ = p.transport_error;
    if (last_transport_error_ != 0) {
        return OwnershipError::snapshot_failed;
    }
    const bool required_endpoint = profile.combined_isdb_ts
                                       ? p.endpoint81_in_alt0 && p.endpoint81_bulk_in_alt0
                                       : p.endpoint82_in_alt0 && p.endpoint82_bulk_in_alt0;
    if (!matches_runtime(p, profile) || p.port_path != primary_path || !required_endpoint) {
        return OwnershipError::primary_mismatch;
    }
    UsbFunctionSnapshot s{};
    if (paired) {
        s = sibling->snapshot();
        last_transport_error_ = s.transport_error;
        if (last_transport_error_ != 0) {
            return OwnershipError::snapshot_failed;
        }
        const bool sibling_identity_ok =
            sibling_policy == SiblingIdentityPolicy::runtime_required
                ? matches_runtime(s, profile)
                : is_permitted_sibling(s, profile);
        if (!sibling_identity_ok || s.port_path != sibling_path ||
            p.address == s.address || !correct_ordered_siblings(p, s)) {
            return OwnershipError::sibling_mismatch;
        }
    }
    if (!interface_is_safe(p) || (paired && !interface_is_safe(s))) {
        return OwnershipError::interface_unavailable;
    }

    last_transport_error_ = primary.claim_interface0();
    if (last_transport_error_ != 0) {
        return OwnershipError::primary_claim_failed;
    }
    primary_ = &primary;
    if (paired) {
        last_transport_error_ = sibling->claim_interface0();
        if (last_transport_error_ != 0) {
            (void)release();
            return OwnershipError::sibling_claim_failed;
        }
        sibling_ = sibling;
    }
    required_function_count_ = profile.expected_runtime_functions;
    primary_endpoint81_bulk_ = p.endpoint81_in_alt0 && p.endpoint81_bulk_in_alt0;
    primary_endpoint82_bulk_ = p.endpoint82_in_alt0 && p.endpoint82_bulk_in_alt0;
    sibling_endpoint81_bulk_ = s.endpoint81_in_alt0 && s.endpoint81_bulk_in_alt0;
    sibling_endpoint82_bulk_ = s.endpoint82_in_alt0 && s.endpoint82_bulk_in_alt0;
    return OwnershipError::none;
}

OwnershipError EnclosureOwnership::claim_w3u3(UsbFunctionClaim& primary, UsbFunctionClaim& sibling,
                                              const std::vector<std::uint8_t>& primary_path,
                                              const std::vector<std::uint8_t>& sibling_path)
{
    return claim_profile(*find_profile(ModelId::W3u3), primary, &sibling, primary_path,
                         sibling_path);
}

OwnershipError EnclosureOwnership::release() noexcept
{
    OwnershipError result = OwnershipError::none;
    if (sibling_ != nullptr) {
        if (sibling_->release_interface0() == 0) {
            sibling_ = nullptr;
        } else {
            result = OwnershipError::release_failed;
        }
    }
    if (primary_ != nullptr) {
        if (primary_->release_interface0() == 0) {
            primary_ = nullptr;
        } else {
            result = OwnershipError::release_failed;
        }
    }
    if (primary_ == nullptr && sibling_ == nullptr) {
        required_function_count_ = 0U;
        primary_endpoint81_bulk_ = false;
        primary_endpoint82_bulk_ = false;
        sibling_endpoint81_bulk_ = false;
        sibling_endpoint82_bulk_ = false;
    }
    return result;
}

bool EnclosureOwnership::owns_required_functions() const noexcept
{
    return primary_ != nullptr && (required_function_count_ == 1U ||
                                   (required_function_count_ == 2U && sibling_ != nullptr));
}

bool EnclosureOwnership::owns_both() const noexcept
{
    return primary_ != nullptr && sibling_ != nullptr;
}

bool EnclosureOwnership::primary_supports_bulk_endpoint(std::uint8_t endpoint) const noexcept
{
    return function_supports_bulk_endpoint(0U, endpoint);
}

bool EnclosureOwnership::function_supports_bulk_endpoint(std::uint8_t function,
                                                         std::uint8_t endpoint) const noexcept
{
    if (!owns_required_functions()) {
        return false;
    }
    const bool endpoint81 = function == 0U ? primary_endpoint81_bulk_ : sibling_endpoint81_bulk_;
    const bool endpoint82 = function == 0U ? primary_endpoint82_bulk_ : sibling_endpoint82_bulk_;
    if (function > 1U || (function == 1U && sibling_ == nullptr)) {
        return false;
    }
    if (endpoint == 0x81U) {
        return endpoint81;
    }
    if (endpoint == 0x82U) {
        return endpoint82;
    }
    return false;
}

ReceiverReservationResult ReceiverLaneReservation::reserve(std::uint8_t receiver) noexcept
{
    if (receiver > 3U) {
        return ReceiverReservationResult::invalid;
    }
    const auto mask = static_cast<std::uint8_t>(1U << receiver);
    std::uint8_t current = owners_.load(std::memory_order_acquire);
    for (;;) {
        if ((current & mask) != 0U) {
            return ReceiverReservationResult::already_owned;
        }
        if (owners_.compare_exchange_weak(current, static_cast<std::uint8_t>(current | mask),
                                          std::memory_order_acq_rel)) {
            return ReceiverReservationResult::reserved;
        }
    }
}

bool ReceiverLaneReservation::release(std::uint8_t receiver) noexcept
{
    if (receiver > 3U) {
        return false;
    }
    const auto mask = static_cast<std::uint8_t>(1U << receiver);
    std::uint8_t current = owners_.load(std::memory_order_acquire);
    for (;;) {
        if ((current & mask) == 0U) {
            return false;
        }
        if (owners_.compare_exchange_weak(current, static_cast<std::uint8_t>(current & ~mask),
                                          std::memory_order_acq_rel)) {
            return true;
        }
    }
}

bool ReceiverLaneReservation::owns(std::uint8_t receiver) const noexcept
{
    return receiver <= 3U &&
           (owners_.load(std::memory_order_acquire) & static_cast<std::uint8_t>(1U << receiver)) !=
               0U;
}

std::uint8_t ReceiverLaneReservation::owner() const noexcept
{
    const auto owners = owners_.load(std::memory_order_acquire);
    for (std::uint8_t receiver = 0U; receiver < 4U; ++receiver) {
        if ((owners & static_cast<std::uint8_t>(1U << receiver)) != 0U) {
            return receiver;
        }
    }
    return 0xffU;
}

}  // namespace asicen
