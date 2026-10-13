// SPDX-License-Identifier: GPL-2.0-only
#include "asicen/transport_transform.h"
#include "transport_transform_reference.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <type_traits>
#include <utility>

namespace {

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            std::fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #condition);  \
            return false;                                                                 \
        }                                                                                 \
    } while (false)

constexpr std::size_t kPacketSize = 188;
using Packet = std::array<std::uint8_t, kPacketSize>;
using Key = std::array<std::uint8_t, 8>;
using State = std::array<std::uint8_t, 4>;

static_assert(!std::is_copy_constructible<asicen::TransportTransformV7>::value,
              "prepared transform contexts must not copy key schedules");
static_assert(!std::is_copy_assignable<asicen::TransportTransformV7>::value,
              "prepared transform contexts must not copy key schedules");
static_assert(std::is_nothrow_move_constructible<asicen::TransportTransformV7>::value,
              "prepared transform contexts must support noexcept ownership transfer");
static_assert(std::is_nothrow_move_assignable<asicen::TransportTransformV7>::value,
              "prepared transform contexts must support noexcept ownership transfer");

// Fixed local generator: every input is synthetic and reproducible.
std::uint32_t next_random(std::uint32_t& state)
{
    state ^= state << 13U;
    state ^= state >> 17U;
    state ^= state << 5U;
    return state;
}

template <std::size_t N>
void fill_random(std::array<std::uint8_t, N>& bytes, std::uint32_t& state)
{
    for (auto& byte : bytes) {
        byte = static_cast<std::uint8_t>(next_random(state));
    }
}

Packet make_packet(std::size_t pattern, std::uint32_t& random)
{
    Packet packet{};
    switch (pattern) {
    case 0:
        packet.fill(0);
        break;
    case 1:
        packet.fill(0xff);
        break;
    case 2:
        for (std::size_t i = 0; i < packet.size(); ++i) {
            packet[i] = static_cast<std::uint8_t>(i);
        }
        break;
    case 3:
        for (std::size_t i = 0; i < packet.size(); ++i) {
            packet[i] = (i & 1U) == 0 ? 0x55 : 0xaa;
        }
        break;
    case 4:
        for (std::size_t i = 0; i < packet.size(); ++i) {
            packet[i] = static_cast<std::uint8_t>(1U << (i % 8));
        }
        break;
    case 5:
        // Distinguish the header, the two key ranges, and the final byte.
        for (const std::size_t offset : {0U, 3U, 4U, 127U, 128U, 131U, 132U, 183U, 187U}) {
            packet[offset] = static_cast<std::uint8_t>(next_random(random));
        }
        break;
    default:
        fill_random(packet, random);
        break;
    }
    // Deliberately include arbitrary headers, not only valid TS headers.
    for (std::size_t i = 0; i < 4; ++i) {
        packet[i] = static_cast<std::uint8_t>(next_random(random));
    }
    return packet;
}

bool check_packet(const Packet& packet, const Key& first, const Key& second,
                  const State& state)
{
    Packet expected{};
    CHECK(transport_transform_reference::transform_transport_packet_v7(
        packet.data(), packet.size(), first.data(), second.data(), state.data(), expected.data()));
    auto reference_in_place = packet;
    CHECK(transport_transform_reference::transform_transport_packet_v7(
        reference_in_place.data(), reference_in_place.size(), first.data(), second.data(),
        state.data(), reference_in_place.data()));
    CHECK(reference_in_place == expected);
    CHECK(std::equal(packet.begin(), packet.begin() + 4, expected.begin()));

    // Both addresses are deliberately unaligned; guard bytes detect writes
    // before or after the exact packet. Nonoverlapping input must not mutate.
    std::array<std::uint8_t, kPacketSize + 34> input{};
    std::array<std::uint8_t, kPacketSize + 34> output{};
    input.fill(0x39);
    output.fill(0xa6);
    std::copy(packet.begin(), packet.end(), input.begin() + 1);
    const auto original_input = input;
    CHECK(asicen::transform_transport_packet_v7(input.data() + 1, kPacketSize, first.data(),
                                                second.data(), state.data(), output.data() + 17));
    CHECK(input == original_input);
    CHECK(std::equal(expected.begin(), expected.end(), output.begin() + 17));
    CHECK(std::all_of(output.begin(), output.begin() + 17,
                      [](std::uint8_t byte) { return byte == 0xa6; }));
    CHECK(std::all_of(output.begin() + 17 + kPacketSize, output.end(),
                      [](std::uint8_t byte) { return byte == 0xa6; }));

    CHECK(asicen::transform_transport_packet_v7(input.data() + 1, kPacketSize, first.data(),
                                                second.data(), state.data(), input.data() + 1));
    CHECK(std::equal(expected.begin(), expected.end(), input.begin() + 1));
    CHECK(input.front() == 0x39);
    CHECK(std::all_of(input.begin() + 1 + kPacketSize, input.end(),
                      [](std::uint8_t byte) { return byte == 0x39; }));

    asicen::TransportTransformV7 prepared;
    CHECK(prepared.prepare(first.data(), second.data(), state.data()));
    CHECK(prepared.valid());
    input = original_input;
    output.fill(0xa6);
    CHECK(prepared.transform_packet(input.data() + 1, kPacketSize, output.data() + 17));
    CHECK(input == original_input);
    CHECK(std::equal(expected.begin(), expected.end(), output.begin() + 17));
    CHECK(std::all_of(output.begin(), output.begin() + 17,
                      [](std::uint8_t byte) { return byte == 0xa6; }));
    CHECK(std::all_of(output.begin() + 17 + kPacketSize, output.end(),
                      [](std::uint8_t byte) { return byte == 0xa6; }));
    CHECK(prepared.transform_packet(input.data() + 1, kPacketSize, input.data() + 1));
    CHECK(std::equal(expected.begin(), expected.end(), input.begin() + 1));
    CHECK(input.front() == 0x39);
    CHECK(std::all_of(input.begin() + 1 + kPacketSize, input.end(),
                      [](std::uint8_t byte) { return byte == 0x39; }));
    return true;
}

bool test_known_answer()
{
    constexpr Key first = {0x13, 0x34, 0x57, 0x79, 0x9b, 0xbc, 0xdf, 0xf1};
    constexpr Key second{};
    constexpr Key ciphertext = {0x85, 0xe8, 0x13, 0x54, 0x0f, 0x0a, 0xb4, 0x05};
    constexpr Key zero_ciphertext = {0x8c, 0xa6, 0x4d, 0xe9, 0xc1, 0xb1, 0x23, 0xa7};
    constexpr Key plaintext = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
    constexpr State state = {0x12, 0x34, 0x56, 0x78};
    constexpr std::array<std::uint8_t, 8> order = {0, 0, 3, 1, 0, 2, 1, 2};
    Packet packet{};
    Packet expected{};
    for (std::size_t i = 0; i < 4; ++i) {
        packet[i] = expected[i] = static_cast<std::uint8_t>(0xc7U + i * 9U);
    }
    for (std::size_t i = 4; i < kPacketSize; ++i) {
        const auto index = (i - 4) % 8;
        packet[i] = static_cast<std::uint8_t>(
            (i < 132 ? ciphertext[index] : zero_ciphertext[index]) ^ state[order[index]]);
        expected[i] = i < 132 ? plaintext[index] : 0;
    }
    Packet actual{};
    CHECK(transport_transform_reference::transform_transport_packet_v7(
        packet.data(), packet.size(), first.data(), second.data(), state.data(), actual.data()));
    CHECK(actual == expected);
    CHECK(check_packet(packet, first, second, state));
    return true;
}

bool test_seed_differential()
{
    std::uint32_t random = 0x7b35a92dU;
    for (std::size_t seed_index = 0; seed_index < 64; ++seed_index) {
        std::array<std::uint8_t, 16> seed{};
        fill_random(seed, random);
        if (seed_index == 0) {
            seed.fill(0);
        } else if (seed_index == 1) {
            seed.fill(0xff);
        }
        transport_transform_reference::TransportMaterialV7 reference;
        asicen::TransportMaterialV7 actual;
        CHECK(transport_transform_reference::derive_transport_material_v7(
            seed.data(), seed.size(), &reference));
        CHECK(asicen::derive_transport_material_v7(seed.data(), seed.size(), &actual));
        CHECK(actual.first_des_key == reference.first_des_key);
        CHECK(actual.second_des_key == reference.second_des_key);
        CHECK(actual.xor_state == reference.xor_state);
        for (std::size_t pattern = 0; pattern < 348; ++pattern) {
            CHECK(check_packet(make_packet(pattern % 12, random), actual.first_des_key,
                               actual.second_des_key, actual.xor_state));
        }
    }
    return true;
}

bool test_arbitrary_keys()
{
    std::uint32_t random = 0x82e2a741U;
    for (std::size_t index = 0; index < 128; ++index) {
        Key first{};
        Key second{};
        State state{};
        fill_random(first, random);
        fill_random(second, random);
        fill_random(state, random);
        if (index == 0 || index == 1) {
            first.fill(index == 0 ? 0 : 0xff);
            second = first;
            state.fill(index == 0 ? 0 : 0xff);
        }
        const auto packet = make_packet(index % 12, random);
        CHECK(check_packet(packet, first, second, state));
        // DES parity bits must not affect output, including arbitrary keys
        // that did not come from the transport-material derivation.
        Packet original{};
        Packet toggled{};
        CHECK(asicen::transform_transport_packet_v7(packet.data(), packet.size(), first.data(),
                                                    second.data(), state.data(), original.data()));
        for (auto& byte : first) {
            byte ^= 1U;
        }
        for (auto& byte : second) {
            byte ^= 1U;
        }
        CHECK(asicen::transform_transport_packet_v7(packet.data(), packet.size(), first.data(),
                                                    second.data(), state.data(), toggled.data()));
        CHECK(original == toggled);
        CHECK(check_packet(packet, first, second, state));
    }
    return true;
}

bool test_invalid_input_no_mutation()
{
    const Key first{};
    const Key second{};
    const State state{};
    Packet packet{};
    packet.fill(0x6d);
    Packet output{};
    output.fill(0xa7);
    const auto original_packet = packet;
    const auto original_output = output;
    for (const auto length : std::array<std::size_t, 12>{
             0, 1, 4, 127, 128, 131, 132, 187, 189, 376, 65536,
             std::numeric_limits<std::size_t>::max()}) {
        CHECK(!asicen::transform_transport_packet_v7(packet.data(), length, first.data(),
                                                     second.data(), state.data(), output.data()));
        CHECK(output == original_output);
        CHECK(packet == original_packet);
        CHECK(!asicen::transform_transport_packet_v7(packet.data(), length, first.data(),
                                                     second.data(), state.data(), packet.data()));
        CHECK(packet == original_packet);
    }
    for (unsigned missing = 0; missing < 5; ++missing) {
        CHECK(!asicen::transform_transport_packet_v7(
            missing == 0 ? nullptr : packet.data(), kPacketSize,
            missing == 1 ? nullptr : first.data(), missing == 2 ? nullptr : second.data(),
            missing == 3 ? nullptr : state.data(), missing == 4 ? nullptr : output.data()));
        CHECK(output == original_output);
        CHECK(packet == original_packet);
        CHECK(!asicen::transform_transport_packet_v7(
            missing == 0 ? nullptr : packet.data(), kPacketSize,
            missing == 1 ? nullptr : first.data(), missing == 2 ? nullptr : second.data(),
            missing == 3 ? nullptr : state.data(), missing == 4 ? nullptr : packet.data()));
        CHECK(packet == original_packet);
    }

    std::array<std::uint8_t, 16> seed{};
    asicen::TransportMaterialV7 material;
    material.first_des_key.fill(0x63);
    material.second_des_key.fill(0x87);
    material.xor_state.fill(0xda);
    const auto original_material = material;
    for (const auto length : std::array<std::size_t, 5>{
             0, 15, 17, 32, std::numeric_limits<std::size_t>::max()}) {
        CHECK(!asicen::derive_transport_material_v7(seed.data(), length, &material));
        CHECK(material.first_des_key == original_material.first_des_key);
        CHECK(material.second_des_key == original_material.second_des_key);
        CHECK(material.xor_state == original_material.xor_state);
    }
    CHECK(!asicen::derive_transport_material_v7(nullptr, seed.size(), &material));
    CHECK(material.first_des_key == original_material.first_des_key);
    CHECK(material.second_des_key == original_material.second_des_key);
    CHECK(material.xor_state == original_material.xor_state);
    CHECK(!asicen::derive_transport_material_v7(seed.data(), seed.size(), nullptr));
    return true;
}

bool test_context_lifecycle()
{
    std::uint32_t random = 0x51743e29U;
    Key first{};
    Key second{};
    State state{};
    fill_random(first, random);
    fill_random(second, random);
    fill_random(state, random);
    const auto packet = make_packet(11, random);
    Packet expected{};
    CHECK(transport_transform_reference::transform_transport_packet_v7(
        packet.data(), packet.size(), first.data(), second.data(), state.data(), expected.data()));
    Packet output{};
    output.fill(0xaa);
    const auto sentinel = output;
    asicen::TransportTransformV7 context;
    CHECK(!context.valid());
    CHECK(!context.transform_packet(packet.data(), packet.size(), output.data()));
    CHECK(output == sentinel);
    CHECK(context.prepare(first.data(), second.data(), state.data()));
    CHECK(context.valid());

    for (const auto length : std::array<std::size_t, 7>{
             0, 4, 132, 187, 189, 376, std::numeric_limits<std::size_t>::max()}) {
        CHECK(!context.transform_packet(packet.data(), length, output.data()));
        CHECK(output == sentinel);
        auto in_place = packet;
        CHECK(!context.transform_packet(in_place.data(), length, in_place.data()));
        CHECK(in_place == packet);
        CHECK(context.valid());
    }
    CHECK(!context.transform_packet(nullptr, packet.size(), output.data()));
    CHECK(output == sentinel);
    CHECK(!context.transform_packet(packet.data(), packet.size(), nullptr));
    CHECK(context.valid());

    // Preparation must own the state; subsequent caller changes are unrelated.
    first.fill(0x23);
    second.fill(0xba);
    state.fill(0x76);
    CHECK(context.transform_packet(packet.data(), packet.size(), output.data()));
    CHECK(output == expected);
    asicen::TransportTransformV7 moved(std::move(context));
    CHECK(moved.valid());
    CHECK(!context.valid());
    output = sentinel;
    CHECK(!context.transform_packet(packet.data(), packet.size(), output.data()));
    CHECK(output == sentinel);
    CHECK(moved.transform_packet(packet.data(), packet.size(), output.data()));
    CHECK(output == expected);

    CHECK(context.prepare(first.data(), second.data(), state.data()));
    context = std::move(moved);
    CHECK(context.valid());
    CHECK(!moved.valid());
    output = sentinel;
    CHECK(context.transform_packet(packet.data(), packet.size(), output.data()));
    CHECK(output == expected);
    // An alias prevents compiler self-move warnings while checking the contract.
    auto* alias = &context;
    context = std::move(*alias);
    CHECK(context.valid());
    output = sentinel;
    CHECK(context.transform_packet(packet.data(), packet.size(), output.data()));
    CHECK(output == expected);

    for (unsigned missing = 0; missing < 3; ++missing) {
        CHECK(context.prepare(first.data(), second.data(), state.data()));
        CHECK(!context.prepare(missing == 0 ? nullptr : first.data(),
                               missing == 1 ? nullptr : second.data(),
                               missing == 2 ? nullptr : state.data()));
        CHECK(!context.valid());
        output = sentinel;
        CHECK(!context.transform_packet(packet.data(), packet.size(), output.data()));
        CHECK(output == sentinel);
    }
    CHECK(context.prepare(first.data(), second.data(), state.data()));
    CHECK(transport_transform_reference::transform_transport_packet_v7(
        packet.data(), packet.size(), first.data(), second.data(), state.data(), expected.data()));
    CHECK(context.transform_packet(packet.data(), packet.size(), output.data()));
    CHECK(output == expected);
    context.clear();
    context.clear();
    CHECK(!context.valid());
    output = sentinel;
    CHECK(!context.transform_packet(packet.data(), packet.size(), output.data()));
    CHECK(output == sentinel);
    moved = std::move(context);
    CHECK(!moved.valid());
    CHECK(!context.valid());
    return true;
}

bool test_all()
{
    CHECK(test_known_answer());
    CHECK(test_seed_differential());
    CHECK(test_arbitrary_keys());
    CHECK(test_invalid_input_no_mutation());
    CHECK(test_context_lifecycle());
    std::puts("transport transform differential tests passed (64 seeds x 348 packets, "
              "128 arbitrary key/parity cases; synthetic input only)");
    return true;
}

} // namespace

int main()
{
    return test_all() ? 0 : 1;
}
