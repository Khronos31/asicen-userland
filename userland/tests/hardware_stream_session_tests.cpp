// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/hardware_stream_session.h"
#include "asicen/px4_mock_backend.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace px4::userland;

void check(bool ok, const char* message) {
    if (!ok) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

template <typename T>
void check(const Result<T>& result, const char* message) {
    check(result.has_value(), message);
}

class FakeSource final : public asicen::StreamCaptureSource {
public:
    Result<void> prepare(std::uint8_t receiver, ipc::System system,
                         const std::atomic<bool>& cancelled) noexcept override {
        std::lock_guard<std::mutex> lock(mutex);
        events.push_back("prepare");
        interrupted = false;
        if (prepare_error) return Result<void>::failure(Error::USB_IO);
        prepared = receiver == 1U && system == ipc::System::ISDB_T &&
                   !cancelled.load();
        return prepared ? Result<void>::success()
                        : Result<void>::failure(Error::UNSUPPORTED);
    }

    asicen::CaptureRunResult run(
        const std::atomic<bool>& cancelled,
        bool (*emit)(void*, const std::uint8_t*, std::size_t),
        void* context) noexcept override {
        for (const auto& chunk : chunks) {
            if (cancelled.load()) break;
            if (!emit(context, chunk.data(), chunk.size())) break;
            {
                std::lock_guard<std::mutex> lock(mutex);
                ++emitted;
            }
            condition.notify_all();
        }
        if (result == asicen::CaptureRunResult::fatal_drain) return result;
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&] { return cancelled.load() || interrupted; });
        return result;
    }

    void interrupt() noexcept override {
        {
            std::lock_guard<std::mutex> lock(mutex);
            interrupted = true;
            events.push_back("interrupt");
        }
        condition.notify_all();
    }

    Result<void> stop() noexcept override {
        std::lock_guard<std::mutex> lock(mutex);
        events.push_back("stop");
        ++stop_calls;
        return stop_error ? Result<void>::failure(Error::USB_IO)
                          : Result<void>::success();
    }

    bool wait_emitted(std::size_t count) {
        std::unique_lock<std::mutex> lock(mutex);
        return condition.wait_for(lock, std::chrono::seconds(2), [&] {
            return emitted >= count;
        });
    }

    std::mutex mutex;
    std::condition_variable condition;
    std::vector<std::string> events;
    std::vector<std::vector<std::uint8_t>> chunks;
    std::size_t emitted = 0;
    int stop_calls = 0;
    bool prepared = false;
    bool interrupted = false;
    bool prepare_error = false;
    bool stop_error = false;
    asicen::CaptureRunResult result = asicen::CaptureRunResult::cancelled;
};

class FakeFatal final : public asicen::StreamProcessFatal {
public:
    void terminate_nonzero(int code) noexcept override {
        exit_code.store(code);
        called.store(true);
    }
    std::atomic<bool> called{false};
    std::atomic<int> exit_code{0};
};

std::vector<std::uint8_t> packet(std::uint8_t cc) {
    std::vector<std::uint8_t> bytes(188U, 0xffU);
    bytes[0] = 0x47U;
    bytes[1] = 0x00U;
    bytes[2] = 0x20U;
    bytes[3] = static_cast<std::uint8_t>(0x10U | cc);
    return bytes;
}

TunerAttachment identity(std::uint64_t attachment_id = 8U) {
    TunerAttachment value{};
    value.owner_client_id = 4U;
    value.lease_id = 6U;
    value.attachment_id = attachment_id;
    value.receiver = 1U;
    value.system = ipc::System::ISDB_T;
    value.nonce[0] = 0x5aU;
    return value;
}

void start_precedes_attach_and_full_identity_is_required() {
    asicen::MockTunerBackend frontend;
    FakeSource source;
    FakeFatal fatal;
    source.chunks = {packet(0), packet(1)};
    asicen::HardwareStreamService service(frontend, source, fatal);
    check(service.start_capture(1U, ipc::System::ISDB_T), "start capture prepares source");
    check(source.wait_emitted(2U), "pre-attach queue receives bounded data");
    auto id = identity();
    check(service.attach(id), "attach registers identity after capture start");
    auto wrong = id;
    wrong.nonce[0] ^= 1U;
    check(service.stats(wrong).error() == Error::NOT_FOUND,
          "nonce mismatch is rejected");
    wrong = id;
    wrong.owner_client_id++;
    check(service.terminal(wrong).error() == Error::NOT_FOUND,
          "owner mismatch is rejected");
    std::array<std::uint8_t, 188> output{};
    const auto read = service.read(id, {output.data(), output.size()}, {20U});
    check(read && read.value().bytes == output.size() && output[0] == 0x47U,
          "reader receives the pre-attach packets");
    std::array<std::uint8_t, 189> odd_output{};
    const auto odd_read = service.read(id, {odd_output.data(), odd_output.size()}, {0U});
    check(odd_read && odd_read.value().bytes == 188U,
          "read does not split TS packets when only 188 bytes remain");
    check(service.detach(id), "detach stops and joins producer");
    check(service.read(id, {output.data(), output.size()}, {0U}).error() == Error::NOT_FOUND,
          "detach invalidates live attachment before return");
    const auto final = service.final_snapshot(id);
    check(final && final.value().counters.bytes == output.size() + 188U &&
              final.value().terminal == static_cast<std::uint8_t>(
                  TunerStreamTerminal::stopped),
          "final counters are retained for exact identity");
    check(service.stop_capture(1U, ipc::System::ISDB_T), "cleanup succeeds after detach");
    check(service.release_final(id), "final state is released after protocol flush");
    check(service.start_capture(1U, ipc::System::ISDB_T), "fresh generation can restart");
    check(service.stop_capture(1U, ipc::System::ISDB_T), "unattached attempt can stop");
}

void overflow_is_sticky_and_quarantines_cleanup_failure() {
    asicen::MockTunerBackend frontend;
    FakeSource source;
    FakeFatal fatal;
    source.chunks = {packet(0), packet(1)};
    asicen::HardwareStreamService service(frontend, source, fatal, 188U);
    check(service.start_capture(1U, ipc::System::ISDB_T), "start for overflow case");
    auto id = identity(9U);
    check(service.attach(id), "attach overflow case");
    check(source.wait_emitted(1U), "first packet is queued");
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto terminal = service.terminal(id);
    check(terminal && terminal.value() ==
              TunerStreamTerminal::slow_consumer,
          "queue overflow is a sticky terminal, not a silent drop");
    check(service.detach(id), "overflow worker joins");
    source.stop_error = true;
    check(service.stop_capture(1U, ipc::System::ISDB_T).error() == Error::USB_IO,
          "cleanup failure is returned");
    const auto final = service.final_snapshot(id);
    check(final && final.value().counters.usb_errors == 1U &&
              final.value().terminal == static_cast<std::uint8_t>(
                  TunerStreamTerminal::slow_consumer),
          "quarantined final counters retain first terminal and cleanup error");
    check(service.release_final(id).error() == Error::NOT_READY,
          "quarantined snapshot cannot make the hardware reusable");
    check(service.start_capture(1U, ipc::System::ISDB_T).error() == Error::USB_IO,
          "unclean session is quarantined against reuse");
    check(!fatal.called.load(), "ordinary cleanup failure does not invoke fatal callback");
}

void failed_prepare_attempts_cleanup_and_reports_quarantine() {
    asicen::MockTunerBackend frontend;
    FakeSource source;
    FakeFatal fatal;
    source.prepare_error = true;
    source.stop_error = true;
    asicen::HardwareStreamService service(frontend, source, fatal);
    check(service.start_capture(1U, ipc::System::ISDB_T).error() == Error::USB_IO,
          "preparation error is preserved");
    check(service.start_capture(1U, ipc::System::ISDB_T).error() == Error::USB_IO,
          "failed compensating cleanup quarantines hardware");
    check(source.stop_calls == 1, "partial start receives one cleanup attempt");
}

void fatal_callback_drain_requests_nonzero_process_exit() {
    asicen::MockTunerBackend frontend;
    FakeSource source;
    FakeFatal fatal;
    source.result = asicen::CaptureRunResult::fatal_drain;
    asicen::HardwareStreamService service(frontend, source, fatal);
    check(service.start_capture(1U, ipc::System::ISDB_T), "start fatal case");
    for (int i = 0; i < 200 && !fatal.called.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    check(fatal.called.load() && fatal.exit_code.load() == 70,
          "fatal pending callback path requests nonzero process termination");
}

void fatal_callback_drain_exits_child_without_running_destructors() {
    const pid_t child = ::fork();
    check(child >= 0, "fork fatal-drain test child");
    if (child == 0) {
        asicen::MockTunerBackend frontend;
        FakeSource source;
        source.result = asicen::CaptureRunResult::fatal_drain;
        asicen::ExitProcessFatal fatal;
        asicen::HardwareStreamService service(frontend, source, fatal);
        if (!service.start_capture(1U, ipc::System::ISDB_T)) ::_exit(101);
        for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    int status = 0;
    bool exited = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        const pid_t result = ::waitpid(child, &status, WNOHANG);
        if (result == child) {
            exited = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!exited) {
        (void)::kill(child, SIGKILL);
        (void)::waitpid(child, &status, 0);
    }
    check(exited && WIFEXITED(status) && WEXITSTATUS(status) == 70,
          "pending-callback fatal path exits child nonzero within bound");
}

void shutdown_cleans_source_and_is_sticky_against_restart() {
    asicen::MockTunerBackend frontend;
    FakeSource source;
    FakeFatal fatal;
    asicen::HardwareStreamService service(frontend, source, fatal);
    check(service.start_capture(1U, ipc::System::ISDB_T), "start shutdown case");
    check(service.shutdown(), "shutdown joins worker and cleans source");
    check(source.stop_calls == 1, "shutdown invokes source cleanup");
    check(service.start_capture(1U, ipc::System::ISDB_T).error() == Error::NOT_READY,
          "global stop cannot be cleared by a later capture start");
}

void shutdown_cleanup_failure_is_reported_in_final_state() {
    asicen::MockTunerBackend frontend;
    FakeSource source;
    FakeFatal fatal;
    asicen::HardwareStreamService service(frontend, source, fatal);
    check(service.start_capture(1U, ipc::System::ISDB_T), "start shutdown failure case");
    auto id = identity(11U);
    check(service.attach(id), "attach shutdown failure case");
    check(service.detach(id), "detach before cleanup failure");
    source.stop_error = true;
    check(service.stop_capture(1U, ipc::System::ISDB_T).error() == Error::USB_IO,
          "shutdown cleanup error is returned");
    const auto final = service.final_snapshot(id);
    check(final && final.value().counters.usb_errors == 1U &&
              final.value().terminal == static_cast<std::uint8_t>(
                  TunerStreamTerminal::usb_error),
          "cleanup failure upgrades lifecycle stopped state into an error snapshot");
}

}  // namespace

int main() {
    start_precedes_attach_and_full_identity_is_required();
    overflow_is_sticky_and_quarantines_cleanup_failure();
    failed_prepare_attempts_cleanup_and_reports_quarantine();
    fatal_callback_drain_requests_nonzero_process_exit();
    fatal_callback_drain_exits_child_without_running_destructors();
    shutdown_cleans_source_and_is_sticky_against_restart();
    shutdown_cleanup_failure_is_reported_in_final_state();
    std::cout << "hardware stream session tests passed\n";
    return 0;
}
