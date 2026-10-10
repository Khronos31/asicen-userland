// SPDX-License-Identifier: GPL-2.0-only
// Link with -Wl,--wrap=des_crypt_ecb_Multi for the isolated reference harness.
// This file is not part of the product and is never linked into asicen code.
#include <cstddef>
#include <limits>

extern "C" void des_crypt_ecb(unsigned long* schedule, unsigned char* input, unsigned char* output,
                              void* tables);

extern "C" int __wrap_des_crypt_ecb_Multi(unsigned long* schedule, unsigned char* input,
                                          unsigned char* output, void* tables, int block_count)
{
    if (schedule == nullptr || input == nullptr || output == nullptr || tables == nullptr ||
        block_count <= 0 ||
        static_cast<std::size_t>(block_count) >
            static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()) / 8U) {
        return -1;
    }
    for (int block = 0; block < block_count; ++block) {
        const std::size_t offset = static_cast<std::size_t>(block) * 8U;
        des_crypt_ecb(schedule, input + offset, output + offset, tables);
    }
    return 0;
}
