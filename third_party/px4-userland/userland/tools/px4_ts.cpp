// SPDX-License-Identifier: GPL-2.0-only
#include "px4_ts_core.h"
#include "px4_ts_posix.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <thread>
#include <unistd.h>

namespace {

using namespace px4::userland;
using namespace px4::userland::cli;

volatile std::sig_atomic_t stop_flag = 0;

void stop_handler(int) noexcept
{
    stop_flag = 1;
}

class PosixClock final : public Px4TsClock {
public:
    std::uint64_t monotonic_ms() noexcept override
    {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    void sleep_ms(std::uint32_t milliseconds) noexcept override
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
    }
};

class PosixSignal final : public Px4TsSignal {
public:
    bool stop_requested() const noexcept override { return stop_flag != 0; }
};

bool install_signals() noexcept
{
    struct sigaction ignore_pipe {};
    ignore_pipe.sa_handler = SIG_IGN;
    if (sigemptyset(&ignore_pipe.sa_mask) != 0 ||
        ::sigaction(SIGPIPE, &ignore_pipe, nullptr) != 0) return false;
    struct sigaction action {};
    action.sa_handler = stop_handler;
    if (sigemptyset(&action.sa_mask) != 0) return false;
    return ::sigaction(SIGINT, &action, nullptr) == 0 &&
           ::sigaction(SIGTERM, &action, nullptr) == 0;
}

void print_counters(const char* label, const ipc::CountersPayload& counters) noexcept
{
    std::fprintf(stderr,
                 "%s packets=%llu bytes=%llu sync-errors=%llu tei=%llu "
                 "continuity-errors=%llu queue-drops=%llu usb-errors=%llu "
                 "empty-intervals=%llu\n",
                 label,
                 static_cast<unsigned long long>(counters.packets),
                 static_cast<unsigned long long>(counters.bytes),
                 static_cast<unsigned long long>(counters.sync_errors),
                 static_cast<unsigned long long>(counters.tei_packets),
                 static_cast<unsigned long long>(counters.continuity_errors),
                 static_cast<unsigned long long>(counters.queue_drops),
                 static_cast<unsigned long long>(counters.usb_errors),
                 static_cast<unsigned long long>(counters.empty_intervals));
}

}  // namespace

int main(int argc, char** argv)
{
    const Px4TsArguments arguments =
        parse_px4_ts_arguments(argc, const_cast<const char* const*>(argv));
    if (!arguments.valid) {
        std::fprintf(stderr, "argument error: %s\n", arguments.error.c_str());
        print_px4_ts_usage(stderr);
        return 2;
    }
    if (arguments.help) {
        print_px4_ts_usage(stdout);
        return 0;
    }
    if (!install_signals()) {
        std::fprintf(stderr, "signal setup failed\n");
        return 70;
    }
    const bool stdout_output = arguments.output == "-";
    int fd = STDOUT_FILENO;
    if (!stdout_output) {
        fd = ::open(arguments.output.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
        if (fd < 0) {
            std::fprintf(stderr, "output: %s\n", std::strerror(errno));
            return 70;
        }
    }
    Px4TsFdOutput output(fd, !stdout_output);
    if (!output.ready()) {
        std::fprintf(stderr, "output setup failed\n");
        return 70;
    }
    PosixClock clock;
    PosixSignal signal;
    Px4TsFailureKind failure_kind = Px4TsFailureKind::ipc;
    Px4TsRunDiagnostics diagnostics;
    const auto result = Px4TsRunner::run(arguments, output, clock, signal,
                                         &failure_kind, &diagnostics);
    if (!result) {
        std::fprintf(stderr, "px4-ts: %s\n", error_string(result.error()));
        std::fprintf(stderr, "written packets=%llu bytes=%llu\n",
                     static_cast<unsigned long long>(diagnostics.packets_written),
                     static_cast<unsigned long long>(diagnostics.packets_written * 188U));
        if (diagnostics.stop_counters_valid)
            print_counters("STOP_STREAM", diagnostics.stop_counters);
        if (diagnostics.stream_end_valid) {
            print_counters("STREAM_END", diagnostics.stream_end.counters);
            std::fprintf(stderr, "STREAM_END error=%u\n",
                         static_cast<unsigned int>(diagnostics.stream_end.error_code));
        }
        return px4_ts_exit_status(result.error(), failure_kind);
    }
    print_counters("stream", result.value().counters);
    return 0;
}
