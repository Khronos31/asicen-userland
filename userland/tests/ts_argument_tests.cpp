// SPDX-License-Identifier: GPL-2.0-only
#include <dtv/host_ts/px4_ts_core.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace px4::userland::cli;

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            std::fprintf(stderr, "check failed at %s:%d: %s\n",             \
                         __FILE__, __LINE__, #condition);                   \
            return false;                                                   \
        }                                                                   \
    } while (0)

const TsClientProfile kProfile{
    "asicen-ts", "asicen-userland", dtv::userland::identity::Product::asicen,
    false, true, nullptr, false};

Px4TsArguments parse(std::vector<const char*> arguments)
{
    return parse_px4_ts_product_arguments(static_cast<int>(arguments.size()),
                                          arguments.data(), kProfile);
}

bool test_product_arguments()
{
    const auto valid = parse({"asicen-ts", "--device", "usb:1-2.1", "--receiver", "0",
                              "--system", "isdb-s", "--freq", "1049480000", "--tsid", "42",
                              "--time", "10"});
    CHECK(valid.valid && valid.product_cli && valid.common_profile);
    CHECK(valid.device == "usb:1-2.1" && valid.receiver == 0U);
    CHECK(valid.system == px4::userland::ipc::System::ISDB_S);
    CHECK(valid.frequency_hz == 1049480000U && valid.stream_id == 42U);
    CHECK(valid.duration_set && valid.duration_seconds == 10U && !valid.packet_count_set);

    const auto short_time = parse({"asicen-ts", "--instance", "usb1-2.1", "--receiver", "1",
                                   "--channel", "T27", "-t", "5"});
    CHECK(short_time.valid && short_time.duration_set && short_time.duration_seconds == 5U);

    const auto packet_count = parse({"asicen-ts", "--instance", "usb1-2.1", "--receiver", "1",
                                     "--channel", "T27", "--packet-count", "3"});
    CHECK(packet_count.valid && packet_count.packet_count_set && packet_count.packet_count == 3U);
    CHECK(!parse({"asicen-ts", "--instance", "usb1-2.1", "--receiver", "1",
                  "--channel", "T27", "--time", "5", "--packet-count", "3"}).valid);

    CHECK(!parse({"asicen-ts", "--instance", "usb1-2.1", "--receiver", "1",
                  "--channel", "T27", "--duration-seconds", "5"}).valid);
    CHECK(!parse({"asicen-ts", "--instance", "usb1-2.1", "--receiver", "1",
                  "--channel", "T27", "--frequency-khz", "557142"}).valid);
    CHECK(!parse({"asicen-ts", "--instance", "usb1-2.1", "--receiver", "1",
                  "--channel", "T27", "--stream-id", "42"}).valid);

    return true;
}

}  // namespace

int main()
{
    if (!test_product_arguments()) {
        std::fprintf(stderr, "FAIL product_ts_arguments\n");
        return 1;
    }
    std::printf("PASS product_ts_arguments\n");
    return 0;
}
