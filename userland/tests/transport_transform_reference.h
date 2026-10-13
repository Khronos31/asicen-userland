// SPDX-License-Identifier: GPL-2.0-only
#ifndef ASICEN_TEST_TRANSPORT_TRANSFORM_REFERENCE_H
#define ASICEN_TEST_TRANSPORT_TRANSFORM_REFERENCE_H

#include <array>
#include <cstddef>
#include <cstdint>

// Test-only copy of the pre-optimization scalar implementation. Keep its
// tables, bit permutations and key scheduling independent of production.
namespace transport_transform_reference {

struct TransportMaterialV7 {
    std::array<std::uint8_t, 8> first_des_key{};
    std::array<std::uint8_t, 8> second_des_key{};
    std::array<std::uint8_t, 4> xor_state{};
};

bool derive_transport_material_v7(const std::uint8_t* link_seed, std::size_t length,
                                  TransportMaterialV7* output);
bool transform_transport_packet_v7(const std::uint8_t* input, std::size_t length,
                                   const std::uint8_t* first_des_key,
                                   const std::uint8_t* second_des_key,
                                   const std::uint8_t* xor_state, std::uint8_t* output);
std::uint32_t scalar_feistel(std::uint32_t right, std::uint64_t subkey);
// DES S-box output placed in its nibble and then independently P-permuted.
std::uint32_t scalar_sp(unsigned box, unsigned six);

} // namespace transport_transform_reference
#endif // ASICEN_TEST_TRANSPORT_TRANSFORM_REFERENCE_H
