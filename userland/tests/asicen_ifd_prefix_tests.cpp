// SPDX-License-Identifier: GPL-2.0-only
#include "px4/pcsc_ifd_adapter.h"

#include <cstdio>

int main() {
    const auto product = px4::userland::pcsc::parse_ifd_device_name(
        "asicen-userland:runtime=/tmp/asicen-ifd:instance=card-test:access=user");
    if (!product || product.value().device_instance != "card-test" ||
        product.value().runtime_directory != "/tmp/asicen-ifd" ||
        product.value().group_access) {
        std::fprintf(stderr, "ASICEN PC/SC device prefix did not parse\n");
        return 1;
    }
    const auto upstream = px4::userland::pcsc::parse_ifd_device_name(
        "px4-userland:runtime=/tmp/asicen-ifd:instance=card-test");
    if (upstream) {
        std::fprintf(stderr, "upstream device prefix must not route to ASICEN IPC\n");
        return 1;
    }
    return 0;
}
