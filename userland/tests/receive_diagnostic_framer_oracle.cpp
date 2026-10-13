// SPDX-License-Identifier: GPL-2.0-only
// Test-only oracle for the keyless Python receive diagnostic analyzer.
#include "asicen/ts_framer.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
px4::userland::Result<void> accept(void*, px4::userland::ByteView) noexcept
{
    return px4::userland::Result<void>::success();
}
}  // namespace

int main(int argc, char** argv)
{
    asicen::TsFramer framer(asicen::TransportMode::AsicenMode7);
    for (int index = 1; index < argc; ++index) {
        errno = 0;
        char* end = nullptr;
        const unsigned long amount = std::strtoul(argv[index], &end, 10);
        if (errno != 0 || end == argv[index] || *end != '\0' || amount > 1048576UL) {
            return 2;
        }
        std::vector<std::uint8_t> input(static_cast<std::size_t>(amount));
        if (std::fread(input.data(), 1U, input.size(), stdin) != input.size()) {
            return 2;
        }
        if (!framer.push({input.data(), input.size()}, accept, nullptr)) {
            return 1;
        }
        const auto counters = framer.counters();
        std::printf("%llu,%llu,%llu,%llu,%llu\n",
                    static_cast<unsigned long long>(counters.input_bytes_accepted),
                    static_cast<unsigned long long>(counters.emitted_packets),
                    static_cast<unsigned long long>(counters.discarded_sync_search_bytes),
                    static_cast<unsigned long long>(counters.sync_loss_events),
                    static_cast<unsigned long long>(counters.buffered_bytes));
    }
    return std::fgetc(stdin) == EOF && !std::ferror(stdin) ? 0 : 2;
}
