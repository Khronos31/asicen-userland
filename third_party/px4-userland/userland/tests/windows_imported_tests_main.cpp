// SPDX-License-Identifier: GPL-2.0-only
// ASICEN selected platform-independent upstream suites for Windows.
#include <cstdio>

bool run_card_tests();
bool run_card_service_tests();
bool run_ipc_tests();
bool run_ipc_state_tests();
bool run_tuner_service_tests();

int main()
{
    const struct Test final {
        const char* name;
        bool (*function)();
    } tests[] = {
        {"card_atr", run_card_tests},
        {"card_service", run_card_service_tests},
        {"ipc_wire_codec", run_ipc_tests},
        {"ipc_connection_state", run_ipc_state_tests},
        {"tuner_service", run_tuner_service_tests},
    };
    for (const Test& test : tests) {
        if (!test.function()) {
            std::fprintf(stderr, "FAIL %s\n", test.name);
            return 1;
        }
        std::printf("PASS %s\n", test.name);
    }
    return 0;
}
