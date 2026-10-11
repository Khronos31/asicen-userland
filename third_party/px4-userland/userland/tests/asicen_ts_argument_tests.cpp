// SPDX-License-Identifier: GPL-2.0-only
// Product-specific identity/capacity/help boundaries supplement px4_ts_tests.
#include "px4_ts_core.h"

#include <array>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace px4::userland::cli;

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            std::fprintf(stderr, "check failed at %s:%d: %s\n",             \
                         __FILE__, __LINE__, #condition);                    \
            return false;                                                   \
        }                                                                   \
    } while (false)

Px4TsArguments parse(std::vector<const char*> arguments)
{
    return parse_px4_ts_arguments(static_cast<int>(arguments.size()), arguments.data());
}

bool test_product_arguments()
{
    for (const char* receiver : {"0", "1", "2", "3"}) {
        const auto valid = parse({"asicen-ts", "--instance", "usb1-2.1", "--receiver",
                                  receiver, "--channel", "T13", "--packet-count", "1"});
        CHECK(valid.valid && !valid.help && valid.frequency_khz == 473142U);
    }
    for (const char* receiver : {"4", "5", "7", "8", "255", "256", "-1"}) {
        CHECK(!parse({"asicen-ts", "--instance", "usb1-2.1", "--receiver",
                      receiver, "--channel", "T13"}).valid);
    }
    CHECK(!parse({"asicen-ts", "--device", "00001205000960", "--receiver", "0",
                  "--channel", "T13"}).valid);
    CHECK(!parse({"asicen-ts", "--receiver", "0", "--channel", "T13"}).valid);
    CHECK(!parse({"asicen-ts", "--instance", "../usb", "--receiver", "0",
                  "--channel", "T13"}).valid);
    CHECK(!parse({"asicen-ts", "--instance", "usb1-2.1", "--receiver", "0",
                  "--receiver", "1", "--channel", "T13"}).valid);
    CHECK(parse({"asicen-ts", "--instance", "usb1-2.1", "--receiver", "0",
                 "--channel", "BS01_0", "--lnb-voltage", "15"}).valid);
    const auto help = parse({"asicen-ts", "--help"});
    CHECK(help.valid && help.help);
    FILE* output = std::tmpfile();
    CHECK(output != nullptr);
    print_px4_ts_usage(output);
    CHECK(std::fflush(output) == 0);
    std::rewind(output);
    std::array<char, 2048U> bytes{};
    const std::size_t count = std::fread(bytes.data(), 1U, bytes.size(), output);
    CHECK(std::fclose(output) == 0);
    const std::string expected =
        "usage: asicen-ts --instance TOKEN --receiver 0..3 "
        "--system isdb-t|isdb-s --frequency-khz N [options]\n"
        "       asicen-ts --instance TOKEN --receiver 0..3 --channel CH [options]\n"
        "  --channel CH                   (T13..T62 | 13..62 | BS<nn>[_<slot>] | CS<n>)\n"
        "  --stream-id N | --slot 0..11   (isdb-s, exactly one)\n"
        "  --bandwidth-hz N               (isdb-t default 6000000)\n"
        "  --lnb-voltage 0|15             (isdb-s; 15 is daemon-dependent)\n"
        "  --tune-timeout-ms 100..30000  (default 10000)\n"
        "  --output PATH|- --duration-seconds N | --packet-count N\n"
        "  --runtime-dir PATH --group --help\n";
    CHECK(std::string(bytes.data(), count) == expected);
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
