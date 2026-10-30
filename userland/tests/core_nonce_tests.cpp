// SPDX-License-Identifier: GPL-2.0-only
#include "px4/posix_tuner_nonce.h"

#include <cstdio>

int main()
{
    px4::userland::ipc::posix::PosixTunerNonceSource source;
    const auto result = source.generate();
    if (!result || result.value().size() != 16U) {
        std::fputs("Core POSIX nonce length check failed\n", stderr);
        return 1;
    }
    std::puts("Core POSIX nonce length: PASS");
    return 0;
}
