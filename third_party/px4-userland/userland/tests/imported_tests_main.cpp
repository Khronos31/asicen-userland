// SPDX-License-Identifier: GPL-2.0-only
#include <cstdio>

bool run_control_integration_tests();
bool run_control_workers_tests();
bool run_card_tests();
bool run_card_service_tests();
bool run_tuner_service_tests();
bool run_ipc_state_tests();
bool run_posix_ipc_tests();
bool run_posix_tuner_nonce_tests();
bool run_px4_ts_tests();
bool run_px4ctl_format_tests();
bool run_px4d_list_format_tests();

int main()
{
    const bool passed = run_control_integration_tests() &&
                        run_control_workers_tests() &&
                        run_card_tests() &&
                        run_card_service_tests() &&
                        run_tuner_service_tests() &&
                        run_ipc_state_tests() &&
                        run_posix_ipc_tests() &&
                        run_posix_tuner_nonce_tests() &&
                        run_px4_ts_tests() &&
                        run_px4ctl_format_tests() &&
                        run_px4d_list_format_tests();
    std::puts(passed ? "selected upstream px4 tests passed"
                     : "selected upstream px4 tests failed");
    return passed ? 0 : 1;
}
