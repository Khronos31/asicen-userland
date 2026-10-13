// SPDX-License-Identifier: GPL-2.0-only
// Compile the production implementation into this test translation unit so
// private constexpr tables/rounds can be checked without adding a runtime or
// public testing API. Do not link this executable to asicen_core.
#include "../src/transport_transform.cpp"
#include "transport_transform_reference.h"

#include <cstdio>

namespace {

bool check_round(std::uint32_t right, std::uint64_t subkey)
{
    return asicen::feistel(right, subkey) ==
           transport_transform_reference::scalar_feistel(right, subkey);
}

bool test_rounds()
{
    for (unsigned box = 0; box < 8; ++box) {
        for (unsigned six = 0; six < 64; ++six) {
            if (asicen::kSpBoxes[box][six] !=
                transport_transform_reference::scalar_sp(box, six)) {
                std::fprintf(stderr, "S/P table mismatch at box %u, entry %u\n", box, six);
                return false;
            }
            const auto subkey = static_cast<std::uint64_t>(six) << (42U - 6U * box);
            if (!check_round(0U, subkey) || !check_round(0xffffffffU, subkey) ||
                !check_round(0xaaaaaaaaU, subkey) || !check_round(0x55555555U, subkey)) {
                return false;
            }
            // Every source bit, including both E wraparound boundaries, with
            // every six-bit key group value for every S box.
            for (unsigned bit = 0; bit < 32; ++bit) {
                if (!check_round(std::uint32_t{1} << bit, subkey)) {
                    return false;
                }
            }
        }
    }
    for (unsigned bit = 0; bit < 64; ++bit) {
        if (!check_round(0x89abcdefU, std::uint64_t{1} << bit)) {
            return false;
        }
    }
    std::uint64_t state = 0x726f756e64737473ULL;
    for (unsigned sample = 0; sample < 65536; ++sample) {
        state ^= state << 13U;
        state ^= state >> 7U;
        state ^= state << 17U;
        if (!check_round(static_cast<std::uint32_t>(state), state)) {
            return false;
        }
    }
    return true;
}

} // namespace

int main()
{
    if (!test_rounds()) {
        std::fputs("DES round equivalence failed\n", stderr);
        return 1;
    }
    std::puts("all 512 S/P entries and 84032 scalar/optimized round comparisons passed");
    return 0;
}
