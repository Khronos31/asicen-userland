// SPDX-License-Identifier: GPL-2.0-only
#include "asicen/transport_transform.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace {

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #condition);           \
            return false;                                                                          \
        }                                                                                          \
    } while (false)

bool expect(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
    return condition;
}

bool test_fips_vector_and_packet_ranges()
{
    constexpr std::array<std::uint8_t, 8> key = {0x13, 0x34, 0x57, 0x79, 0x9b, 0xbc, 0xdf, 0xf1};
    constexpr std::array<std::uint8_t, 8> ciphertext = {0x85, 0xe8, 0x13, 0x54,
                                                        0x0f, 0x0a, 0xb4, 0x05};
    constexpr std::array<std::uint8_t, 8> plaintext = {0x01, 0x23, 0x45, 0x67,
                                                       0x89, 0xab, 0xcd, 0xef};
    constexpr std::array<std::uint8_t, 8> second_key = {};
    constexpr std::array<std::uint8_t, 8> zero_ciphertext = {0x8c, 0xa6, 0x4d, 0xe9,
                                                             0xc1, 0xb1, 0x23, 0xa7};
    constexpr std::array<std::uint8_t, 4> state = {0x12, 0x34, 0x56, 0x78};
    constexpr std::array<std::uint8_t, 8> state_order = {0, 0, 3, 1, 0, 2, 1, 2};

    std::array<std::uint8_t, 188> input{};
    input[0] = 0xc7;
    input[1] = 0x42;
    input[2] = 0x31;
    input[3] = 0x9a;
    for (std::size_t block = 0; block < 23; ++block) {
        for (std::size_t i = 0; i < 8; ++i) {
            const std::size_t payload_index = block * 8 + i;
            const auto cipher_byte = block < 16 ? ciphertext[i] : zero_ciphertext[i];
            input[4 + payload_index] = cipher_byte ^ state[state_order[payload_index % 8]];
        }
    }

    std::array<std::uint8_t, 188> output{};
    CHECK(expect(asicen::transform_transport_packet_v7(input.data(), input.size(), key.data(),
                                                       second_key.data(), state.data(),
                                                       output.data()),
                 "accepts one complete packet"));
    CHECK(expect(output[0] == input[0] && output[1] == input[1] && output[2] == input[2] &&
                     output[3] == input[3],
                 "preserves header"));
    bool payload_matches = true;
    for (std::size_t block = 0; block < 23; ++block) {
        for (std::size_t i = 0; i < 8; ++i) {
            payload_matches &= output[4 + block * 8 + i] == (block < 16 ? plaintext[i] : 0);
        }
    }
    CHECK(expect(payload_matches,
                 "decrypts 16 FIPS blocks then 7 zero-key blocks at the 128-byte boundary"));
    return true;
}

bool test_seed_material_api()
{
    std::array<std::uint8_t, 16> seed{};
    for (std::size_t i = 0; i < seed.size(); ++i) {
        seed[i] = static_cast<std::uint8_t>(i * 17U + 3U);
    }
    asicen::TransportMaterialV7 material;
    CHECK(expect(asicen::derive_transport_material_v7(seed.data(), seed.size(), &material),
                 "derives transform material from a 16-byte caller seed"));
    constexpr std::array<std::uint8_t, 8> expected_first_key = {0xe6, 0xf0, 0xe0, 0x6c,
                                                                0x90, 0xc6, 0x24, 0x56};
    constexpr std::array<std::uint8_t, 8> expected_second_key = {0x50, 0xe4, 0xca, 0xf2,
                                                                 0x12, 0x24, 0x54, 0x72};
    constexpr std::array<std::uint8_t, 4> expected_xor_state = {0x52, 0x4c, 0xc6, 0xeb};
    CHECK(expect(material.first_des_key == expected_first_key &&
                     material.second_des_key == expected_second_key &&
                     material.xor_state == expected_xor_state,
                 "matches fixed synthetic seed derivation vector"));
    auto repeated = material;
    repeated.first_des_key.fill(0xff);
    repeated.second_des_key.fill(0xff);
    repeated.xor_state.fill(0xff);
    CHECK(expect(asicen::derive_transport_material_v7(seed.data(), seed.size(), &repeated) &&
                     repeated.first_des_key == material.first_des_key &&
                     repeated.second_des_key == material.second_des_key &&
                     repeated.xor_state == material.xor_state,
                 "overwrites prior output and is stable for the same seed"));
    CHECK(expect(!asicen::derive_transport_material_v7(seed.data(), seed.size() - 1, &material),
                 "rejects a short link seed"));
    CHECK(expect(!asicen::derive_transport_material_v7(seed.data(), seed.size(), nullptr),
                 "rejects a null material output"));
    CHECK(expect(!asicen::derive_transport_material_v7(nullptr, seed.size(), &material),
                 "rejects a null link seed"));
    return true;
}

bool test_in_place_and_validation()
{
    constexpr std::array<std::uint8_t, 8> key = {0x13, 0x34, 0x57, 0x79, 0x9b, 0xbc, 0xdf, 0xf1};
    constexpr std::array<std::uint8_t, 4> state = {1, 2, 3, 4};
    std::array<std::uint8_t, 188> packet{};
    packet.fill(0xa5);
    const auto original = packet;
    CHECK(expect(asicen::transform_transport_packet_v7(packet.data(), packet.size(), key.data(),
                                                       key.data(), state.data(), packet.data()),
                 "supports in-place transformation"));
    CHECK(expect(packet[0] == original[0] && packet[1] == original[1] && packet[2] == original[2] &&
                     packet[3] == original[3],
                 "in-place path preserves header"));

    std::array<std::uint8_t, 187> short_packet{};
    std::array<std::uint8_t, 188> output{};
    CHECK(expect(!asicen::transform_transport_packet_v7(short_packet.data(), short_packet.size(),
                                                        key.data(), key.data(), state.data(),
                                                        output.data()),
                 "rejects partial packet length"));
    CHECK(expect(!asicen::transform_transport_packet_v7(nullptr, 188, key.data(), key.data(),
                                                        state.data(), output.data()),
                 "rejects null input"));
    CHECK(expect(!asicen::transform_transport_packet_v7(output.data(), 188, nullptr, key.data(),
                                                        state.data(), output.data()),
                 "rejects null first key"));
    CHECK(expect(!asicen::transform_transport_packet_v7(output.data(), 188, key.data(), nullptr,
                                                        state.data(), output.data()),
                 "rejects null second key"));
    CHECK(expect(!asicen::transform_transport_packet_v7(output.data(), 188, key.data(), key.data(),
                                                        nullptr, output.data()),
                 "rejects null xor state"));
    CHECK(expect(!asicen::transform_transport_packet_v7(output.data(), 188, key.data(), key.data(),
                                                        state.data(), nullptr),
                 "rejects null output"));
    return true;
}

} // namespace

bool test_all()
{
    CHECK(test_fips_vector_and_packet_ranges());
    CHECK(test_in_place_and_validation());
    CHECK(test_seed_material_api());
    std::puts("transport transform tests passed");
    return true;
}

int main()
{
    return test_all() ? 0 : 1;
}
