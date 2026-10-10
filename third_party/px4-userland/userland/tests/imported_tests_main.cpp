// SPDX-License-Identifier: GPL-2.0-only
// ASICEN selected portable suites; hardware-only upstream tests are not linked.
#include <cstdio>

bool run_card_tests();
bool run_card_service_tests();
bool run_ipc_tests();
bool run_ipc_state_tests();
bool run_control_integration_tests();
bool run_posix_ipc_tests();
bool run_posix_tuner_nonce_tests();
bool run_control_workers_tests();
bool run_px4_ts_tests();
bool run_px4ctl_format_tests();
bool run_px4d_list_format_tests();
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
        {"control_integration", run_control_integration_tests},
        {"posix_ipc_transport", run_posix_ipc_tests},
        {"posix_tuner_nonce", run_posix_tuner_nonce_tests},
        {"control_workers", run_control_workers_tests},
        {"px4_ts", run_px4_ts_tests},
        {"px4ctl_format", run_px4ctl_format_tests},
        {"px4d_list_format", run_px4d_list_format_tests},
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
