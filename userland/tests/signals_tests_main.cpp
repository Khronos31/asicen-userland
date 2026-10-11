// SPDX-License-Identifier: GPL-2.0-only
bool run_px4d_signal_tests();

int main()
{
    return run_px4d_signal_tests() ? 0 : 1;
}
