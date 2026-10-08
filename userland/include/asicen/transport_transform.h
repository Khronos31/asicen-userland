// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <array>

namespace asicen {

struct TransportMaterialV7 {
    std::array<std::uint8_t, 8> first_des_key{};
    std::array<std::uint8_t, 8> second_des_key{};
    std::array<std::uint8_t, 4> xor_state{};
};

// Derive host-side v7 transform material from an application-selected 16-byte
// link seed. This does not perform license-table matching or device I/O.
bool derive_transport_material_v7(const std::uint8_t* link_seed,
                                  std::size_t length,
                                  TransportMaterialV7* output);

// Apply the revision-7 Key2 transport transform to exactly one 188-byte TS
// packet. The first four bytes are preserved. Input and output may be the
// same buffer; partial overlap is not supported.
bool transform_transport_packet_v7(const std::uint8_t* input,
                                   std::size_t length,
                                   const std::uint8_t* first_des_key,
                                   const std::uint8_t* second_des_key,
                                   const std::uint8_t* xor_state,
                                   std::uint8_t* output);

}  // namespace asicen
