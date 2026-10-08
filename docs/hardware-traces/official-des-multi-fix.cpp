// SPDX-License-Identifier: GPL-2.0-only
// Link with -Wl,--wrap=des_crypt_ecb_Multi for the isolated reference harness.
// This file is not part of the product and is never linked into asicen code.
extern "C" void des_crypt_ecb(unsigned long* schedule, unsigned char* input,
                              unsigned char* output, void* tables);

extern "C" int __wrap_des_crypt_ecb_Multi(unsigned long* schedule,
                                           unsigned char* input,
                                           unsigned char* output,
                                           void* tables, int block_count) {
    if (schedule == nullptr || input == nullptr || output == nullptr ||
        tables == nullptr || block_count <= 0) {
        return -1;
    }
    for (int block = 0; block < block_count; ++block) {
        des_crypt_ecb(schedule, input + block * 8,
                      output + block * 8, tables);
    }
    return 0;
}
