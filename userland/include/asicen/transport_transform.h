// SPDX-License-Identifier: GPL-2.0-only
#ifndef ASICEN_USERLAND_TRANSPORT_TRANSFORM_H
#define ASICEN_USERLAND_TRANSPORT_TRANSFORM_H

#include <cstddef>
#include <cstdint>
#include <array>

namespace asicen {

struct TransportMaterialV7 {
    std::array<std::uint8_t, 8> first_des_key{};
    std::array<std::uint8_t, 8> second_des_key{};
    std::array<std::uint8_t, 4> xor_state{};
};

// Per-decoder prepared transform. Owns only expanded schedules and XOR state;
// clear, replacement, destruction and moves wipe key-derived storage. This is
// the same table-based DES transform as the single-packet API, not constant-time
// cryptography. No heap allocation or shared mutable cache is used.
class TransportTransformV7 final {
public:
    TransportTransformV7() noexcept = default;
    ~TransportTransformV7();
    TransportTransformV7(const TransportTransformV7&) = delete;
    TransportTransformV7& operator=(const TransportTransformV7&) = delete;
    TransportTransformV7(TransportTransformV7&& other) noexcept;
    TransportTransformV7& operator=(TransportTransformV7&& other) noexcept;

    // A failed prepare clears any prior prepared material.
    bool prepare(const std::uint8_t* first_des_key, const std::uint8_t* second_des_key,
                 const std::uint8_t* xor_state) noexcept;
    void clear() noexcept;
    bool valid() const noexcept { return valid_; }
    // Same packet, header and overlap contract as transform_transport_packet_v7.
    // Invalid input or a cleared context leaves output untouched.
    bool transform_packet(const std::uint8_t* input, std::size_t length,
                          std::uint8_t* output) const noexcept;

private:
    std::array<std::uint64_t, 16> first_subkeys_{};
    std::array<std::uint64_t, 16> second_subkeys_{};
    std::array<std::uint8_t, 4> xor_state_{};
    bool valid_ = false;
};

// Derive host-side v7 transform material from an application-selected 16-byte
// link seed. This does not perform license-table matching or device I/O.
bool derive_transport_material_v7(const std::uint8_t* link_seed, std::size_t length,
                                  TransportMaterialV7* output);

// Apply the revision-7 Key2 transport transform to exactly one 188-byte TS
// packet. The first four bytes are preserved. Input and output may be the
// same buffer; partial overlap is not supported.
bool transform_transport_packet_v7(const std::uint8_t* input, std::size_t length,
                                   const std::uint8_t* first_des_key,
                                   const std::uint8_t* second_des_key,
                                   const std::uint8_t* xor_state, std::uint8_t* output);

} // namespace asicen

#endif // ASICEN_USERLAND_TRANSPORT_TRANSFORM_H
