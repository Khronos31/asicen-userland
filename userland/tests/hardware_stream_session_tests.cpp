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
#include <signal.h>
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
        prepared = receiver == expected_receiver && system == expected_system &&
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
        run_started.store(true, std::memory_order_release);
        condition.notify_all();
        condition.wait(lock, [&] { return cancelled.load() || interrupted; });
        if (hold_run_after_interrupt)
            condition.wait(lock, [&] { return release_held_run; });
        return result;
    }

    void interrupt() noexcept override {
        {
            std::lock_guard<std::mutex> lock(mutex);
            interrupted = true;
            interrupt_seen.store(true, std::memory_order_release);
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

    void release_run() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            release_held_run = true;
        }
        condition.notify_all();
    }

    std::mutex mutex;
    std::condition_variable condition;
    std::vector<std::string> events;
    std::vector<std::vector<std::uint8_t>> chunks;
    std::size_t emitted = 0;
    std::uint8_t expected_receiver = 1U;
    ipc::System expected_system = ipc::System::ISDB_T;
    int stop_calls = 0;
    bool prepared = false;
    bool interrupted = false;
    bool prepare_error = false;
    bool stop_error = false;
    bool hold_run_after_interrupt = false;
    bool release_held_run = false;
    std::atomic<bool> run_started{false};
    std::atomic<bool> interrupt_seen{false};
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

class ShutdownTrackingFrontend final : public TunerServiceBackend {
public:
    std::uint8_t receiver_count() const noexcept override { return 4U; }
    bool receiver_supports(std::uint8_t receiver, ipc::System system) const noexcept override {
        return (receiver == 0U && system == ipc::System::ISDB_S) ||
               (receiver == 1U && system == ipc::System::ISDB_T);
    }
    Result<void> open_receiver(std::uint8_t) noexcept override { return Result<void>::success(); }
    Result<void> tune_terrestrial(std::uint8_t, std::uint32_t,
                                  std::uint32_t) noexcept override {
        return Result<void>::success();
    }
    Result<void> tune_satellite(std::uint8_t, std::uint32_t,
                                std::uint32_t) noexcept override {
        return Result<void>::success();
    }
    Result<bool> is_locked(std::uint8_t, ipc::System) noexcept override {
        return Result<bool>::success(true);
    }
    Result<void> select_satellite_slot(std::uint8_t, std::uint8_t,
                                       std::uint32_t) noexcept override {
        return Result<void>::success();
    }
    Result<void> select_satellite_tsid(std::uint8_t, std::uint16_t,
                                       std::uint32_t) noexcept override {
        return Result<void>::success();
    }
    Result<void> close_receiver(std::uint8_t) noexcept override {
        return Result<void>::success();
    }
    Result<void> begin_tune_power(std::uint8_t receiver, ipc::System system,
                                  std::uint8_t voltage) noexcept override {
        ++power_calls;
        power_receiver = receiver;
        power_system = system;
        power_voltage = voltage;
        return power_error == Error::OK ? Result<void>::success()
                                       : Result<void>::failure(power_error);
    }
    Result<void> commit_tune_power(std::uint8_t receiver) noexcept override {
        ++commit_calls;
        power_receiver = receiver;
        return power_error == Error::OK ? Result<void>::success()
                                       : Result<void>::failure(power_error);
    }
    Result<void> rollback_tune_power(std::uint8_t receiver) noexcept override {
        ++rollback_calls;
        power_receiver = receiver;
        return power_error == Error::OK ? Result<void>::success()
                                       : Result<void>::failure(power_error);
    }
    Result<void> shutdown() noexcept override {
        ++shutdown_calls;
        return shutdown_error ? Result<void>::failure(Error::NOT_READY)
                              : Result<void>::success();
    }
    unsigned shutdown_calls = 0;
    bool shutdown_error = false;
    unsigned power_calls = 0;
    unsigned commit_calls = 0;
    unsigned rollback_calls = 0;
    std::uint8_t power_receiver = 0xffU;
    ipc::System power_system = ipc::System::ISDB_T;
    std::uint8_t power_voltage = 0xffU;
    Error power_error = Error::OK;
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

void detach_does_not_publish_stale_final_snapshot_before_worker_join() {
    asicen::MockTunerBackend frontend;
    FakeSource source;
    FakeFatal fatal;
    source.hold_run_after_interrupt = true;
    asicen::HardwareStreamService service(frontend, source, fatal);
    check(service.start_capture(1U, ipc::System::ISDB_T),
          "start capture for final-snapshot publication race");
    const auto id = identity(18U);
    check(service.attach(id), "attach final-snapshot publication race");
    for (int i = 0; i < 200 && !source.run_started.load(std::memory_order_acquire); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    check(source.run_started.load(std::memory_order_acquire), "capture worker is running");

    std::atomic<bool> detach_done{false};
    std::atomic<bool> detach_ok{false};
    std::thread detacher([&] {
        detach_ok.store(static_cast<bool>(service.detach(id)), std::memory_order_release);
        detach_done.store(true, std::memory_order_release);
    });
    for (int i = 0; i < 200 && !source.interrupt_seen.load(std::memory_order_acquire); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    check(source.interrupt_seen.load(std::memory_order_acquire),
          "detach reached the blocked worker join");

    std::array<std::uint8_t, 188> output{};
    const auto interim_read = service.read(id, {output.data(), output.size()}, {0U});
    check(interim_read && interim_read.value().timed_out && !interim_read.value().eof &&
              interim_read.value().terminal == TunerStreamTerminal::none,
          "stream read waits for the final snapshot while detach joins");
    check(service.final_snapshot(id).error() == Error::NOT_READY,
          "an unpublished default snapshot is never exposed");
    check(!detach_done.load(std::memory_order_acquire), "worker join remains blocked");

    source.release_run();
    detacher.join();
    check(detach_ok.load(std::memory_order_acquire), "detach completes after worker exits");
    const auto final = service.final_snapshot(id);
    check(final && final.value().counters.packets == 0U &&
              final.value().terminal == static_cast<std::uint8_t>(TunerStreamTerminal::stopped),
          "only the completed detach publishes the stopped final snapshot");
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
    ShutdownTrackingFrontend frontend;
    FakeSource source;
    FakeFatal fatal;
    asicen::HardwareStreamService service(frontend, source, fatal);
    check(service.start_capture(1U, ipc::System::ISDB_T), "start shutdown failure case");
    auto id = identity(11U);
    check(service.attach(id), "attach shutdown failure case");
    check(service.detach(id), "detach before cleanup failure");
    source.stop_error = true;
    frontend.shutdown_error = true;
    check(service.stop_capture(1U, ipc::System::ISDB_T).error() == Error::USB_IO,
          "shutdown cleanup error is returned");
    check(service.shutdown().error() == Error::USB_IO,
          "shutdown retains earlier stream cleanup failure");
    check(frontend.shutdown_calls == 1U,
          "frontend shutdown still runs after quarantined stream cleanup failure");
    const auto final = service.final_snapshot(id);
    check(final && final.value().counters.usb_errors == 1U &&
              final.value().terminal == static_cast<std::uint8_t>(
                  TunerStreamTerminal::usb_error),
          "cleanup failure upgrades lifecycle stopped state into an error snapshot");
}

void model_capabilities_delegate_without_enabling_unsupported_paths() {
    asicen::MockTunerBackend combined(1U);
    FakeSource source;
    FakeFatal fatal;
    asicen::HardwareStreamService service(combined, source, fatal);
    check(service.receiver_count() == 1U, "single-receiver count is delegated");
    check(service.receiver_supports(0U, ipc::System::ISDB_T) &&
          service.receiver_supports(0U, ipc::System::ISDB_S),
          "shared receiver inherits both supported tune systems");
    check(service.open_receiver(0U), "combined receiver can open");
    check(service.tune_terrestrial(0U, 557142U, 1000U),
          "terrestrial tune need not use logical receiver one");
    check(service.tune_satellite(0U, 1049480U, 1000U),
          "same combined receiver can tune satellite");
    check(service.open_receiver(1U).error() == Error::UNSUPPORTED &&
          service.start_capture(1U, ipc::System::ISDB_T).error() == Error::UNSUPPORTED,
          "out-of-model receiver rejected before source prepare");
    check(source.events.empty(), "unsupported mapping never prepares the source");
    source.expected_receiver = 0U;
    check(service.start_capture(0U, ipc::System::ISDB_T),
          "combined terrestrial receiver reaches source with logical receiver zero");
    check(service.stop_capture(0U, ipc::System::ISDB_T), "combined terrestrial source stops");
    source.expected_system = ipc::System::ISDB_S;
    check(service.start_capture(0U, ipc::System::ISDB_S),
          "same logical receiver can select satellite source after cleanup");
    check(service.stop_capture(0U, ipc::System::ISDB_S), "combined satellite source stops");

    asicen::MockTunerBackend four;
    FakeSource secondary_source;
    asicen::HardwareStreamService secondary(four, secondary_source, fatal);
    check(secondary.receiver_count() == 4U &&
          secondary.receiver_supports(2U, ipc::System::ISDB_S) &&
          secondary.receiver_supports(3U, ipc::System::ISDB_T),
          "wrapper accepts additional receivers only when frontend supports them");
    check(secondary.tune_terrestrial(3U, 557142U, 1000U) &&
          secondary.select_satellite_slot(2U, 0U, 1000U),
          "secondary frontend operations preserve receiver routing");
    check(!secondary.receiver_supports(4U, ipc::System::ISDB_S),
          "wrapper enforces delegated count boundary");
    secondary_source.expected_receiver = 3U;
    check(secondary.start_capture(3U, ipc::System::ISDB_T),
          "supported secondary logical receiver reaches the selected source");
    check(secondary.stop_capture(3U, ipc::System::ISDB_T), "secondary source stops");
}

void primary_satellite_receiver_mapping_is_explicit() {
    ShutdownTrackingFrontend frontend;
    FakeSource source;
    FakeFatal fatal;
    asicen::HardwareStreamService service(frontend, source, fatal);
    check(service.receiver_supports(0U, ipc::System::ISDB_S),
          "primary W3U3 receiver is satellite local zero");
    check(service.receiver_supports(1U, ipc::System::ISDB_T),
          "second W3U3 lane retains terrestrial local one");
    check(!service.receiver_supports(0U, ipc::System::ISDB_T) &&
          !service.receiver_supports(1U, ipc::System::ISDB_S) &&
          !service.receiver_supports(2U, ipc::System::ISDB_S),
          "unsupported receiver/system combinations stay unavailable");
    check(service.tune_satellite(1U, 1049480U, 5000U).error() == Error::UNSUPPORTED,
          "satellite tuning cannot route onto the wrong frontend lane");
    check(service.select_satellite_slot(1U, 0U, 1000U).error() == Error::UNSUPPORTED,
          "satellite TSID selection cannot route onto local one");
}

void lnb_requests_delegate_exact_values_and_errors() {
    ShutdownTrackingFrontend frontend;
    FakeSource source;
    FakeFatal fatal;
    asicen::HardwareStreamService service(frontend, source, fatal);
    for (const std::uint8_t voltage : {15U, 0U}) {
        check(service.begin_tune_power(0U, ipc::System::ISDB_S, voltage),
              "valid satellite LNB request delegates to frontend");
        check(frontend.power_receiver == 0U &&
              frontend.power_system == ipc::System::ISDB_S &&
              frontend.power_voltage == voltage,
              "wrapper preserves ON/OFF and receiver/system exactly");
        check(service.commit_tune_power(0U), "LNB commit delegates");
        check(service.rollback_tune_power(0U), "LNB rollback delegates");
    }
    check(frontend.power_calls == 2U && frontend.commit_calls == 2U &&
          frontend.rollback_calls == 2U, "each valid request is forwarded exactly once");
    for (const auto error : {Error::UNSUPPORTED, Error::USB_IO, Error::DISCONNECTED,
                            Error::NOT_READY, Error::TIMEOUT}) {
        frontend.power_error = error;
        check(service.begin_tune_power(0U, ipc::System::ISDB_S, 15U).error() == error &&
              service.commit_tune_power(0U).error() == error &&
              service.rollback_tune_power(0U).error() == error,
              "frontend denial and hardware/cleanup errors cannot become success");
    }
    const auto calls = frontend.power_calls;
    check(service.begin_tune_power(1U, ipc::System::ISDB_T, 15U).error() ==
              Error::INVALID_ARGUMENT &&
          service.begin_tune_power(0U, ipc::System::ISDB_S, 13U).error() ==
              Error::INVALID_ARGUMENT,
          "invalid voltage and terrestrial ON fail before frontend access");
    check(service.begin_tune_power(2U, ipc::System::ISDB_S, 15U).error() ==
              Error::UNSUPPORTED &&
          service.begin_tune_power(0U, ipc::System::ISDB_T, 0U).error() ==
              Error::UNSUPPORTED,
          "wrong receiver/system cannot reach LNB controls");
    check(frontend.power_calls == calls && source.events.empty(),
          "rejected power requests perform no frontend or capture operation");
}

}  // namespace

int main() {
    start_precedes_attach_and_full_identity_is_required();
    detach_does_not_publish_stale_final_snapshot_before_worker_join();
    overflow_is_sticky_and_quarantines_cleanup_failure();
    failed_prepare_attempts_cleanup_and_reports_quarantine();
    fatal_callback_drain_requests_nonzero_process_exit();
    fatal_callback_drain_exits_child_without_running_destructors();
    shutdown_cleans_source_and_is_sticky_against_restart();
    shutdown_cleanup_failure_is_reported_in_final_state();
    primary_satellite_receiver_mapping_is_explicit();
    lnb_requests_delegate_exact_values_and_errors();
    model_capabilities_delegate_without_enabling_unsupported_paths();
    std::cout << "hardware stream session tests passed\n";
    return 0;
}
