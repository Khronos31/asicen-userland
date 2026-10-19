// SPDX-License-Identifier: GPL-2.0-only
#include "px4_ts_core.h"

#include "dtv/userland/tuning.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

using px4::userland::cli::Px4TsArguments;
using px4::userland::cli::parse_px4_ts_product_arguments;

Px4TsArguments parse(std::vector<std::string> values)
{
    std::vector<const char*> argv;
    for (const auto& value : values) argv.push_back(value.c_str());
    return parse_px4_ts_product_arguments(static_cast<int>(argv.size()), argv.data());
}

bool check(bool condition, const char* message)
{
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}

}  // namespace

int main()
{
    bool ok = true;
    const auto t13 = parse({"asicen-ts", "-d", "00001205000960", "--receiver", "0",
                            "-c", "T13", "-t", "-"});
    ok &= check(t13.valid && t13.product_cli && t13.system == px4::userland::ipc::System::ISDB_T &&
                    t13.frequency_hz == 473142857U && !t13.duration_set,
                "T13 resolves to exact common Hz and unlimited time");
    const auto t27 = parse({"asicen-ts", "--device", "00001205000960", "--receiver", "0",
                            "-f", "557142857", "-s", "isdb-t", "-o", "-"});
    ok &= check(t27.valid && t27.frequency_hz == 557142857U && t27.output == "-",
                "Hz input and stdout output are retained");
    const auto bs = parse({"asicen-ts", "--device", "00001205000960", "--receiver", "0",
                           "-c", "BS15", "-s", "isdb-s", "--slot", "2"});
    ok &= check(bs.valid && bs.frequency_hz == 1318000000U && bs.slot == 2U,
                "satellite channel and slot resolve");
    const auto cs = parse({"asicen-ts", "--device", "00001205000960", "--receiver", "0",
                           "-c", "CS02", "--tsid", "42", "--time", "3"});
    ok &= check(cs.valid && cs.stream_id == 42U && cs.duration_set &&
                    cs.duration_seconds == 3U,
                "CS channel accepts TSID and finite time");

    ok &= check(!parse({"asicen-ts", "--device", "00001205000960", "--receiver", "0",
                        "--frequency-khz", "557142"}).valid,
                "old frequency alias is rejected by product parser");
    ok &= check(!parse({"asicen-ts", "--device", "00001205000960", "--receiver", "0",
                        "-f", "557142857"}).valid,
                "frequency requires system");
    ok &= check(!parse({"asicen-ts", "--device", "00001205000960", "--receiver", "0",
                        "-c", "T27", "-s", "isdb-s"}).valid,
                "channel and system must agree");
    ok &= check(!parse({"asicen-ts", "--device", "00001205000960", "--receiver", "0",
                        "-c", "BS15_2", "--slot", "2"}).valid,
                "embedded and separate satellite selections are duplicate");
    ok &= check(!parse({"asicen-ts", "--device", "00001205000960", "--receiver", "0",
                        "-c", "T27", "--tsid", "42"}).valid,
                "terrestrial channel rejects satellite selection");

    const auto pure = dtv::tuning::resolve(dtv::tuning::Request{
        std::nullopt, std::uint64_t{557142857U},
        dtv::userland::wire::System::isdb_t, std::nullopt, std::nullopt});
    ok &= check(pure && pure.value().frequency_hz == 557142857U,
                "pure shared tuning resolver preserves requested Hz");
    return ok ? 0 : 1;
}
