// SPDX-License-Identifier: GPL-2.0-only
// Windows px4-ts front end. The binary sink lives in px4_ts_windows_output so
// it can be exercised by isolated tests; the portable protocol runner in
// px4_ts_core.cpp is shared with the POSIX front end.
#include "px4_ts_core.h"
#include "px4_ts_windows_output.h"
#include "px4_windows_args.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace px4::userland;
using namespace px4::userland::cli;

std::atomic<bool> stop_flag{false};
HANDLE cleanup_event = nullptr;

BOOL WINAPI stop_handler(DWORD control_type) noexcept
{
    switch (control_type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        stop_flag.store(true, std::memory_order_release);
        if (cleanup_event != nullptr) {
            (void)::WaitForSingleObject(cleanup_event, 5000U);
        }
        return TRUE;
    default:
        return FALSE;
    }
}

class WindowsClock final : public Px4TsClock {
public:
    std::uint64_t monotonic_ms() noexcept override
    {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    void sleep_ms(std::uint32_t milliseconds) noexcept override
    {
        ::Sleep(milliseconds);
    }
};

class WindowsSignal final : public Px4TsSignal {
public:
    bool stop_requested() const noexcept override
    {
        return stop_flag.load(std::memory_order_acquire);
    }
};

bool install_handlers() noexcept
{
    cleanup_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (cleanup_event == nullptr) {
        return false;
    }
    return ::SetConsoleCtrlHandler(stop_handler, TRUE) != 0;
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
    const std::vector<std::string> owned =
        px4::userland::cli::windows_argv_utf8(argc, argv);
    std::vector<const char*> views;
    views.reserve(owned.size());
    for (const std::string& value : owned) {
        views.push_back(value.c_str());
    }
    const Px4TsArguments arguments =
        parse_px4_ts_arguments(static_cast<int>(views.size()), views.data());
    if (!arguments.valid) {
        std::fprintf(stderr, "argument error: %s\n", arguments.error.c_str());
        print_px4_ts_usage(stderr);
        return 2;
    }
    if (arguments.help) {
        print_px4_ts_usage(stdout);
        return 0;
    }
    if (!install_handlers()) {
        std::fprintf(stderr, "signal setup failed\n");
        return 70;
    }
    const bool stdout_output = arguments.output == "-";
    HANDLE handle = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "output: stdout is not available\n");
        return 70;
    }
    if (!stdout_output) {
        const std::vector<wchar_t> wide =
            px4::userland::cli::utf8_to_wide_path(arguments.output);
        if (wide.empty()) {
            std::fprintf(stderr, "output: invalid path\n");
            return 70;
        }
        handle = ::CreateFileW(wide.data(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            std::fprintf(stderr, "output: error %lu\n",
                         static_cast<unsigned long>(::GetLastError()));
            return 70;
        }
    }
    auto sink_result = Px4TsWindowsOutput::create(handle, !stdout_output);
    if (!sink_result) {
        std::fprintf(stderr, "output setup failed\n");
        return 70;
    }
    std::unique_ptr<Px4TsWindowsOutput> output = std::move(sink_result.value());
    WindowsClock clock;
    WindowsSignal signal;
    Px4TsFailureKind failure_kind = Px4TsFailureKind::ipc;
    Px4TsRunDiagnostics diagnostics;
    const auto result = Px4TsRunner::run(arguments, *output, clock, signal,
                                         &failure_kind, &diagnostics);
    const auto finished = output->finish();
    const std::uint64_t completed = output->completed_packets();
    // Release a blocked console/termination handler only when the worker has
    // actually stopped; an abandoned uncancellable writer must not advertise
    // cleanup completion.
    if (cleanup_event != nullptr && output->worker_stopped()) {
        (void)::SetEvent(cleanup_event);
    }

    Error error = result ? Error::OK : result.error();
    if (!finished && error == Error::OK) {
        error = finished.error();
        failure_kind = Px4TsFailureKind::output;
    }
    if (error != Error::OK) {
        std::fprintf(stderr, "px4-ts: %s\n", error_string(error));
        std::fprintf(stderr, "written packets=%llu bytes=%llu\n",
                     static_cast<unsigned long long>(completed),
                     static_cast<unsigned long long>(completed * 188U));
        if (diagnostics.stop_counters_valid)
            print_counters("STOP_STREAM", diagnostics.stop_counters);
        if (diagnostics.stream_end_valid) {
            print_counters("STREAM_END", diagnostics.stream_end.counters);
            std::fprintf(stderr, "STREAM_END error=%u\n",
                         static_cast<unsigned int>(diagnostics.stream_end.error_code));
        }
        return px4_ts_exit_status(error, failure_kind);
    }
    print_counters("stream", result.value().counters);
    return 0;
}

#endif  // _WIN32
