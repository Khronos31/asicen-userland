// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/hardware_ownership.h"

namespace asicen {
namespace {

bool is_runtime_primary(const UsbFunctionSnapshot& s) {
    return s.vendor_id == 0x0b06U && s.product_id == 0x0005U;
}

bool is_permitted_sibling(const UsbFunctionSnapshot& s) {
    return (s.vendor_id == 0x0b06U && s.product_id == 0x0005U) ||
           (s.vendor_id == 0x1738U &&
            (s.product_id == 0x5211U || s.product_id == 0x5216U));
}

bool interface_is_safe(const UsbFunctionSnapshot& s) {
    return s.interface0_present && s.kernel_driver_state_known &&
           !s.interface0_kernel_driver && s.active_alt0 == 0;
}

bool correct_ordered_siblings(const UsbFunctionSnapshot& p,
                              const UsbFunctionSnapshot& s) {
    if (p.bus != s.bus || p.port_path.size() < 2U ||
        p.port_path.size() != s.port_path.size() ||
        p.port_path.back() != 1U || s.port_path.back() != 2U) {
        return false;
    }
    for (std::size_t i = 0; i + 1U < p.port_path.size(); ++i) {
        if (p.port_path[i] != s.port_path[i]) return false;
    }
    return true;
}

}  // namespace

EnclosureOwnership::~EnclosureOwnership() { release(); }

OwnershipError EnclosureOwnership::claim_w3u3(
    UsbFunctionClaim& primary, UsbFunctionClaim& sibling,
    const std::vector<std::uint8_t>& primary_path,
    const std::vector<std::uint8_t>& sibling_path) {
    if (release() != OwnershipError::none) return OwnershipError::release_failed;
    if (primary_path.empty() || sibling_path.empty() ||
        primary_path == sibling_path) {
        return OwnershipError::invalid_path;
    }

    const UsbFunctionSnapshot p = primary.snapshot();
    const UsbFunctionSnapshot s = sibling.snapshot();
    if (!is_runtime_primary(p) || p.port_path != primary_path ||
        !p.endpoint82_in_alt0 || !p.endpoint82_bulk_in_alt0) {
        return OwnershipError::primary_mismatch;
    }
    if (!is_permitted_sibling(s) || s.port_path != sibling_path ||
        p.address == s.address || !correct_ordered_siblings(p, s)) {
        return OwnershipError::sibling_mismatch;
    }
    if (!interface_is_safe(p) || !interface_is_safe(s)) {
        return OwnershipError::interface_unavailable;
    }

    if (primary.claim_interface0() != 0) {
        return OwnershipError::primary_claim_failed;
    }
    primary_ = &primary;
    if (sibling.claim_interface0() != 0) {
        (void)release();
        return OwnershipError::sibling_claim_failed;
    }
    sibling_ = &sibling;
    return OwnershipError::none;
}

OwnershipError EnclosureOwnership::release() noexcept {
    OwnershipError result = OwnershipError::none;
    if (sibling_ != nullptr) {
        if (sibling_->release_interface0() == 0) sibling_ = nullptr;
        else result = OwnershipError::release_failed;
    }
    if (primary_ != nullptr) {
        if (primary_->release_interface0() == 0) primary_ = nullptr;
        else result = OwnershipError::release_failed;
    }
    return result;
}

bool EnclosureOwnership::owns_both() const noexcept {
    return primary_ != nullptr && sibling_ != nullptr;
}

}  // namespace asicen
