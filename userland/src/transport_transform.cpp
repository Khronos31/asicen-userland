// SPDX-License-Identifier: GPL-2.0-only
#include "asicen/transport_transform.h"

#include <array>
#include <cstdint>
#include <cstring>

namespace asicen {
namespace {

constexpr std::uint8_t kStateOrder[8] = {0, 0, 3, 1, 0, 2, 1, 2};
constexpr std::uint8_t kKt1Mask[14] = {0x60, 0x57, 0x79, 0x88, 0,    0x00, 0x02,
                                       0x6f, 0x67, 0x40, 0,    0x00, 0xfa, 0xcd};
constexpr std::uint8_t kKt2Permutation[14] = {11, 2, 1, 13, 4, 7, 6, 5, 0, 10, 3, 12, 9, 8};
constexpr std::uint8_t kInitialPermutation[64] = {
    58, 50, 42, 34, 26, 18, 10, 2,  60, 52, 44, 36, 28, 20, 12, 4,  62, 54, 46, 38, 30, 22,
    14, 6,  64, 56, 48, 40, 32, 24, 16, 8,  57, 49, 41, 33, 25, 17, 9,  1,  59, 51, 43, 35,
    27, 19, 11, 3,  61, 53, 45, 37, 29, 21, 13, 5,  63, 55, 47, 39, 31, 23, 15, 7};
constexpr std::uint8_t kFinalPermutation[64] = {
    40, 8,  48, 16, 56, 24, 64, 32, 39, 7,  47, 15, 55, 23, 63, 31, 38, 6,  46, 14, 54, 22,
    62, 30, 37, 5,  45, 13, 53, 21, 61, 29, 36, 4,  44, 12, 52, 20, 60, 28, 35, 3,  43, 11,
    51, 19, 59, 27, 34, 2,  42, 10, 50, 18, 58, 26, 33, 1,  41, 9,  49, 17, 57, 25};
constexpr std::uint8_t kPermutation[32] = {16, 7, 20, 21, 29, 12, 28, 17, 1,  15, 23,
                                           26, 5, 18, 31, 10, 2,  8,  24, 14, 32, 27,
                                           3,  9, 19, 13, 30, 6,  22, 11, 4,  25};
constexpr std::uint8_t kPc1[56] = {57, 49, 41, 33, 25, 17, 9,  1,  58, 50, 42, 34, 26, 18,
                                   10, 2,  59, 51, 43, 35, 27, 19, 11, 3,  60, 52, 44, 36,
                                   63, 55, 47, 39, 31, 23, 15, 7,  62, 54, 46, 38, 30, 22,
                                   14, 6,  61, 53, 45, 37, 29, 21, 13, 5,  28, 20, 12, 4};
constexpr std::uint8_t kPc2[48] = {14, 17, 11, 24, 1,  5,  3,  28, 15, 6,  21, 10, 23, 19, 12, 4,
                                   26, 8,  16, 7,  27, 20, 13, 2,  41, 52, 31, 37, 47, 55, 30, 40,
                                   51, 45, 33, 48, 44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32};
constexpr std::uint8_t kRotations[16] = {1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1};
constexpr std::uint8_t kSBoxes[8][64] = {
    {14, 4,  13, 1, 2,  15, 11, 8, 3, 10, 6, 12, 5,  9,  0,  7,  0,  15, 7,  4,  14, 2,
     13, 1,  10, 6, 12, 11, 9,  5, 3, 8,  4, 1,  14, 8,  13, 6,  2,  11, 15, 12, 9,  7,
     3,  10, 5,  0, 15, 12, 8,  2, 4, 9,  1, 7,  5,  11, 3,  14, 10, 0,  6,  13},
    {15, 1,  8,  14, 6,  11, 3,  4, 9,  7,  2, 13, 12, 0,  5,  10, 3,  13, 4,  7, 15, 2,
     8,  14, 12, 0,  1,  10, 6,  9, 11, 5,  0, 14, 7,  11, 10, 4,  13, 1,  5,  8, 12, 6,
     9,  3,  2,  15, 13, 8,  10, 1, 3,  15, 4, 2,  11, 6,  7,  12, 0,  5,  14, 9},
    {10, 0,  9,  14, 6, 3,  15, 5,  1,  13, 12, 7, 11, 4,  2,  8,  13, 7, 0,  9, 3, 4,
     6,  10, 2,  8,  5, 14, 12, 11, 15, 1,  13, 6, 4,  9,  8,  15, 3,  0, 11, 1, 2, 12,
     5,  10, 14, 7,  1, 10, 13, 0,  6,  9,  8,  7, 4,  15, 14, 3,  11, 5, 2,  12},
    {7, 13, 14, 3, 0, 6,  9, 10, 1,  2, 8,  5, 11, 12, 4,  15, 13, 8,  11, 5, 6, 15,
     0, 3,  4,  7, 2, 12, 1, 10, 14, 9, 10, 6, 9,  0,  12, 11, 7,  13, 15, 1, 3, 14,
     5, 2,  8,  4, 3, 15, 0, 6,  10, 1, 13, 8, 9,  4,  5,  11, 12, 7,  2,  14},
    {2,  12, 4, 1,  7,  10, 11, 6, 8, 5,  3, 15, 13, 0,  14, 9,  14, 11, 2,  12, 4,  7,
     13, 1,  5, 0,  15, 10, 3,  9, 8, 6,  4, 2,  1,  11, 10, 13, 7,  8,  15, 9,  12, 5,
     6,  3,  0, 14, 11, 8,  12, 7, 1, 14, 2, 13, 6,  15, 0,  9,  10, 4,  5,  3},
    {12, 1,  10, 15, 9,  2,  6, 8,  0, 13, 3,  4,  14, 7,  5, 11, 10, 15, 4, 2, 7, 12,
     9,  5,  6,  1,  13, 14, 0, 11, 3, 8,  9,  14, 15, 5,  2, 8,  12, 3,  7, 0, 4, 10,
     1,  13, 11, 6,  4,  3,  2, 12, 9, 5,  15, 10, 11, 14, 1, 7,  6,  0,  8, 13},
    {4, 11, 2,  14, 15, 0,  8,  13, 3, 12, 9,  7, 5,  10, 6,  1,  13, 0,  11, 7,  4, 9,
     1, 10, 14, 3,  5,  12, 2,  15, 8, 6,  1,  4, 11, 13, 12, 3,  7,  14, 10, 15, 6, 8,
     0, 5,  9,  2,  6,  11, 13, 8,  1, 4,  10, 7, 9,  5,  0,  15, 14, 2,  3,  12},
    {13, 2, 8,  4, 6, 15, 11, 1,  10, 9,  3, 14, 5,  0,  12, 7,  1,  15, 13, 8, 10, 3,
     7,  4, 12, 5, 6, 11, 0,  14, 9,  2,  7, 11, 4,  1,  9,  12, 14, 2,  0,  6, 10, 13,
     15, 3, 5,  8, 2, 1,  14, 7,  4,  10, 8, 13, 15, 12, 9,  0,  3,  5,  6,  11}};

template <std::size_t N>
constexpr std::uint64_t permute(std::uint64_t input, const std::uint8_t (&table)[N], unsigned input_bits)
{
    std::uint64_t output = 0;
    for (std::size_t i = 0; i < N; ++i) {
        output = (output << 1U) | ((input >> (input_bits - table[i])) & 1U);
    }
    return output;
}

using DesDecryptKey = std::array<std::uint64_t, 16>;

// Each six-bit index includes the S-box row/column mapping; each value already
// occupies its final P-permuted bit positions. These eight contributions are
// disjoint. Generate from the DES definitions instead of opaque magic tables.
constexpr std::array<std::array<std::uint32_t, 64>, 8> make_sp_boxes()
{
    std::array<std::array<std::uint32_t, 64>, 8> result{};
    for (unsigned box = 0; box < 8; ++box) {
        for (unsigned six = 0; six < 64; ++six) {
            const unsigned row = ((six & 0x20U) >> 4U) | (six & 1U);
            const unsigned column = (six >> 1U) & 0x0fU;
            const std::uint32_t substituted =
                static_cast<std::uint32_t>(kSBoxes[box][row * 16U + column]) << (28U - 4U * box);
            result[box][six] = static_cast<std::uint32_t>(permute(substituted, kPermutation, 32));
        }
    }
    return result;
}

constexpr auto kSpBoxes = make_sp_boxes();
static_assert(sizeof(kSpBoxes) == 2048, "DES S/P tables must remain compact");

template <typename T>
void wipe(T& value) noexcept
{
    volatile std::uint8_t* bytes = reinterpret_cast<volatile std::uint8_t*>(&value);
    for (std::size_t i = 0; i < sizeof(value); ++i) {
        bytes[i] = 0U;
    }
}

void make_decrypt_key(const std::uint8_t* key, DesDecryptKey& result)
{
    std::uint64_t key_bits = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        key_bits = (key_bits << 8U) | key[i];
    }
    const std::uint64_t selected = permute(key_bits, kPc1, 64);
    std::uint32_t left = static_cast<std::uint32_t>((selected >> 28U) & 0x0fffffffU);
    std::uint32_t right = static_cast<std::uint32_t>(selected & 0x0fffffffU);
    for (std::size_t round = 0; round < result.size(); ++round) {
        const unsigned shift = kRotations[round];
        left = ((left << shift) | (left >> (28U - shift))) & 0x0fffffffU;
        right = ((right << shift) | (right >> (28U - shift))) & 0x0fffffffU;
        result[round] =
            permute((static_cast<std::uint64_t>(left) << 28U) | right, kPc2, 56);
    }
}

std::uint32_t feistel(std::uint32_t right, std::uint64_t subkey)
{
    // DES E expansion as eight fixed six-bit windows; the first and last wrap
    // at bit 32. Keep the original 48-bit schedule order and decrypt rounds.
    const auto first = ((right & 1U) << 5U) | (right >> 27U);
    const auto last = ((right << 1U) | (right >> 31U)) & 0x3fU;
    return kSpBoxes[0][(first ^ (subkey >> 42U)) & 0x3fU] ^
           kSpBoxes[1][((right >> 23U) ^ (subkey >> 36U)) & 0x3fU] ^
           kSpBoxes[2][((right >> 19U) ^ (subkey >> 30U)) & 0x3fU] ^
           kSpBoxes[3][((right >> 15U) ^ (subkey >> 24U)) & 0x3fU] ^
           kSpBoxes[4][((right >> 11U) ^ (subkey >> 18U)) & 0x3fU] ^
           kSpBoxes[5][((right >> 7U) ^ (subkey >> 12U)) & 0x3fU] ^
           kSpBoxes[6][((right >> 3U) ^ (subkey >> 6U)) & 0x3fU] ^
           kSpBoxes[7][(last ^ subkey) & 0x3fU];
}

void des_decrypt_block(const std::uint8_t* input, const DesDecryptKey& key, std::uint8_t* output)
{
    std::uint64_t block = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        block = (block << 8U) | input[i];
    }
    const std::uint64_t initial = permute(block, kInitialPermutation, 64);
    std::uint32_t left = static_cast<std::uint32_t>(initial >> 32U);
    std::uint32_t right = static_cast<std::uint32_t>(initial);
    for (int round = 15; round >= 0; --round) {
        const std::uint32_t next_left = right;
        const std::uint32_t next_right =
            left ^ feistel(right, key[static_cast<std::size_t>(round)]);
        left = next_left;
        right = next_right;
    }
    const std::uint64_t final =
        permute((static_cast<std::uint64_t>(right) << 32U) | left, kFinalPermutation, 64);
    for (int i = 7; i >= 0; --i) {
        output[i] = static_cast<std::uint8_t>(final >> (8U * (7 - i)));
    }
}

void copy_and_xor_packet(const std::uint8_t* input, const std::uint8_t* xor_state,
                         std::uint8_t* output)
{
    if (input != output) {
        std::memcpy(output, input, 188);
    }
    for (std::size_t offset = 4; offset < 188; ++offset) {
        output[offset] ^= xor_state[kStateOrder[(offset - 4) % 8]];
    }
}

void decrypt_packet(const DesDecryptKey& first_key, const DesDecryptKey& second_key,
                     std::uint8_t* output)
{
    for (std::size_t offset = 4; offset < 132; offset += 8) {
        std::array<std::uint8_t, 8> block{};
        std::memcpy(block.data(), output + offset, block.size());
        des_decrypt_block(block.data(), first_key, output + offset);
    }
    for (std::size_t offset = 132; offset < 188; offset += 8) {
        std::array<std::uint8_t, 8> block{};
        std::memcpy(block.data(), output + offset, block.size());
        des_decrypt_block(block.data(), second_key, output + offset);
    }
}

} // namespace

TransportTransformV7::~TransportTransformV7()
{
    clear();
}

TransportTransformV7::TransportTransformV7(TransportTransformV7&& other) noexcept
    : first_subkeys_(other.first_subkeys_), second_subkeys_(other.second_subkeys_),
      xor_state_(other.xor_state_), valid_(other.valid_)
{
    other.clear();
}

TransportTransformV7& TransportTransformV7::operator=(TransportTransformV7&& other) noexcept
{
    if (this != &other) {
        clear();
        first_subkeys_ = other.first_subkeys_;
        second_subkeys_ = other.second_subkeys_;
        xor_state_ = other.xor_state_;
        valid_ = other.valid_;
        other.clear();
    }
    return *this;
}

bool TransportTransformV7::prepare(const std::uint8_t* first_des_key,
                                   const std::uint8_t* second_des_key,
                                   const std::uint8_t* xor_state) noexcept
{
    clear();
    if (first_des_key == nullptr || second_des_key == nullptr || xor_state == nullptr) {
        return false;
    }
    make_decrypt_key(first_des_key, first_subkeys_);
    make_decrypt_key(second_des_key, second_subkeys_);
    std::memcpy(xor_state_.data(), xor_state, xor_state_.size());
    valid_ = true;
    return true;
}

void TransportTransformV7::clear() noexcept
{
    wipe(first_subkeys_);
    wipe(second_subkeys_);
    wipe(xor_state_);
    valid_ = false;
}

bool TransportTransformV7::transform_packet(const std::uint8_t* input, std::size_t length,
                                           std::uint8_t* output) const noexcept
{
    if (!valid_ || input == nullptr || output == nullptr || length != 188) {
        return false;
    }
    copy_and_xor_packet(input, xor_state_.data(), output);
    decrypt_packet(first_subkeys_, second_subkeys_, output);
    return true;
}

bool transform_transport_packet_v7(const std::uint8_t* input, std::size_t length,
                                   const std::uint8_t* first_des_key,
                                   const std::uint8_t* second_des_key,
                                   const std::uint8_t* xor_state, std::uint8_t* output)
{
    if (input == nullptr || first_des_key == nullptr || second_des_key == nullptr ||
        xor_state == nullptr || output == nullptr || length != 188) {
        return false;
    }
    // Preserve the existing wrapper's copy/XOR/key-read order as well as its
    // public interface. Streaming decoders use a prepared context instead.
    copy_and_xor_packet(input, xor_state, output);
    DesDecryptKey first_key{};
    DesDecryptKey second_key{};
    make_decrypt_key(first_des_key, first_key);
    make_decrypt_key(second_des_key, second_key);
    decrypt_packet(first_key, second_key, output);
    wipe(first_key);
    wipe(second_key);
    return true;
}

bool derive_transport_material_v7(const std::uint8_t* link_seed, std::size_t length,
                                  TransportMaterialV7* output)
{
    if (link_seed == nullptr || output == nullptr || length != 16)
        return false;
    TransportMaterialV7 derived{};

    std::array<std::uint8_t, 112> bits{};
    for (std::size_t i = 0; i < 16; ++i) {
        for (std::size_t bit = 0; bit < 7; ++bit) {
            bits[i * 7 + bit] = static_cast<std::uint8_t>((link_seed[i] >> (bit + 1)) & 1U);
        }
    }
    std::array<std::uint8_t, 14> packed{};
    for (std::size_t i = 0; i < 7; ++i) {
        for (std::size_t bit = 0; bit < 8; ++bit) {
            packed[7 + i] |= static_cast<std::uint8_t>(bits[i * 8 + bit] << bit);
            packed[i] |= static_cast<std::uint8_t>(bits[56 + i * 8 + bit] << bit);
        }
    }

    std::uint8_t low_bits_a = 0;
    std::uint8_t low_bits_z = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        low_bits_a |= static_cast<std::uint8_t>((link_seed[i] & 1U) << i);
        low_bits_z |= static_cast<std::uint8_t>((link_seed[8 + i] & 1U) << i);
    }
    std::array<std::uint8_t, 14> mixed{};
    for (std::size_t i = 0; i < mixed.size(); ++i) {
        const std::uint8_t mask = i == 4 ? low_bits_a : (i == 10 ? low_bits_z : kKt1Mask[i]);
        mixed[i] = static_cast<std::uint8_t>(packed[i] ^ mask);
    }

    std::array<std::uint8_t, 14> kt1{};
    for (std::size_t i = 0; i < kt1.size(); ++i) {
        kt1[i] =
            static_cast<std::uint8_t>((mixed[(i + 6) % 14] >> 3U) | (mixed[(i + 7) % 14] << 5U));
    }

    std::array<std::uint8_t, 14> kt2_mask = {
        low_bits_z, low_bits_a, low_bits_a, low_bits_z, 0xcf,       low_bits_z, 0x02,
        low_bits_a, low_bits_z, low_bits_z, low_bits_a, low_bits_z, low_bits_a, low_bits_z};
    std::array<std::uint8_t, 14> transformed{};
    for (std::size_t i = 0; i < transformed.size(); ++i) {
        transformed[i] = static_cast<std::uint8_t>(kt1[kKt2Permutation[i]] ^ kt2_mask[i]);
    }

    derived.xor_state[0] =
        static_cast<std::uint8_t>((transformed[8] << 4U) | (transformed[9] >> 4U));
    derived.xor_state[1] =
        static_cast<std::uint8_t>((transformed[9] << 4U) | (transformed[12] >> 4U));
    derived.xor_state[2] =
        static_cast<std::uint8_t>((transformed[1] & 0xf0U) | (transformed[3] >> 4U));
    derived.xor_state[3] =
        static_cast<std::uint8_t>((transformed[6] & 0xf0U) | (transformed[0] & 0x0fU));

    std::array<std::uint8_t, 112> key_bits{};
    for (std::size_t i = 0; i < transformed.size(); ++i) {
        for (std::size_t bit = 0; bit < 8; ++bit) {
            key_bits[i * 8 + bit] = static_cast<std::uint8_t>((transformed[i] >> bit) & 1U);
        }
    }
    for (std::size_t bit = 0; bit < 56; ++bit) {
        derived.first_des_key[7 - bit / 7] |=
            static_cast<std::uint8_t>(key_bits[bit] << (bit % 7 + 1));
        derived.second_des_key[7 - bit / 7] |=
            static_cast<std::uint8_t>(key_bits[56 + bit] << (bit % 7 + 1));
    }
    *output = derived;
    return true;
}

} // namespace asicen
