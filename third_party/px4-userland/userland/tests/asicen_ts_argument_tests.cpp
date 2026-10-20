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
    return parse_px4_ts_product_arguments(static_cast<int>(arguments.size()), arguments.data());
}

bool test_product_arguments()
{
    // Parsing checks the wire ID domain. Actual receiver availability comes
    // from LIST, not the former fixed four-receiver parser limit.
    for (const char* receiver : {"0", "1", "2", "3", "4", "5", "6", "7"}) {
        const auto valid = parse({"asicen-ts", "--instance", "usb1-2.1", "--receiver",
                                  receiver, "--channel", "T13", "--packet-count", "1"});
        CHECK(valid.valid && !valid.help && valid.frequency_hz == 473142857U);
    }
    for (const char* receiver : {"8", "255", "256", "-1"}) {
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
    const TsClientProfile profile{
        "asicen-ts", "asicen-userland", dtv::userland::identity::Product::asicen,
        false, true};
    print_px4_ts_usage(profile, output);
    CHECK(std::fflush(output) == 0);
    std::rewind(output);
    std::array<char, 2048U> bytes{};
    const std::size_t count = std::fread(bytes.data(), 1U, bytes.size(), output);
    CHECK(std::fclose(output) == 0);
    const std::string expected =
        "usage: asicen-ts [--device serial:...|usb:...] [--instance TOKEN] [--receiver ID] "
        "(-c|--channel CH | -f|--freq HZ -s|--system isdb-t|isdb-s) [options]\n"
        "  -c, --channel CH              (T13..T62 | 13..62 | BS<nn>[_<slot>] | CS<n>)\n"
        "  -f, --freq HZ                 (unsigned decimal Hz)\n"
        "  -s, --system isdb-t|isdb-s    (required with --freq)\n"
        "      --slot N | --tsid N       (satellite selection, exactly one)\n"
        "  -t, --time SECONDS|-          (default unlimited)\n"
        "  -o, --output PATH|-           (default stdout)\n"
        "  -d, --device serial:...|usb:... [--instance TOKEN]\n"
        "      --receiver ID --runtime-dir PATH --group\n"
        "      --packet-count N --lnb-voltage 0|15 --tune-timeout-ms N\n"
        "  -h, --help\n"
        "      --version\n";
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
