// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <atomic>
#include <vector>

namespace asicen {

struct UsbFunctionSnapshot {
    std::uint16_t vendor_id = 0;
    std::uint16_t product_id = 0;
    std::uint8_t bus = 0;
    std::uint8_t address = 0;
    std::vector<std::uint8_t> port_path;
    bool interface0_present = false;
    bool kernel_driver_state_known = false;
    bool interface0_kernel_driver = false;
    int active_alt0 = -1;
    bool endpoint81_in_alt0 = false;
    bool endpoint81_bulk_in_alt0 = false;
    bool endpoint82_in_alt0 = false;
    bool endpoint82_bulk_in_alt0 = false;
};

// Narrow ownership surface so topology and rollback can be exercised without
// USB hardware. Implementations must not detach kernel drivers or select an
// alternate setting.
class UsbFunctionClaim {
public:
    virtual ~UsbFunctionClaim() = default;
    virtual UsbFunctionSnapshot snapshot() const = 0;
    virtual int claim_interface0() = 0;
    virtual int release_interface0() noexcept = 0;
};

enum class OwnershipError : std::uint8_t {
    none,
    invalid_path,
    primary_mismatch,
    sibling_mismatch,
    interface_unavailable,
    primary_claim_failed,
    sibling_claim_failed,
    release_failed,
};

class EnclosureOwnership final {
public:
    EnclosureOwnership() = default;
    ~EnclosureOwnership();
    EnclosureOwnership(const EnclosureOwnership&) = delete;
    EnclosureOwnership& operator=(const EnclosureOwnership&) = delete;

    OwnershipError claim_w3u3(UsbFunctionClaim& primary,
                              UsbFunctionClaim& sibling,
                              const std::vector<std::uint8_t>& primary_path,
                              const std::vector<std::uint8_t>& sibling_path);
    OwnershipError release() noexcept;
    bool owns_both() const noexcept;
    bool primary_supports_bulk_endpoint(std::uint8_t endpoint) const noexcept;

private:
    UsbFunctionClaim* primary_ = nullptr;
    UsbFunctionClaim* sibling_ = nullptr;
    bool primary_endpoint81_bulk_ = false;
};

enum class ReceiverReservationResult : std::uint8_t {
    reserved,
    already_owned,
    busy,
    invalid,
};

// The W3U3 frontend has two source lanes but a single mutable CF/DSC/link
// state. A single owner prevents interleaved receiver leases/captures.
class ReceiverLaneReservation final {
public:
    ReceiverReservationResult reserve(std::uint8_t receiver) noexcept;
    bool release(std::uint8_t receiver) noexcept;
    bool owns(std::uint8_t receiver) const noexcept;
    std::uint8_t owner() const noexcept;

private:
    static constexpr std::uint8_t kNone = 0xffU;
    std::atomic<std::uint8_t> owner_{kNone};
};

}  // namespace asicen
