// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/hardware_gpio_guard.h"

#include <cstdlib>
#include <iostream>

namespace {
bool check(bool ok, const char* message)
{
    if (!ok) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

#define CHECK(...)                                                                                 \
    do {                                                                                           \
        if (!check(__VA_ARGS__)) {                                                                 \
            return false;                                                                          \
        }                                                                                          \
    } while (false)

bool retry_after_restore_takes_a_fresh_gpio_snapshot()
{
    bool valid = true;
    std::uint8_t snapshot = 0xa5U;
    unsigned reads = 0;
    // Previous startup attempt failed after power and successfully restored
    // its original value. A new attempt must snapshot that current value.
    valid = false;
    const bool captured =
        asicen::snapshot_gpio_if_needed(&valid, &snapshot, [&](std::uint8_t* value) {
            ++reads;
            *value = 0xffU;
            return true;
        });
    CHECK(captured && valid && snapshot == 0xffU && reads == 1U,
          "retry snapshots restored GPIO state before applying startup writes");

    std::uint8_t hardware_after_startup = 0x17U;
    hardware_after_startup = snapshot;  // shutdown restores the fresh snapshot
    CHECK(hardware_after_startup == 0xffU,
          "fresh snapshot remains available for final shutdown restoration");
    return true;
}

bool failed_resnapshot_does_not_create_a_valid_snapshot()
{
    bool valid = false;
    std::uint8_t snapshot = 0x3cU;
    const bool captured =
        asicen::snapshot_gpio_if_needed(&valid, &snapshot, [](std::uint8_t*) { return false; });
    CHECK(!captured && !valid && snapshot == 0x3cU,
          "failed GPIO read leaves prior snapshot storage untouched and invalid");
    return true;
}
}  // namespace

int main()
{
    if (!retry_after_restore_takes_a_fresh_gpio_snapshot()) {
        return 1;
    }
    if (!failed_resnapshot_does_not_create_a_valid_snapshot()) {
        return 1;
    }
    std::cout << "GPIO snapshot tests passed\n";
    return 0;
}
