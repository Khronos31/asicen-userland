// SPDX-License-Identifier: GPL-2.0-only
// Windows entry point for the shared worker-lane tests.
#include <cstdio>

bool run_control_workers_tests();

int main()
{
    if (!run_control_workers_tests()) {
        return 1;
    }
    std::printf("windows worker tests: PASS\n");
    return 0;
}
