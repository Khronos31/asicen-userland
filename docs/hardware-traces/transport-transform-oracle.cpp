// SPDX-License-Identifier: GPL-2.0-only
#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

#include "asicen/transport_transform.h"

extern "C" int official_decryp_ts(void*, unsigned char*,
                                  unsigned char*) asm("_Z12DTV_DecrypTSP13_STnimControlPhS1_");
extern "C" void official_add_usb_device(void*) asm("_Z16DTV_AddUSBDeviceP17_DEVICE_EXTENSION");
extern "C" int official_kt1(void*) asm("_Z23DTV_5606B2_KeyTransfer1P13_STnimControl");
extern "C" int official_kt2(void*) asm("_Z23DTV_5606B2_KeyTransfer2P13_STnimControl");
extern "C" int official_des_setkey_dec(unsigned long*, unsigned char*, void*) asm("des_setkey_dec");
extern "C" int official_des_setkey_enc(unsigned long*, unsigned char*, void*) asm("des_setkey_enc");
extern "C" int official_des_crypt_one(unsigned long*, unsigned char*, unsigned char*,
                                      void*) asm("des_crypt_ecb");
extern "C" int __real_des_crypt_ecb_Multi(unsigned long*, unsigned char*, unsigned char*, void*,
                                          int);
extern "C" int __wrap_des_crypt_ecb_Multi(unsigned long* schedule, unsigned char* input,
                                          unsigned char* output, void* tables, int block_count)
{
    if (schedule == nullptr || input == nullptr || output == nullptr || tables == nullptr ||
        block_count <= 0 ||
        static_cast<std::size_t>(block_count) >
            static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()) / 8U) {
        return -1;
    }
    for (int i = 0; i < block_count; ++i) {
        const std::size_t offset = static_cast<std::size_t>(i) * 8U;
        official_des_crypt_one(schedule, input + offset, output + offset, tables);
    }
    return 0;
}

namespace {
constexpr std::size_t kControlSize = 0x91000;
constexpr std::size_t kTablesOffset = 0x3668;
constexpr std::size_t kScheduleSize = 512;

bool put_pointer(std::vector<std::uint8_t>& bytes, std::size_t offset, const void* pointer)
{
    if (offset > bytes.size() || bytes.size() - offset < sizeof(pointer)) {
        return false;
    }
    std::memcpy(bytes.data() + offset, &pointer, sizeof(pointer));
    return true;
}

std::uint8_t reverse_bits(std::uint8_t value)
{
    std::uint8_t result = 0;
    for (unsigned i = 0; i < 8; ++i) {
        result = static_cast<std::uint8_t>((result << 1U) | (value & 1U));
        value = static_cast<std::uint8_t>(value >> 1U);
    }
    return result;
}

void orient(const std::array<std::uint8_t, 8>& source, unsigned mode,
            std::array<std::uint8_t, 8>& destination)
{
    for (std::size_t i = 0; i < source.size(); ++i) {
        const auto value = (mode & 2U) != 0 ? reverse_bits(source[i]) : source[i];
        destination[(mode & 1U) != 0 ? source.size() - 1 - i : i] = value;
    }
}

bool compare_case(unsigned seed)
{
    std::vector<std::uint8_t> extension(0x5000, 0);
    // DTV_AddUSBDevice has no call instructions and fills the DES tables at
    // extension+0x3668. It is used here only as the official oracle setup.
    official_add_usb_device(extension.data());

    std::vector<std::uint8_t> control(kControlSize, 0);
    std::array<unsigned long, kScheduleSize / sizeof(unsigned long)> key_a_schedule{};
    std::array<unsigned long, kScheduleSize / sizeof(unsigned long)> key_b_schedule{};
    std::array<std::uint8_t, 8> key_a{};
    std::array<std::uint8_t, 8> key_b{};
    std::array<std::uint8_t, 4> xor_state{};
    for (std::size_t i = 0; i < key_a.size(); ++i) {
        key_a[i] = static_cast<std::uint8_t>(seed * 17U + i * 29U + 3U);
        key_b[i] = static_cast<std::uint8_t>(seed * 31U + i * 11U + 7U);
    }
    for (std::size_t i = 0; i < xor_state.size(); ++i) {
        xor_state[i] = static_cast<std::uint8_t>(seed * 13U + i * 47U + 9U);
    }
    official_des_setkey_dec(key_a_schedule.data(), key_a.data(), extension.data() + kTablesOffset);
    official_des_setkey_dec(key_b_schedule.data(), key_b.data(), extension.data() + kTablesOffset);

    std::array<std::uint8_t, 188> input{};
    for (std::size_t i = 0; i < input.size(); ++i) {
        input[i] = static_cast<std::uint8_t>(seed * 37U + i * 19U + (i >> 2U));
    }
    const auto original_input = input;

    // The archive's exported multi-block function has an x86-64 stack-read
    // defect, so synthesize the source-proven packet loop from its stable
    // official single-block DES primitive for a deterministic oracle.
    auto official_output = original_input;
    constexpr std::uint8_t state_order[8] = {0, 0, 3, 1, 0, 2, 1, 2};
    for (std::size_t offset = 4; offset < official_output.size(); ++offset) {
        official_output[offset] ^= xor_state[state_order[(offset - 4) % 8]];
    }
    std::array<std::uint8_t, 8> block{}, transformed{};
    auto* tables = extension.data() + kTablesOffset;
    for (std::size_t offset = 4; offset < 132; offset += 8) {
        std::memcpy(block.data(), official_output.data() + offset, block.size());
        official_des_crypt_one(key_a_schedule.data(), block.data(), transformed.data(), tables);
        std::memcpy(official_output.data() + offset, transformed.data(), transformed.size());
    }
    for (std::size_t offset = 132; offset < 188; offset += 8) {
        std::memcpy(block.data(), official_output.data() + offset, block.size());
        official_des_crypt_one(key_b_schedule.data(), block.data(), transformed.data(), tables);
        std::memcpy(official_output.data() + offset, transformed.data(), transformed.size());
    }

    // Linker wrapping replaces only DTV_DecrypTS's defective multi-block
    // primitive with one call per block to the archive's stable DES routine.
    std::array<std::uint8_t, 188> vendor_multi_output{};
    auto vendor_input = original_input;
    if (!put_pointer(control, 0x1d38, extension.data()) ||
        !put_pointer(control, 0x90f50, key_a_schedule.data()) ||
        !put_pointer(control, 0x90f58, key_b_schedule.data())) {
        return false;
    }
    control[0x30de4] = 1;
    control[0x30da1] = 1;
    control[0x8f475] = 7;
    std::memcpy(control.data() + 0x90f60, xor_state.data(), xor_state.size());
    const int transform_result =
        official_decryp_ts(control.data(), vendor_input.data(), vendor_multi_output.data());
    std::array<std::uint8_t, 188> portable{};
    const bool portable_result = asicen::transform_transport_packet_v7(
        original_input.data(), original_input.size(), key_a.data(), key_b.data(), xor_state.data(),
        portable.data());
    const bool matches = transform_result == 1 && portable_result && portable == official_output &&
                         portable == vendor_multi_output;
    std::printf("case=%u wrapped-DTV_DecrypTS=%s bytes=188\n", seed,
                matches ? "match" : "MISMATCH");
    if (!matches) {
        std::size_t count = 0;
        std::size_t first = official_output.size();
        for (std::size_t i = 0; i < official_output.size(); ++i) {
            if (portable[i] != official_output[i] || portable[i] != vendor_multi_output[i]) {
                ++count;
                if (first == official_output.size())
                    first = i;
            }
        }
        std::fprintf(stderr, "first_difference=%zu differing_bytes=%zu\n", first, count);
    }
    return matches;
}

bool compare_seed_case(const std::array<std::uint8_t, 16>& seed,
                       std::vector<std::uint8_t>& extension, std::vector<std::uint8_t>& control)
{
    std::fill(control.begin(), control.end(), 0);
    std::array<unsigned long, kScheduleSize / sizeof(unsigned long)> official_a{};
    std::array<unsigned long, kScheduleSize / sizeof(unsigned long)> official_b{};
    std::array<unsigned long, kScheduleSize / sizeof(unsigned long)> derived_a{};
    std::array<unsigned long, kScheduleSize / sizeof(unsigned long)> derived_b{};
    if (!put_pointer(control, 0x1d38, extension.data()) ||
        !put_pointer(control, 0x90f50, official_a.data()) ||
        !put_pointer(control, 0x90f58, official_b.data())) {
        return false;
    }
    std::memcpy(control.data() + 0x30da4, seed.data(), seed.size());

    if (official_kt1(control.data()) != 1 || official_kt2(control.data()) != 1) {
        std::puts("official KT call rejected a synthetic seed");
        return false;
    }
    asicen::TransportMaterialV7 material;
    if (!asicen::derive_transport_material_v7(seed.data(), seed.size(), &material)) {
        std::puts("portable seed derivation rejected a complete seed");
        return false;
    }
    if (!std::equal(material.xor_state.begin(), material.xor_state.end(),
                    control.begin() + 0x90f60)) {
        std::puts("portable XOR state differs from official KT2");
        return false;
    }

    auto* tables = extension.data() + kTablesOffset;
    official_des_setkey_dec(derived_a.data(), material.first_des_key.data(), tables);
    official_des_setkey_dec(derived_b.data(), material.second_des_key.data(), tables);
    for (unsigned sample = 0; sample < 8; ++sample) {
        std::array<std::uint8_t, 8> input{}, official_output{}, derived_output{};
        for (std::size_t i = 0; i < input.size(); ++i) {
            input[i] = static_cast<std::uint8_t>(seed[i] + sample * 23U + i * 41U);
        }
        official_des_crypt_one(official_a.data(), input.data(), official_output.data(), tables);
        official_des_crypt_one(derived_a.data(), input.data(), derived_output.data(), tables);
        if (official_output != derived_output) {
            std::puts("first derived DES key schedule differs from official KT2");
            return false;
        }
        official_des_crypt_one(official_b.data(), input.data(), official_output.data(), tables);
        official_des_crypt_one(derived_b.data(), input.data(), derived_output.data(), tables);
        if (official_output != derived_output) {
            std::puts("second derived DES key schedule differs from official KT2");
            return false;
        }
    }
    return true;
}

bool test_seed_derivation()
{
    std::vector<std::uint8_t> extension(0x5000, 0);
    std::vector<std::uint8_t> control(kControlSize, 0);
    official_add_usb_device(extension.data());
    std::array<std::uint8_t, 16> fixed_seed{};
    for (std::size_t i = 0; i < fixed_seed.size(); ++i) {
        fixed_seed[i] = static_cast<std::uint8_t>(i * 17U + 3U);
    }
    if (!compare_seed_case(fixed_seed, extension, control))
        return false;
    asicen::TransportMaterialV7 fixed_material;
    constexpr std::array<std::uint8_t, 8> expected_first = {0xe6, 0xf0, 0xe0, 0x6c,
                                                            0x90, 0xc6, 0x24, 0x56};
    constexpr std::array<std::uint8_t, 8> expected_second = {0x50, 0xe4, 0xca, 0xf2,
                                                             0x12, 0x24, 0x54, 0x72};
    constexpr std::array<std::uint8_t, 4> expected_state = {0x52, 0x4c, 0xc6, 0xeb};
    if (!asicen::derive_transport_material_v7(fixed_seed.data(), fixed_seed.size(),
                                              &fixed_material) ||
        fixed_material.first_des_key != expected_first ||
        fixed_material.second_des_key != expected_second ||
        fixed_material.xor_state != expected_state) {
        std::puts("fixed synthetic seed vector differs");
        return false;
    }
    std::size_t cases = 0;
    for (unsigned sample = 0; sample < 64; ++sample) {
        std::array<std::uint8_t, 16> seed{};
        for (std::size_t i = 0; i < seed.size(); ++i) {
            seed[i] = static_cast<std::uint8_t>(sample * 53U + i * 29U + (i >> 1U));
        }
        if (!compare_seed_case(seed, extension, control))
            return false;
        ++cases;
    }
    for (unsigned bit = 0; bit < 128; ++bit) {
        std::array<std::uint8_t, 16> seed{};
        seed[bit / 8] = static_cast<std::uint8_t>(1U << (bit % 8));
        if (!compare_seed_case(seed, extension, control))
            return false;
        ++cases;
    }
    std::printf("seed derivation oracle: fixed vector plus %zu synthetic seeds matched\n", cases);
    return true;
}
} // namespace

int main()
{
    constexpr std::array<std::uint8_t, 8> reference_key = {0x13, 0x34, 0x57, 0x79,
                                                           0x9b, 0xbc, 0xdf, 0xf1};
    constexpr std::array<std::uint8_t, 8> reference_input = {0x85, 0xe8, 0x13, 0x54,
                                                             0x0f, 0x0a, 0xb4, 0x05};
    constexpr std::array<std::uint8_t, 8> reference_output = {0x01, 0x23, 0x45, 0x67,
                                                              0x89, 0xab, 0xcd, 0xef};
    bool found_vector_layout = false;
    for (unsigned key_mode = 0; key_mode < 4; ++key_mode) {
        for (unsigned input_mode = 0; input_mode < 4; ++input_mode) {
            for (unsigned output_mode = 0; output_mode < 4; ++output_mode) {
                std::vector<std::uint8_t> extension(0x5000, 0);
                official_add_usb_device(extension.data());
                std::array<unsigned long, kScheduleSize / sizeof(unsigned long)> schedule{};
                std::array<std::uint8_t, 8> key{}, input{}, output{}, normalized{};
                orient(reference_key, key_mode, key);
                orient(reference_input, input_mode, input);
                official_des_setkey_dec(schedule.data(), key.data(),
                                        extension.data() + kTablesOffset);
                official_des_crypt_one(schedule.data(), input.data(), output.data(),
                                       extension.data() + kTablesOffset);
                orient(output, output_mode, normalized);
                if (normalized == reference_output) {
                    std::printf("official DES FIPS vector matched orientation %u/%u/%u\n", key_mode,
                                input_mode, output_mode);
                    found_vector_layout = true;
                }
            }
        }
    }
    if (!found_vector_layout) {
        std::puts("official DES FIPS orientation unresolved");
    }
    {
        std::vector<std::uint8_t> extension(0x5000, 0);
        official_add_usb_device(extension.data());
        std::array<unsigned long, kScheduleSize / sizeof(unsigned long)> enc_schedule{};
        std::array<unsigned long, kScheduleSize / sizeof(unsigned long)> dec_schedule{};
        auto key = reference_key;
        auto input = reference_output;
        std::array<std::uint8_t, 8> encrypted{}, decrypted{}, fips_decrypted{};
        std::array<std::uint8_t, 8> multi_encrypted{};
        std::array<std::uint8_t, 8> multi_repeated{};
        auto* tables = extension.data() + kTablesOffset;
        official_des_setkey_enc(enc_schedule.data(), key.data(), tables);
        official_des_crypt_one(enc_schedule.data(), input.data(), encrypted.data(), tables);
        __real_des_crypt_ecb_Multi(enc_schedule.data(), input.data(), multi_encrypted.data(),
                                   tables, 1);
        __real_des_crypt_ecb_Multi(enc_schedule.data(), input.data(), multi_repeated.data(), tables,
                                   1);
        official_des_setkey_dec(dec_schedule.data(), key.data(), tables);
        official_des_crypt_one(dec_schedule.data(), encrypted.data(), decrypted.data(), tables);
        official_des_crypt_one(dec_schedule.data(),
                               const_cast<std::uint8_t*>(reference_input.data()),
                               fips_decrypted.data(), tables);
        std::printf(
            "official DES inverse self-test=%s, FIPS ciphertext=%s, multi/single=%s, repeat=%s\n",
            decrypted == input ? "passed" : "FAILED",
            fips_decrypted == reference_output ? "match" : "different",
            encrypted == multi_encrypted ? "same" : "different",
            multi_encrypted == multi_repeated ? "same" : "different");
    }
    for (unsigned seed = 0; seed < 32; ++seed) {
        if (!compare_case(seed + 1)) {
            return 1;
        }
    }
    if (!test_seed_derivation())
        return 1;
    const bool output_ok = std::fflush(stdout) == 0;
    const bool error_ok = std::fflush(stderr) == 0;
    return output_ok && error_ok ? 0 : 1;
}
