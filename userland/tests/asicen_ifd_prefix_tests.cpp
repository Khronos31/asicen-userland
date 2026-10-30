// SPDX-License-Identifier: GPL-2.0-only
#include "dtv/userland/unix_pcsc_ifd/pcsc_ifd_adapter.h"

#include <cstdio>
#include <cstring>

int main() {
    using namespace dtv::userland::unix_pcsc_ifd;
    if (std::strcmp(kAsicenIfdProfile.product, "asicen-userland") != 0 ||
        std::strcmp(kAsicenIfdProfile.device_prefix, "asicen-userland:") != 0 ||
        std::strcmp(kAsicenIfdProfile.reader_name, "Khronos31") != 0 ||
        std::strcmp(kAsicenIfdProfile.reader_type,
                    "ASICEN card reader via asicend") != 0 ||
        kAsicenIfdProfile.maximum_baud != 19200U ||
        !kAsicenIfdProfile.transport_failure_means_absent ||
        !kAsicenIfdProfile.validate_status_identity ||
        kAsicenIfdProfile.leave_initial_status_unread ||
        !kAsicenIfdProfile.resource_limit_is_channel_failure ||
        kAsicenIfdProfile.invalidate_client_on_connect_failure) {
        std::fprintf(stderr, "ASICEN IFD profile drifted\n");
        return 1;
    }
    const auto product = parse_ifd_device_name(
        "asicen-userland:runtime=/tmp/asicen-ifd:instance=card-test:access=user",
        kAsicenIfdProfile);
    if (!product || product.value().device_instance != "card-test" ||
        product.value().runtime_directory != "/tmp/asicen-ifd" ||
        product.value().group_access) {
        std::fprintf(stderr, "ASICEN PC/SC device prefix did not parse\n");
        return 1;
    }
    const auto upstream = parse_ifd_device_name(
        "px4-userland:runtime=/tmp/asicen-ifd:instance=card-test",
        kAsicenIfdProfile);
    if (upstream) {
        std::fprintf(stderr, "upstream device prefix must not route to ASICEN IPC\n");
        return 1;
    }
    return 0;
}
