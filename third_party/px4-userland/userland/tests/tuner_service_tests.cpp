// SPDX-License-Identifier: GPL-2.0-only
#include "px4/tuner_service.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace px4::userland;
using namespace px4::userland::ipc;

#define TUNER_CHECK(condition)                                                        \
    do {                                                                               \
        if (!(condition)) {                                                            \
            std::fprintf(stderr, "tuner service check failed at line %d: %s\n",       \
                         __LINE__, #condition);                                       \
            return false;                                                              \
        }                                                                              \
    } while (false)

class FakeBackend final : public TunerServiceBackend {
public:
    enum class Operation : std::uint8_t {
        open, tune_t, tune_s, lock, slot, tsid, start_capture, stop_capture,
        mark_disconnected, close
    };

    Result<void> open_receiver(std::uint8_t receiver) noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex);
        opened.push_back(receiver);
        operations.push_back(Operation::open);
        return take(open_failures);
    }

    Result<void> tune_terrestrial(std::uint8_t receiver,
                                  std::uint32_t,
                                  std::uint32_t timeout_ms) noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex);
        tuned_receivers.push_back(receiver);
        operations.push_back(Operation::tune_t);
        last_tune_timeout = timeout_ms;
        if (time != nullptr && tune_duration_ms != 0U)
            time->sleep_ms(tune_duration_ms);
        return take(tune_failures);
    }

    Result<void> tune_satellite(std::uint8_t receiver,
                                std::uint32_t,
                                std::uint32_t timeout_ms) noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex);
        tuned_receivers.push_back(receiver);
        operations.push_back(Operation::tune_s);
        last_tune_timeout = timeout_ms;
        if (time != nullptr && tune_duration_ms != 0U)
            time->sleep_ms(tune_duration_ms);
        return take(tune_failures);
    }

    Result<bool> is_locked(std::uint8_t,
                           System) noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex);
        operations.push_back(Operation::lock);
        if (!lock_results.empty()) {
            const auto result = lock_results.front();
            lock_results.pop_front();
            return result;
        }
        return Result<bool>::success(true);
    }

    Result<void> select_satellite_slot(std::uint8_t,
                                       std::uint8_t,
                                       std::uint32_t timeout_ms) noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex);
        operations.push_back(Operation::slot);
        last_select_timeout = timeout_ms;
        if (time != nullptr && select_duration_ms != 0U)
            time->sleep_ms(select_duration_ms);
        return take(slot_failures);
    }

    Result<void> select_satellite_tsid(std::uint8_t,
                                       std::uint16_t,
                                       std::uint32_t timeout_ms) noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex);
        operations.push_back(Operation::tsid);
        last_select_timeout = timeout_ms;
        if (time != nullptr && select_duration_ms != 0U)
            time->sleep_ms(select_duration_ms);
        return take(tsid_failures);
    }

    Result<void> close_receiver(std::uint8_t receiver) noexcept override
    {
        std::unique_lock<std::mutex> lock(mutex);
        closed.push_back(receiver);
        operations.push_back(Operation::close);
        if (sequence != nullptr) sequence->push_back(5);
        if (block_close && receiver == blocked_close_receiver) {
            close_entered = true;
            capture_cv.notify_all();
            capture_cv.wait(lock, [&]() noexcept { return allow_close; });
        }
        return take(close_failures);
    }

    Result<void> start_capture(std::uint8_t receiver, System system) noexcept override
    {
        std::unique_lock<std::mutex> lock(mutex);
        capture_receivers.push_back(receiver);
        capture_systems.push_back(system);
        operations.push_back(Operation::start_capture);
        if (sequence != nullptr) sequence->push_back(3);
        if (block_capture_start) {
            capture_start_entered = true;
            capture_cv.notify_all();
            capture_cv.wait(lock, [&]() noexcept { return allow_capture_start; });
        }
        return take(capture_start_failures);
    }

    Result<void> stop_capture(std::uint8_t receiver, System system) noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex);
        capture_stop_receivers.push_back(receiver);
        capture_stop_systems.push_back(system);
        operations.push_back(Operation::stop_capture);
        if (sequence != nullptr) sequence->push_back(4);
        return take(capture_stop_failures);
    }

    Result<void> begin_tune_power(std::uint8_t receiver, System system,
                                  std::uint8_t voltage) noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex);
        power_begin_receivers.push_back(receiver);
        power_begin_systems.push_back(system);
        power_begin_voltages.push_back(voltage);
        if (voltage == 15U && !allow_lnb_15v)
            return Result<void>::failure(Error::UNSUPPORTED);
        return take(power_begin_failures);
    }

    Result<void> commit_tune_power(std::uint8_t receiver) noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex);
        power_commits.push_back(receiver);
        return take(power_commit_failures);
    }

    Result<void> rollback_tune_power(std::uint8_t receiver) noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex);
        power_rollbacks.push_back(receiver);
        rollback_observed_disconnect = !disconnected_receivers.empty();
        return take(power_rollback_failures);
    }

    void mark_receiver_disconnected(std::uint8_t receiver) noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex);
        disconnected_receivers.push_back(receiver);
        operations.push_back(Operation::mark_disconnected);
    }

    Result<void> shutdown() noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex);
        ++shutdown_calls;
        return take(shutdown_failures);
    }

    static Result<void> take(std::deque<Error>& failures) noexcept
    {
        if (failures.empty()) return Result<void>::success();
        const Error error = failures.front();
        failures.pop_front();
        return error == Error::OK ? Result<void>::success()
                                  : Result<void>::failure(error);
    }

    std::deque<Error> open_failures;
    std::deque<Error> tune_failures;
    std::deque<Error> slot_failures;
    std::deque<Error> tsid_failures;
    std::deque<Error> close_failures;
    std::deque<Error> capture_start_failures;
    std::deque<Error> capture_stop_failures;
    std::deque<Error> power_begin_failures;
    std::deque<Error> power_commit_failures;
    std::deque<Error> power_rollback_failures;
    std::deque<Error> shutdown_failures;
    std::deque<Result<bool>> lock_results;
    std::vector<std::uint8_t> opened;
    std::vector<std::uint8_t> tuned_receivers;
    std::vector<std::uint8_t> closed;
    std::vector<std::uint8_t> capture_receivers;
    std::vector<std::uint8_t> capture_stop_receivers;
    std::vector<System> capture_systems;
    std::vector<System> capture_stop_systems;
    std::vector<std::uint8_t> power_begin_receivers;
    std::vector<System> power_begin_systems;
    std::vector<std::uint8_t> power_begin_voltages;
    std::vector<std::uint8_t> power_commits;
    std::vector<std::uint8_t> power_rollbacks;
    std::vector<std::uint8_t> disconnected_receivers;
    std::vector<Operation> operations;
    TunerServiceTime* time = nullptr;
    std::uint32_t tune_duration_ms = 0U;
    std::uint32_t select_duration_ms = 0U;
    std::uint32_t last_tune_timeout = 0U;
    std::uint32_t last_select_timeout = 0U;
    std::mutex mutex;
    std::condition_variable capture_cv;
    bool block_capture_start = false;
    bool capture_start_entered = false;
    bool allow_capture_start = false;
    bool block_close = false;
    bool close_entered = false;
    bool allow_close = false;
    std::uint8_t blocked_close_receiver = 0U;
    bool allow_lnb_15v = false;
    bool rollback_observed_disconnect = false;
    std::size_t shutdown_calls = 0U;
    std::vector<int>* sequence = nullptr;
};

class FakeStreamControl final : public TunerStreamControl {
public:
    Result<void> attach(const TunerAttachment& value) noexcept override
    {
        std::unique_lock<std::mutex> lock(mutex);
        attachments.push_back(value);
        if (sequence != nullptr) sequence->push_back(1);
        if (block_attach) {
            attach_entered = true;
            changed.notify_all();
            changed.wait(lock, [&]() noexcept { return allow_attach; });
        }
        if (attach_error != Error::OK)
            return Result<void>::failure(attach_error);
        return Result<void>::success();
    }

    Result<void> detach(const TunerAttachment& value) noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex);
        detached.push_back(value);
        if (sequence != nullptr) sequence->push_back(2);
        if (detach_errors.empty()) return Result<void>::success();
        const Error error = detach_errors.front();
        detach_errors.pop_front();
        return error == Error::OK ? Result<void>::success()
                                  : Result<void>::failure(error);
    }

    Result<TunerStreamCounters> stats(
        const TunerAttachment& value) const noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (value.attachment_id == 0U || !stats_available)
            return Result<TunerStreamCounters>::failure(Error::NOT_FOUND);
        return Result<TunerStreamCounters>::success(stats_value);
    }

    Result<TunerStreamFinalSnapshot> final_snapshot(
        const TunerAttachment& value) const noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (value.attachment_id == 0U || !final_snapshot_available)
            return Result<TunerStreamFinalSnapshot>::failure(Error::NOT_FOUND);
        return Result<TunerStreamFinalSnapshot>::success(final_snapshot_value);
    }

    bool wait_attach_entered() noexcept
    {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(5), [&]() noexcept {
            return attach_entered;
        });
    }

    void allow_blocked_attach() noexcept
    {
        std::lock_guard<std::mutex> lock(mutex);
        allow_attach = true;
        changed.notify_all();
    }

    Error attach_error = Error::OK;
    std::deque<Error> detach_errors;
    std::vector<TunerAttachment> attachments;
    std::vector<TunerAttachment> detached;
    TunerStreamCounters stats_value{};
    TunerStreamFinalSnapshot final_snapshot_value{};
    bool stats_available = false;
    bool final_snapshot_available = false;
    std::vector<int>* sequence = nullptr;
    mutable std::mutex mutex;
    std::condition_variable changed;
    bool block_attach = false;
    bool attach_entered = false;
    bool allow_attach = false;
};

class AttachmentIds final : public TunerAttachmentIdSource {
public:
    Result<std::uint64_t> next() noexcept override
    {
        ++calls;
        if (values.empty()) return Result<std::uint64_t>::failure(Error::INTERNAL);
        const auto value = values.front();
        values.pop_front();
        return Result<std::uint64_t>::success(value);
    }

    std::deque<std::uint64_t> values;
    std::size_t calls = 0U;
};

class Nonce final : public TunerNonceSource {
public:
    Result<std::array<std::uint8_t, kNonceLength>> generate() noexcept override
    {
        ++calls;
        if (failure != Error::OK)
            return Result<std::array<std::uint8_t, kNonceLength>>::failure(failure);
        std::array<std::uint8_t, kNonceLength> value{};
        value[0] = next++;
        return Result<std::array<std::uint8_t, kNonceLength>>::success(value);
    }

    Error failure = Error::OK;
    std::uint8_t next = 1U;
    std::size_t calls = 0U;
};

class Clock final : public TunerServiceTime {
public:
    std::uint64_t monotonic_ms() noexcept override { return now; }
    void sleep_ms(std::uint32_t milliseconds) noexcept override
    {
        sleeps.push_back(milliseconds);
        now += milliseconds;
    }

    std::uint64_t now = 0U;
    std::vector<std::uint32_t> sleeps;
};

class LeaseIds final : public TunerLeaseIdSource {
public:
    Result<std::uint64_t> next() noexcept override
    {
        ++calls;
        if (values.empty()) return Result<std::uint64_t>::failure(Error::INTERNAL);
        const auto value = values.front();
        values.pop_front();
        return Result<std::uint64_t>::success(value);
    }

    std::deque<std::uint64_t> values;
    std::size_t calls = 0U;
};

class ReservationBackend final : public TunerServiceBackend {
public:
    Result<void> open_receiver(std::uint8_t) noexcept override
    {
        ++open_count;
        while (!allow_open.load()) std::this_thread::yield();
        return Result<void>::success();
    }

    Result<void> tune_terrestrial(std::uint8_t, std::uint32_t,
                                  std::uint32_t) noexcept override
    {
        return Result<void>::success();
    }
    Result<void> tune_satellite(std::uint8_t, std::uint32_t,
                                std::uint32_t) noexcept override
    {
        return Result<void>::success();
    }
    Result<bool> is_locked(std::uint8_t, System) noexcept override
    {
        return Result<bool>::success(true);
    }
    Result<void> select_satellite_slot(std::uint8_t, std::uint8_t,
                                       std::uint32_t) noexcept override
    {
        return Result<void>::success();
    }
    Result<void> select_satellite_tsid(std::uint8_t, std::uint16_t,
                                       std::uint32_t) noexcept override
    {
        return Result<void>::success();
    }
    Result<void> close_receiver(std::uint8_t) noexcept override
    {
        ++close_count;
        return Result<void>::success();
    }

    std::atomic<unsigned> open_count{0U};
    std::atomic<unsigned> close_count{0U};
    std::atomic<bool> allow_open{false};
};

ipc::TuneRequestPayload terrestrial(std::uint64_t lease_id,
                                    std::uint64_t frequency = 40000U) noexcept
{
    return TuneRequestPayload{lease_id, System::ISDB_T, frequency, 0xffffU,
                               0xffffU, 6000000U, 0U, 100U};
}

ipc::TuneRequestPayload satellite(std::uint64_t lease_id,
                                  std::uint64_t frequency = 146875U) noexcept
{
    return TuneRequestPayload{lease_id, System::ISDB_S, frequency, 0xffffU,
                               0U, 0U, 0U, 100U};
}

bool test_mapping_exclusive_and_generation()
{
    FakeBackend backend;
    Nonce nonce;
    Clock clock;
    TunerService service(backend, nonce, clock);

    for (std::uint8_t receiver = 0U; receiver < 8U; ++receiver) {
        const auto acquired = service.acquire(receiver + 1U, receiver);
        TUNER_CHECK(acquired);
        TUNER_CHECK(acquired.value().lease_id == receiver + 1U);
    }
    TUNER_CHECK(!service.acquire(20U, 0U));
    TUNER_CHECK(service.acquire(20U, 8U).error() == Error::INVALID_ARGUMENT);
    const auto status = service.status();
    TUNER_CHECK(status && status.value().generation == 9U);
    for (const auto state : status.value().receiver_states)
        TUNER_CHECK(state == ReceiverState::leased);
    for (std::uint8_t receiver = 0U; receiver < 8U; ++receiver)
        TUNER_CHECK(service.release(receiver + 1U, receiver + 1U));
    TUNER_CHECK(service.status().value().generation == 17U);
    return true;
}

bool test_all_receiver_system_mapping()
{
    FakeBackend backend;
    Nonce nonce;
    Clock clock;
    TunerService service(backend, nonce, clock);
    std::array<TunerAcquireResult, kReceiverCount> acquired{};

    for (std::uint8_t receiver = 0U; receiver < kReceiverCount; ++receiver) {
        const auto result = service.acquire(receiver + 1U, receiver);
        TUNER_CHECK(result);
        acquired[receiver] = result.value();
        const bool satellite_receiver = receiver == 0U || receiver == 1U ||
                                        receiver == 4U || receiver == 5U;
        const auto request = satellite_receiver
            ? satellite(result.value().lease_id)
            : terrestrial(result.value().lease_id);
        const auto tuned = service.tune(receiver + 1U, request);
        if (!tuned) {
            std::fprintf(stderr, "mapping tune receiver %u failed: %u\n",
                         receiver, static_cast<unsigned>(tuned.error()));
            return false;
        }
    }
    const auto status = service.status();
    TUNER_CHECK(status);
    for (const auto state : status.value().receiver_states)
        TUNER_CHECK(state == ReceiverState::tuned);
    for (std::uint8_t receiver = 0U; receiver < kReceiverCount; ++receiver)
        TUNER_CHECK(service.release(receiver + 1U, acquired[receiver].lease_id));
    return true;
}

bool test_acquire_failures_and_lease_collision()
{
    FakeBackend backend;
    Nonce nonce;
    Clock clock;
    LeaseIds ids;
    ids.values = {0U, 4U, 4U, 5U};
    TunerService service(backend, nonce, clock, &ids);

    TUNER_CHECK(service.acquire(0U, 0U).error() == Error::INVALID_ARGUMENT);
    nonce.failure = Error::USB_IO;
    TUNER_CHECK(service.acquire(1U, 0U).error() == Error::USB_IO);
    TUNER_CHECK(backend.opened.empty());
    nonce.failure = Error::OK;
    TUNER_CHECK(service.acquire(1U, 0U).error() == Error::INVALID_ARGUMENT);
    TUNER_CHECK(backend.opened.empty());

    ids.values = {4U};
    backend.open_failures.push_back(Error::TIMEOUT);
    TUNER_CHECK(service.acquire(1U, 0U).error() == Error::TIMEOUT);
    TUNER_CHECK(service.status().value().receiver_states[0] == ReceiverState::free);

    ids.values = {4U};
    TUNER_CHECK(service.acquire(1U, 0U));
    ids.values = {4U, 5U};
    TUNER_CHECK(service.acquire(2U, 1U));
    TUNER_CHECK(service.release(1U, 4U));
    TUNER_CHECK(service.release(2U, 5U));
    return true;
}

bool test_tune_parameters_and_operation_order()
{
    FakeBackend backend;
    Nonce nonce;
    Clock clock;
    TunerService service(backend, nonce, clock);
    const auto terrestrial_lease = service.acquire(1U, 2U);
    const auto satellite_lease = service.acquire(2U, 0U);
    TUNER_CHECK(terrestrial_lease && satellite_lease);

    const auto terrestrial_result = service.tune(1U, terrestrial(terrestrial_lease.value().lease_id,
                                                                  1002000U));
    TUNER_CHECK(terrestrial_result && terrestrial_result.value().locked == 1U);
    TUNER_CHECK(backend.operations[2] == FakeBackend::Operation::tune_t);
    TUNER_CHECK(backend.operations[3] == FakeBackend::Operation::lock);

    const auto satellite_result = service.tune(2U, satellite(satellite_lease.value().lease_id,
                                                             2350000U));
    TUNER_CHECK(satellite_result);
    TUNER_CHECK(backend.operations[4] == FakeBackend::Operation::tune_s);
    TUNER_CHECK(backend.operations[5] == FakeBackend::Operation::lock);
    TUNER_CHECK(backend.operations[6] == FakeBackend::Operation::slot);

    auto direct = satellite(satellite_lease.value().lease_id);
    direct.stream_id = 0U;
    direct.slot = 0xffffU;
    TUNER_CHECK(service.tune(2U, direct));
    TUNER_CHECK(backend.operations.back() == FakeBackend::Operation::tsid);
    TUNER_CHECK(service.status().value().receiver_states[2] == ReceiverState::tuned);
    TUNER_CHECK(service.status().value().receiver_states[0] == ReceiverState::tuned);
    return true;
}

bool test_tune_uses_one_end_to_end_budget()
{
    FakeBackend backend;
    Nonce nonce;
    Clock clock;
    backend.time = &clock;
    backend.tune_duration_ms = 60U;
    backend.select_duration_ms = 35U;
    TunerService service(backend, nonce, clock);
    const auto acquired = service.acquire(1U, 0U);
    TUNER_CHECK(acquired);

    const auto result = service.tune(1U, satellite(acquired.value().lease_id));
    TUNER_CHECK(result);
    TUNER_CHECK(clock.now == 95U);
    TUNER_CHECK(backend.last_tune_timeout == 100U);
    TUNER_CHECK(backend.last_select_timeout == 40U);

    FakeBackend timeout_backend;
    Nonce timeout_nonce;
    Clock timeout_clock;
    timeout_backend.time = &timeout_clock;
    timeout_backend.tune_duration_ms = 60U;
    for (std::size_t count = 0U; count < 20U; ++count)
        timeout_backend.lock_results.push_back(Result<bool>::success(false));
    TunerService timeout_service(timeout_backend, timeout_nonce, timeout_clock);
    const auto timeout_lease = timeout_service.acquire(1U, 2U);
    TUNER_CHECK(timeout_lease);
    const auto timed_out = timeout_service.tune(
        1U, terrestrial(timeout_lease.value().lease_id));
    TUNER_CHECK(!timed_out && timed_out.error() == Error::TIMEOUT);
    TUNER_CHECK(timeout_clock.now == 100U);
    TUNER_CHECK(timeout_backend.last_select_timeout == 0U);
    return true;
}

bool test_tune_validation_lock_timeout_and_retune()
{
    FakeBackend backend;
    Nonce nonce;
    Clock clock;
    TunerService service(backend, nonce, clock);
    const auto acquired = service.acquire(1U, 2U);
    const auto satellite_acquired = service.acquire(2U, 0U);
    TUNER_CHECK(acquired && satellite_acquired);

    auto invalid = terrestrial(acquired.value().lease_id, 39999U);
    TUNER_CHECK(service.tune(1U, invalid).error() == Error::INVALID_ARGUMENT);
    invalid = terrestrial(acquired.value().lease_id);
    invalid.bandwidth_hz = 0U;
    TUNER_CHECK(service.tune(1U, invalid).error() == Error::INVALID_ARGUMENT);
    invalid.timeout_ms = 99U;
    TUNER_CHECK(service.tune(1U, invalid).error() == Error::INVALID_ARGUMENT);
    invalid.timeout_ms = 30001U;
    TUNER_CHECK(service.tune(1U, invalid).error() == Error::INVALID_ARGUMENT);

    auto unsupported = satellite(satellite_acquired.value().lease_id);
    unsupported.lnb_voltage = 15U;
    TUNER_CHECK(service.tune(2U, unsupported).error() == Error::UNSUPPORTED);
    unsupported.lnb_voltage = 1U;
    TUNER_CHECK(service.tune(2U, unsupported).error() == Error::INVALID_ARGUMENT);
    TUNER_CHECK(service.tune(2U, terrestrial(acquired.value().lease_id)).error() == Error::NOT_FOUND);

    for (std::size_t count = 0U; count < 10U; ++count)
        backend.lock_results.push_back(Result<bool>::success(false));
    const auto timed_out = service.tune(1U, terrestrial(acquired.value().lease_id));
    TUNER_CHECK(!timed_out && timed_out.error() == Error::TIMEOUT);
    TUNER_CHECK(service.status().value().receiver_states[2] == ReceiverState::leased);
    TUNER_CHECK(service.tune(1U, terrestrial(acquired.value().lease_id)));
    TUNER_CHECK(service.release(2U, satellite_acquired.value().lease_id));
    return true;
}

bool test_tune_failures_are_recoverable()
{
    FakeBackend backend;
    Nonce nonce;
    Clock clock;
    TunerService service(backend, nonce, clock);
    const auto terrestrial_lease = service.acquire(1U, 2U);
    const auto satellite_lease = service.acquire(2U, 0U);
    TUNER_CHECK(terrestrial_lease && satellite_lease);

    backend.tune_failures.push_back(Error::USB_IO);
    TUNER_CHECK(service.tune(1U, terrestrial(terrestrial_lease.value().lease_id))
                    .error() == Error::USB_IO);
    TUNER_CHECK(service.status().value().receiver_states[2] == ReceiverState::leased);
    TUNER_CHECK(service.tune(1U, terrestrial(terrestrial_lease.value().lease_id)));

    backend.lock_results.push_back(Result<bool>::failure(Error::TIMEOUT));
    TUNER_CHECK(service.tune(2U, satellite(satellite_lease.value().lease_id))
                    .error() == Error::TIMEOUT);
    TUNER_CHECK(service.status().value().receiver_states[0] == ReceiverState::leased);

    backend.slot_failures.push_back(Error::USB_IO);
    TUNER_CHECK(service.tune(2U, satellite(satellite_lease.value().lease_id))
                    .error() == Error::USB_IO);
    TUNER_CHECK(service.status().value().receiver_states[0] == ReceiverState::leased);

    backend.tsid_failures.push_back(Error::USB_IO);
    auto direct = satellite(satellite_lease.value().lease_id);
    direct.stream_id = 0x1234U;
    direct.slot = 0xffffU;
    TUNER_CHECK(service.tune(2U, direct).error() == Error::USB_IO);
    TUNER_CHECK(service.status().value().receiver_states[0] == ReceiverState::leased);
    TUNER_CHECK(service.release(1U, terrestrial_lease.value().lease_id));
    TUNER_CHECK(service.release(2U, satellite_lease.value().lease_id));
    return true;
}

bool test_lnb_tune_transaction_and_failure_rollback()
{
    FakeBackend backend;
    backend.allow_lnb_15v = true;
    Nonce nonce;
    Clock clock;
    backend.time = &clock;
    TunerService service(backend, nonce, clock);
    const auto acquired = service.acquire(1U, 0U);
    TUNER_CHECK(acquired);
    auto request = satellite(acquired.value().lease_id);
    request.lnb_voltage = 15U;

    TUNER_CHECK(service.tune(1U, request));
    TUNER_CHECK((backend.power_begin_receivers == std::vector<std::uint8_t>{0U}));
    TUNER_CHECK((backend.power_begin_systems == std::vector<System>{System::ISDB_S}));
    TUNER_CHECK((backend.power_begin_voltages == std::vector<std::uint8_t>{15U}));
    TUNER_CHECK((backend.power_commits == std::vector<std::uint8_t>{0U}));
    TUNER_CHECK(backend.power_rollbacks.empty());

    const auto expect_rollback = [&](Error expected) {
        const std::size_t before = backend.power_rollbacks.size();
        const auto result = service.tune(1U, request);
        return !result && result.error() == expected &&
               backend.power_rollbacks.size() == before + 1U &&
               backend.power_rollbacks.back() == 0U;
    };

    backend.tune_failures.push_back(Error::USB_IO);
    TUNER_CHECK(expect_rollback(Error::USB_IO));
    backend.lock_results.push_back(Result<bool>::failure(Error::TIMEOUT));
    TUNER_CHECK(expect_rollback(Error::TIMEOUT));
    for (std::size_t count = 0U; count < 10U; ++count)
        backend.lock_results.push_back(Result<bool>::success(false));
    TUNER_CHECK(expect_rollback(Error::TIMEOUT));
    backend.slot_failures.push_back(Error::PROTOCOL_ERROR);
    TUNER_CHECK(expect_rollback(Error::PROTOCOL_ERROR));

    auto direct = request;
    direct.slot = 0xffffU;
    direct.stream_id = 0x1234U;
    backend.tsid_failures.push_back(Error::USB_IO);
    const std::size_t before_direct = backend.power_rollbacks.size();
    const auto direct_failed = service.tune(1U, direct);
    TUNER_CHECK(!direct_failed && direct_failed.error() == Error::USB_IO &&
                backend.power_rollbacks.size() == before_direct + 1U);

    backend.select_duration_ms = 100U;
    const std::size_t before_deadline = backend.power_rollbacks.size();
    const auto deadline = service.tune(1U, request);
    TUNER_CHECK(!deadline && deadline.error() == Error::TIMEOUT &&
                backend.power_rollbacks.size() == before_deadline + 1U);
    backend.select_duration_ms = 0U;

    backend.power_commit_failures.push_back(Error::USB_IO);
    TUNER_CHECK(expect_rollback(Error::USB_IO));

    // begin_tune_power owns its own physical rollback. A failed preparation
    // must not enter frontend tuning or invoke the service-level rollback.
    const std::size_t tune_count_before_prepare = backend.tuned_receivers.size();
    const std::size_t rollback_count_before_prepare =
        backend.power_rollbacks.size();
    backend.power_begin_failures.push_back(Error::TIMEOUT);
    const auto prepare_failed = service.tune(1U, request);
    TUNER_CHECK(!prepare_failed && prepare_failed.error() == Error::TIMEOUT &&
                backend.tuned_receivers.size() == tune_count_before_prepare &&
                backend.power_rollbacks.size() == rollback_count_before_prepare);

    // A cleanup failure never turns a failed tune into success and makes the
    // receiver state explicitly unsafe until release.
    backend.tune_failures.push_back(Error::USB_IO);
    backend.power_rollback_failures.push_back(Error::TIMEOUT);
    const auto cleanup_failed = service.tune(1U, request);
    TUNER_CHECK(!cleanup_failed && cleanup_failed.error() == Error::USB_IO);
    TUNER_CHECK(service.status().value().receiver_states[0U] == ReceiverState::error);
    TUNER_CHECK(service.release(1U, acquired.value().lease_id));
    return true;
}

bool test_disconnect_reaches_backend_power_authority()
{
    // A frontend loss is reported before transactional LNB rollback.  The
    // production marker makes that bridge terminal, so rollback cannot issue
    // a post-disconnect GPIO write.
    {
        FakeBackend backend;
        backend.allow_lnb_15v = true;
        Nonce nonce;
        Clock clock;
        TunerService service(backend, nonce, clock);
        const auto acquired = service.acquire(1U, 0U);
        TUNER_CHECK(acquired);
        auto request = satellite(acquired.value().lease_id);
        request.lnb_voltage = 15U;
        backend.tune_failures.push_back(Error::DISCONNECTED);
        const auto tuned = service.tune(1U, request);
        TUNER_CHECK(!tuned && tuned.error() == Error::DISCONNECTED);
        TUNER_CHECK((backend.disconnected_receivers ==
                     std::vector<std::uint8_t>{0U}));
        TUNER_CHECK(backend.power_rollbacks.size() == 1U &&
                    backend.rollback_observed_disconnect);
        (void)service.release(1U, acquired.value().lease_id);
    }

    // A loss reported by the data-plane detach reaches the same metadata-only
    // marker and suppresses later capture/close hardware operations.
    {
        FakeBackend backend;
        FakeStreamControl stream_control;
        Nonce nonce;
        Clock clock;
        TunerService service(backend, nonce, clock, nullptr, nullptr,
                             &stream_control);
        const auto acquired = service.acquire(2U, 2U);
        TUNER_CHECK(acquired &&
                    service.tune(2U, terrestrial(acquired.value().lease_id)) &&
                    service.start_stream(2U, acquired.value().lease_id) &&
                    service.attach_stream(acquired.value().lease_id,
                                          acquired.value().nonce));
        stream_control.detach_errors.push_back(Error::DISCONNECTED);
        const auto released = service.release(2U, acquired.value().lease_id);
        TUNER_CHECK(!released && released.error() == Error::DISCONNECTED);
        TUNER_CHECK((backend.disconnected_receivers ==
                     std::vector<std::uint8_t>{2U}));
        TUNER_CHECK(backend.capture_stop_receivers.empty() &&
                    backend.closed.empty());
    }

    // The capture-pin operation is another independent transport-loss source.
    {
        FakeBackend backend;
        Nonce nonce;
        Clock clock;
        TunerService service(backend, nonce, clock);
        const auto acquired = service.acquire(3U, 3U);
        TUNER_CHECK(acquired &&
                    service.tune(3U, terrestrial(acquired.value().lease_id)) &&
                    service.start_stream(3U, acquired.value().lease_id) &&
                    service.attach_stream(acquired.value().lease_id,
                                          acquired.value().nonce));
        backend.capture_stop_failures.push_back(Error::DISCONNECTED);
        const auto released = service.release(3U, acquired.value().lease_id);
        TUNER_CHECK(!released && released.error() == Error::DISCONNECTED);
        TUNER_CHECK((backend.disconnected_receivers ==
                     std::vector<std::uint8_t>{3U}));
        TUNER_CHECK(backend.closed.empty());
    }

    // A disconnect while raising LNB power itself is terminal and is not
    // followed by a transactional rollback write.
    {
        FakeBackend backend;
        backend.allow_lnb_15v = true;
        Nonce nonce;
        Clock clock;
        TunerService service(backend, nonce, clock);
        const auto acquired = service.acquire(4U, 4U);
        TUNER_CHECK(acquired);
        auto request = satellite(acquired.value().lease_id);
        request.lnb_voltage = 15U;
        backend.power_begin_failures.push_back(Error::DISCONNECTED);
        const auto tuned = service.tune(4U, request);
        TUNER_CHECK(!tuned && tuned.error() == Error::DISCONNECTED);
        TUNER_CHECK((backend.disconnected_receivers ==
                     std::vector<std::uint8_t>{4U}));
        TUNER_CHECK(backend.power_rollbacks.empty());
        (void)service.release(4U, acquired.value().lease_id);
    }

    // A terminal data-plane snapshot is also transport-loss evidence even if
    // logical detach and capture-stop themselves completed successfully.
    {
        FakeBackend backend;
        FakeStreamControl stream_control;
        stream_control.final_snapshot_available = true;
        stream_control.final_snapshot_value.terminal =
            static_cast<std::uint8_t>(TunerStreamTerminal::disconnected);
        Nonce nonce;
        Clock clock;
        TunerService service(backend, nonce, clock, nullptr, nullptr,
                             &stream_control);
        const auto acquired = service.acquire(5U, 6U);
        TUNER_CHECK(acquired &&
                    service.tune(5U, terrestrial(acquired.value().lease_id)) &&
                    service.start_stream(5U, acquired.value().lease_id) &&
                    service.attach_stream(acquired.value().lease_id,
                                          acquired.value().nonce));
        const auto stopped = service.stop_stream(5U, acquired.value().lease_id);
        TUNER_CHECK(stopped && stopped.value().terminal ==
                                   static_cast<std::uint8_t>(
                                       TunerStreamTerminal::disconnected));
        TUNER_CHECK((backend.disconnected_receivers ==
                     std::vector<std::uint8_t>{6U}));
        TUNER_CHECK(service.status().value().receiver_states[6U] ==
                    ReceiverState::error);
        const auto released = service.release(5U, acquired.value().lease_id);
        TUNER_CHECK(!released && released.error() == Error::DISCONNECTED);
        TUNER_CHECK(backend.closed.empty());
    }
    return true;
}

bool test_deterministic_concurrent_leases()
{
    FakeBackend backend;
    Nonce nonce;
    Clock clock;
    TunerService service(backend, nonce, clock);
    std::array<std::uint64_t, kReceiverCount> lease_ids{};
    std::array<bool, kReceiverCount> succeeded{};
    std::array<std::thread, kReceiverCount> workers;
    for (std::uint8_t receiver = 0U; receiver < kReceiverCount; ++receiver) {
        workers[receiver] = std::thread([&, receiver]() {
            const auto acquired = service.acquire(receiver + 1U, receiver);
            if (!acquired) return;
            lease_ids[receiver] = acquired.value().lease_id;
            succeeded[receiver] = static_cast<bool>(
                service.release(receiver + 1U, lease_ids[receiver]));
        });
    }
    for (auto& worker : workers) worker.join();
    for (const bool result : succeeded) TUNER_CHECK(result);
    const auto status = service.status();
    TUNER_CHECK(status);
    for (const auto state : status.value().receiver_states)
        TUNER_CHECK(state == ReceiverState::free);
    return true;
}

bool test_pending_lease_id_reservation()
{
    ReservationBackend backend;
    Nonce nonce;
    Clock clock;
    LeaseIds ids;
    ids.values = {41U, 41U, 42U};
    TunerService service(backend, nonce, clock, &ids);
    bool first_ok = false;
    bool second_ok = false;
    std::uint64_t first_id = 0U;
    std::uint64_t second_id = 0U;
    std::thread first([&]() {
        const auto result = service.acquire(1U, 0U);
        if (result) {
            first_ok = true;
            first_id = result.value().lease_id;
        }
    });
    std::thread second([&]() {
        const auto result = service.acquire(2U, 1U);
        if (result) {
            second_ok = true;
            second_id = result.value().lease_id;
        }
    });
    for (std::size_t count = 0U; count < 100000U &&
                                 backend.open_count.load() < 2U; ++count)
        std::this_thread::yield();
    if (backend.open_count.load() != 2U) {
        backend.allow_open.store(true);
        first.join();
        second.join();
        std::fprintf(stderr, "tuner service reservation test did not start both opens\n");
        return false;
    }
    backend.allow_open.store(true);
    first.join();
    second.join();
    TUNER_CHECK(first_ok && second_ok && first_id != second_id &&
                first_id != 0U && second_id != 0U);
    TUNER_CHECK(service.release(1U, first_id));
    TUNER_CHECK(service.release(2U, second_id));
    return true;
}

bool test_destructor_releases_active_leases()
{
    FakeBackend backend;
    Nonce nonce;
    Clock clock;
    {
        TunerService service(backend, nonce, clock);
        TUNER_CHECK(service.acquire(1U, 3U));
    }
    TUNER_CHECK(backend.closed.size() == 1U && backend.closed[0] == 3U);
    return true;
}

bool test_owner_release_cleanup_and_shutdown()
{
    FakeBackend backend;
    Nonce nonce;
    Clock clock;
    TunerService service(backend, nonce, clock);
    const auto first = service.acquire(1U, 0U);
    const auto second = service.acquire(1U, 1U);
    const auto other = service.acquire(2U, 2U);
    TUNER_CHECK(first && second && other);
    TUNER_CHECK(service.release(2U, first.value().lease_id).error() == Error::NOT_FOUND);
    TUNER_CHECK(service.release(1U, first.value().lease_id));
    TUNER_CHECK(service.release(1U, first.value().lease_id).error() == Error::NOT_FOUND);

    backend.close_failures.push_back(Error::USB_IO);
    const auto disconnected = service.disconnect_client(1U);
    TUNER_CHECK(!disconnected && disconnected.error() == Error::USB_IO);
    TUNER_CHECK(service.status().value().receiver_states[1] == ReceiverState::free);
    TUNER_CHECK(service.release(1U, second.value().lease_id).error() == Error::NOT_FOUND);

    backend.close_failures.push_back(Error::TIMEOUT);
    const auto shutdown = service.shutdown();
    TUNER_CHECK(!shutdown && shutdown.error() == Error::TIMEOUT);
    TUNER_CHECK(service.status().value().receiver_states[2] == ReceiverState::free);
    TUNER_CHECK(service.shutdown());
    return true;
}

bool test_stream_control_failure_and_cleanup_contract()
{
    FakeBackend backend;
    FakeStreamControl stream_control;
    Nonce nonce;
    Clock clock;
    std::vector<int> sequence;
    backend.sequence = &sequence;
    stream_control.sequence = &sequence;
    TunerService service(backend, nonce, clock, nullptr, nullptr, &stream_control);

    const auto acquired = service.acquire(1U, 2U);
    TUNER_CHECK(acquired && service.tune(1U, terrestrial(acquired.value().lease_id)) &&
                service.start_stream(1U, acquired.value().lease_id));
    const auto attached = service.attach_stream(acquired.value().lease_id,
                                                acquired.value().nonce);
    TUNER_CHECK(attached && stream_control.attachments.size() == 1U);
    sequence.clear();
    TUNER_CHECK(service.release(1U, acquired.value().lease_id));
    TUNER_CHECK(sequence.size() == 3U && sequence[0] == 2 && sequence[1] == 4 &&
                sequence[2] == 5);
    TUNER_CHECK(stream_control.detached.size() == 1U &&
                stream_control.detached[0].attachment_id ==
                    attached.value().attachment_id);

    FakeBackend failed_backend;
    FakeStreamControl failed_stream;
    Nonce failed_nonce;
    Clock failed_clock;
    TunerService failed(failed_backend, failed_nonce, failed_clock,
                        nullptr, nullptr, &failed_stream);
    const auto failed_lease = failed.acquire(2U, 3U);
    TUNER_CHECK(failed_lease &&
                failed.tune(2U, terrestrial(failed_lease.value().lease_id)) &&
                failed.start_stream(2U, failed_lease.value().lease_id));
    failed_stream.attach_error = Error::USB_IO;
    failed_stream.detach_errors.push_back(Error::TIMEOUT);
    const auto attach_failed = failed.attach_stream(
        failed_lease.value().lease_id, failed_lease.value().nonce);
    TUNER_CHECK(!attach_failed && attach_failed.error() == Error::USB_IO);
    TUNER_CHECK(failed.status().value().receiver_states[3] == ReceiverState::error);
    TUNER_CHECK(failed_stream.detached.size() == 1U &&
                failed_stream.detached[0].attachment_id != 0U);
    const std::uint64_t retained_id = failed_stream.detached[0].attachment_id;
    TUNER_CHECK(failed.release(2U, failed_lease.value().lease_id));
    TUNER_CHECK(failed_stream.detached.size() == 2U &&
                failed_stream.detached[1].attachment_id == retained_id);
    TUNER_CHECK(failed_backend.closed.size() == 1U &&
                failed_backend.closed[0] == 3U);
    return true;
}

bool test_stream_authorization_and_mapping()
{
    FakeBackend backend;
    FakeStreamControl stream_control;
    stream_control.stats_available = true;
    stream_control.stats_value = TunerStreamCounters{11U, 2068U, 2U, 3U,
                                                    4U, 5U, 6U, 7U};
    stream_control.final_snapshot_available = true;
    stream_control.final_snapshot_value = TunerStreamFinalSnapshot{
        stream_control.stats_value, 0U};
    Nonce nonce;
    Clock clock;
    TunerService service(backend, nonce, clock, nullptr, nullptr, &stream_control);
    const auto terrestrial_lease = service.acquire(10U, 2U);
    const auto satellite_lease = service.acquire(11U, 0U);
    TUNER_CHECK(terrestrial_lease && satellite_lease);
    TUNER_CHECK(service.start_stream(10U, terrestrial_lease.value().lease_id)
                    .error() == Error::NOT_READY);
    TUNER_CHECK(service.tune(10U, terrestrial(terrestrial_lease.value().lease_id)));
    TUNER_CHECK(service.tune(11U, satellite(satellite_lease.value().lease_id)));

    TUNER_CHECK(service.start_stream(10U, terrestrial_lease.value().lease_id));
    TUNER_CHECK(service.status().value().receiver_states[2] == ReceiverState::tuned);
    auto wrong_nonce = terrestrial_lease.value().nonce;
    wrong_nonce[0] ^= 1U;
    TUNER_CHECK(service.attach_stream(terrestrial_lease.value().lease_id,
                                      wrong_nonce).error() == Error::NOT_FOUND);
    const auto attached = service.attach_stream(terrestrial_lease.value().lease_id,
                                                terrestrial_lease.value().nonce);
    TUNER_CHECK(attached);
    TUNER_CHECK(attached.value().owner_client_id == 10U &&
                attached.value().lease_id == terrestrial_lease.value().lease_id &&
                attached.value().attachment_id != 0U &&
                attached.value().receiver == 2U &&
                attached.value().system == System::ISDB_T);
    TUNER_CHECK(service.status().value().receiver_states[2] == ReceiverState::streaming);
    const auto live_stats = service.stats(10U, terrestrial_lease.value().lease_id);
    TUNER_CHECK(live_stats && live_stats.value().packets == 11U &&
                live_stats.value().continuity_errors == 4U);
    TUNER_CHECK(service.attach_stream(terrestrial_lease.value().lease_id,
                                      terrestrial_lease.value().nonce).error() ==
                Error::NOT_FOUND);
    TUNER_CHECK(service.tune(10U, terrestrial(terrestrial_lease.value().lease_id))
                    .error() == Error::BUSY);
    TUNER_CHECK(backend.capture_receivers.back() == 2U &&
                backend.capture_systems.back() == System::ISDB_T);
    const auto stopped = service.stop_stream(10U, terrestrial_lease.value().lease_id);
    TUNER_CHECK(stopped && stopped.value().counters.packets == 11U &&
                stopped.value().counters.bytes == 2068U);
    TUNER_CHECK(service.status().value().receiver_states[2] == ReceiverState::tuned);
    TUNER_CHECK(service.detach_stream(attached.value()).error() == Error::NOT_FOUND);
    // START_STREAM without a retune is still BUSY on a consumed lease.
    TUNER_CHECK(service.start_stream(10U, terrestrial_lease.value().lease_id)
                    .error() == Error::BUSY);
    // A successful retune re-arms the same lease; the ACQUIRE nonce stays the
    // same but each arm accepts exactly one attachment.
    TUNER_CHECK(service.tune(10U, terrestrial(terrestrial_lease.value().lease_id)));
    TUNER_CHECK(service.start_stream(10U, terrestrial_lease.value().lease_id));
    TUNER_CHECK(service.attach_stream(terrestrial_lease.value().lease_id,
                                      wrong_nonce).error() == Error::NOT_FOUND);
    const auto reattached = service.attach_stream(
        terrestrial_lease.value().lease_id, terrestrial_lease.value().nonce);
    TUNER_CHECK(reattached);
    TUNER_CHECK(reattached.value().attachment_id != 0U &&
                reattached.value().attachment_id != attached.value().attachment_id);
    TUNER_CHECK(service.status().value().receiver_states[2] == ReceiverState::streaming);
    TUNER_CHECK(service.stop_stream(10U, terrestrial_lease.value().lease_id));

    TUNER_CHECK(service.start_stream(11U, satellite_lease.value().lease_id));
    const auto satellite_attached = service.attach_stream(
        satellite_lease.value().lease_id, satellite_lease.value().nonce);
    TUNER_CHECK(satellite_attached);
    TUNER_CHECK(backend.capture_receivers.back() == 0U &&
                backend.capture_systems.back() == System::ISDB_S);
    TUNER_CHECK(service.detach_stream(satellite_attached.value()));
    TUNER_CHECK(service.release(10U, terrestrial_lease.value().lease_id));
    TUNER_CHECK(service.release(11U, satellite_lease.value().lease_id));
    return true;
}

bool test_stop_stream_requires_exact_final_snapshot()
{
    FakeBackend backend;
    FakeStreamControl stream_control;
    Nonce nonce;
    Clock clock;
    TunerService service(backend, nonce, clock, nullptr, nullptr,
                         &stream_control);
    const auto acquired = service.acquire(1U, 2U);
    TUNER_CHECK(acquired && service.tune(1U, terrestrial(acquired.value().lease_id)) &&
                service.start_stream(1U, acquired.value().lease_id));
    const auto attached = service.attach_stream(acquired.value().lease_id,
                                                acquired.value().nonce);
    TUNER_CHECK(attached);

    // The physical detach succeeds, but this fake deliberately has no exact
    // retained snapshot.  STOP must not fabricate an all-zero success.
    const auto stopped = service.stop_stream(1U, acquired.value().lease_id);
    TUNER_CHECK(!stopped && stopped.error() == Error::NOT_FOUND);
    TUNER_CHECK(stream_control.detached.size() == 1U &&
                backend.capture_stop_receivers.size() == 1U);
    TUNER_CHECK(service.status().value().receiver_states[2] == ReceiverState::tuned);
    TUNER_CHECK(service.release(1U, acquired.value().lease_id));
    return true;
}

bool test_stream_expiry_and_wrap()
{
    FakeBackend backend;
    Nonce nonce;
    Clock clock;
    TunerService service(backend, nonce, clock);
    const auto first = service.acquire(1U, 2U);
    TUNER_CHECK(first && service.tune(1U, terrestrial(first.value().lease_id)));
    TUNER_CHECK(service.start_stream(1U, first.value().lease_id));
    clock.now = 4999U;
    TUNER_CHECK(service.attach_stream(first.value().lease_id, first.value().nonce));
    TUNER_CHECK(service.stop_stream(1U, first.value().lease_id));
    TUNER_CHECK(service.release(1U, first.value().lease_id));

    const auto exact = service.acquire(2U, 3U);
    TUNER_CHECK(exact && service.tune(2U, terrestrial(exact.value().lease_id)));
    clock.now = 10000U;
    TUNER_CHECK(service.start_stream(2U, exact.value().lease_id));
    clock.now = 15000U;
    TUNER_CHECK(service.attach_stream(exact.value().lease_id, exact.value().nonce)
                    .error() == Error::TIMEOUT);
    TUNER_CHECK(service.release(2U, exact.value().lease_id));

    clock.now = std::numeric_limits<std::uint64_t>::max() - 100U;
    const auto wrapped = service.acquire(3U, 4U);
    TUNER_CHECK(wrapped && service.tune(3U, satellite(wrapped.value().lease_id)));
    TUNER_CHECK(service.start_stream(3U, wrapped.value().lease_id));
    clock.now = 4898U;
    TUNER_CHECK(service.attach_stream(wrapped.value().lease_id, wrapped.value().nonce));
    TUNER_CHECK(service.stop_stream(3U, wrapped.value().lease_id));
    TUNER_CHECK(service.release(3U, wrapped.value().lease_id));
    return true;
}

bool test_stream_failure_and_error_state()
{
    FakeBackend backend;
    Nonce nonce;
    Clock clock;
    TunerService service(backend, nonce, clock);
    const auto failed_start = service.acquire(1U, 2U);
    TUNER_CHECK(failed_start && service.tune(1U, terrestrial(failed_start.value().lease_id)));
    TUNER_CHECK(service.start_stream(1U, failed_start.value().lease_id));
    backend.capture_start_failures.push_back(Error::USB_IO);
    const auto attach_failed = service.attach_stream(failed_start.value().lease_id,
                                                     failed_start.value().nonce);
    TUNER_CHECK(!attach_failed && attach_failed.error() == Error::USB_IO);
    TUNER_CHECK(service.status().value().receiver_states[2] == ReceiverState::tuned);
    TUNER_CHECK(service.start_stream(1U, failed_start.value().lease_id)
                    .error() == Error::BUSY);
    TUNER_CHECK(service.release(1U, failed_start.value().lease_id));

    const auto failed_compensation = service.acquire(4U, 4U);
    TUNER_CHECK(failed_compensation &&
                service.tune(4U, satellite(failed_compensation.value().lease_id)) &&
                service.start_stream(4U, failed_compensation.value().lease_id));
    backend.capture_start_failures.push_back(Error::USB_IO);
    backend.capture_stop_failures.push_back(Error::TIMEOUT);
    TUNER_CHECK(service.attach_stream(
                    failed_compensation.value().lease_id,
                    failed_compensation.value().nonce).error() == Error::USB_IO);
    TUNER_CHECK(service.status().value().receiver_states[4] == ReceiverState::error);
    TUNER_CHECK(service.release(4U, failed_compensation.value().lease_id));

    const auto stop_failed = service.acquire(2U, 3U);
    TUNER_CHECK(stop_failed && service.tune(2U, terrestrial(stop_failed.value().lease_id)));
    TUNER_CHECK(service.start_stream(2U, stop_failed.value().lease_id));
    const auto stop_attachment = service.attach_stream(stop_failed.value().lease_id,
                                                       stop_failed.value().nonce);
    TUNER_CHECK(stop_attachment);
    backend.capture_stop_failures.push_back(Error::USB_IO);
    TUNER_CHECK(service.stop_stream(2U, stop_failed.value().lease_id).error() ==
                Error::USB_IO);
    TUNER_CHECK(service.status().value().receiver_states[3] == ReceiverState::error);
    TUNER_CHECK(service.tune(2U, terrestrial(stop_failed.value().lease_id)).error() ==
                Error::NOT_READY);
    TUNER_CHECK(service.release(2U, stop_failed.value().lease_id));
    TUNER_CHECK(service.status().value().receiver_states[3] == ReceiverState::free);

    const auto detach_failed = service.acquire(3U, 6U);
    TUNER_CHECK(detach_failed && service.tune(3U, terrestrial(detach_failed.value().lease_id)));
    TUNER_CHECK(service.start_stream(3U, detach_failed.value().lease_id));
    const auto detach_attachment = service.attach_stream(
        detach_failed.value().lease_id, detach_failed.value().nonce);
    TUNER_CHECK(detach_attachment);
    backend.capture_stop_failures.push_back(Error::USB_IO);
    TUNER_CHECK(service.detach_stream(detach_attachment.value()).error() == Error::USB_IO);
    TUNER_CHECK(service.status().value().receiver_states[6] == ReceiverState::error);
    TUNER_CHECK(service.release(3U, detach_failed.value().lease_id));
    TUNER_CHECK(service.status().value().receiver_states[6] == ReceiverState::free);
    return true;
}

bool test_stream_revoke_compensation_and_destructor_cleanup()
{
    FakeBackend backend;
    Nonce nonce;
    Clock clock;
    {
        TunerService service(backend, nonce, clock);
        const auto acquired = service.acquire(1U, 0U);
        TUNER_CHECK(acquired && service.tune(1U, satellite(acquired.value().lease_id)));
        TUNER_CHECK(service.start_stream(1U, acquired.value().lease_id));
        backend.block_capture_start = true;
        backend.capture_stop_failures.push_back(Error::USB_IO);
        bool attach_ok = false;
        std::thread attach([&]() {
            const auto result = service.attach_stream(acquired.value().lease_id,
                                                      acquired.value().nonce);
            attach_ok = static_cast<bool>(result);
        });
        bool entered = false;
        {
            std::unique_lock<std::mutex> lock(backend.mutex);
            entered = backend.capture_cv.wait_for(
                lock, std::chrono::seconds(5), [&]() noexcept {
                    return backend.capture_start_entered;
                });
        }
        const auto revoked = service.revoke_stream(1U, acquired.value().lease_id);
        {
            std::lock_guard<std::mutex> lock(backend.mutex);
            backend.allow_capture_start = true;
            backend.capture_cv.notify_all();
        }
        attach.join();
        TUNER_CHECK(entered && revoked && !attach_ok);
        TUNER_CHECK(service.status().value().receiver_states[0] == ReceiverState::error);
        TUNER_CHECK(backend.operations.size() >= 2U &&
                    backend.operations[backend.operations.size() - 1U] ==
                        FakeBackend::Operation::stop_capture);
        TUNER_CHECK(service.release(1U, acquired.value().lease_id));
    }

    const std::size_t stop_count = backend.capture_stop_receivers.size();
    {
        TunerService service(backend, nonce, clock);
        const auto acquired = service.acquire(2U, 2U);
        TUNER_CHECK(acquired && service.tune(2U, terrestrial(acquired.value().lease_id)));
        TUNER_CHECK(service.start_stream(2U, acquired.value().lease_id));
        TUNER_CHECK(service.attach_stream(acquired.value().lease_id,
                                          acquired.value().nonce));
    }
    TUNER_CHECK(backend.capture_stop_receivers.size() == stop_count + 1U);
    TUNER_CHECK(backend.closed.back() == 2U);
    return true;
}

bool test_stream_control_races_and_retry_identity()
{
    {
        FakeBackend backend;
        FakeStreamControl stream_control;
        Nonce nonce;
        Clock clock;
        std::vector<int> sequence;
        backend.sequence = &sequence;
        stream_control.sequence = &sequence;
        TunerService service(backend, nonce, clock, nullptr, nullptr,
                             &stream_control);
        const auto acquired = service.acquire(1U, 0U);
        TUNER_CHECK(acquired && service.tune(1U, satellite(acquired.value().lease_id)) &&
                    service.start_stream(1U, acquired.value().lease_id));
        stream_control.block_attach = true;
        std::atomic<bool> attach_done{false};
        bool attach_ok = false;
        std::thread attach([&]() {
            const auto result = service.attach_stream(
                acquired.value().lease_id, acquired.value().nonce);
            attach_ok = static_cast<bool>(result);
            attach_done.store(true);
        });
        if (!stream_control.wait_attach_entered()) {
            stream_control.allow_blocked_attach();
            attach.join();
            return false;
        }
        const auto revoked = service.revoke_stream(1U, acquired.value().lease_id);
        std::atomic<bool> release_done{false};
        bool release_ok = false;
        std::thread release([&]() {
            const auto result = service.release(1U, acquired.value().lease_id);
            release_ok = static_cast<bool>(result);
            release_done.store(true);
        });
        {
            std::lock_guard<std::mutex> lock(stream_control.mutex);
            stream_control.allow_attach = true;
            stream_control.changed.notify_all();
        }
        attach.join();
        release.join();
        TUNER_CHECK(revoked && !attach_ok && attach_done.load() && release_ok &&
                    release_done.load());
        TUNER_CHECK(stream_control.attachments.size() == 1U &&
                    stream_control.detached.size() == 1U &&
                    backend.capture_stop_receivers.size() == 1U &&
                    backend.closed.size() == 1U);
        const auto detach_position =
            std::find(sequence.begin(), sequence.end(), 2);
        const auto stop_position =
            std::find(sequence.begin(), sequence.end(), 4);
        const auto close_position =
            std::find(sequence.begin(), sequence.end(), 5);
        TUNER_CHECK(detach_position != sequence.end() &&
                    stop_position != sequence.end() && close_position != sequence.end() &&
                    detach_position < stop_position && stop_position < close_position);
        TUNER_CHECK(service.status().value().receiver_states[0] == ReceiverState::free);
    }

    {
        FakeBackend backend;
        FakeStreamControl stream_control;
        Nonce nonce;
        Clock clock;
        TunerService service(backend, nonce, clock, nullptr, nullptr,
                             &stream_control);
        const auto acquired = service.acquire(1U, 2U);
        TUNER_CHECK(acquired && service.tune(1U, terrestrial(acquired.value().lease_id)) &&
                    service.start_stream(1U, acquired.value().lease_id));
        const auto attached = service.attach_stream(acquired.value().lease_id,
                                                    acquired.value().nonce);
        TUNER_CHECK(attached);
        std::atomic<bool> stop_done{false};
        std::atomic<bool> release_done{false};
        std::thread stop([&]() {
            (void)service.stop_stream(1U, acquired.value().lease_id);
            stop_done.store(true);
        });
        std::thread release([&]() {
            (void)service.release(1U, acquired.value().lease_id);
            release_done.store(true);
        });
        stop.join();
        release.join();
        TUNER_CHECK(stop_done.load() && release_done.load());
        TUNER_CHECK(stream_control.detached.size() == 1U &&
                    backend.capture_stop_receivers.size() == 1U &&
                    backend.closed.size() <= 1U);
        TUNER_CHECK(service.status().value().receiver_states[2] == ReceiverState::free);
    }

    {
        FakeBackend backend;
        FakeStreamControl stream_control;
        Nonce nonce;
        Clock clock;
        TunerService service(backend, nonce, clock, nullptr, nullptr,
                             &stream_control);
        const auto acquired = service.acquire(1U, 3U);
        TUNER_CHECK(acquired && service.tune(1U, terrestrial(acquired.value().lease_id)) &&
                    service.start_stream(1U, acquired.value().lease_id));
        const auto attached = service.attach_stream(acquired.value().lease_id,
                                                    acquired.value().nonce);
        TUNER_CHECK(attached);
        stream_control.detach_errors.push_back(Error::USB_IO);
        backend.capture_stop_failures.push_back(Error::TIMEOUT);
        const auto stopped = service.stop_stream(1U, acquired.value().lease_id);
        TUNER_CHECK(!stopped && stopped.error() == Error::USB_IO);
        TUNER_CHECK(service.status().value().receiver_states[3] == ReceiverState::error);
        const auto released = service.release(1U, acquired.value().lease_id);
        TUNER_CHECK(released);
        TUNER_CHECK(stream_control.detached.size() == 2U &&
                    stream_control.detached[0].attachment_id ==
                        stream_control.detached[1].attachment_id &&
                    backend.capture_stop_receivers.size() == 2U &&
                    service.status().value().receiver_states[3] == ReceiverState::free);
    }
    return true;
}

bool test_attachment_id_collision_and_stale_identity()
{
    FakeBackend backend;
    Nonce nonce;
    Clock clock;
    AttachmentIds ids;
    ids.values = {77U, 77U, 78U};
    TunerService service(backend, nonce, clock, nullptr, &ids);
    const auto first = service.acquire(1U, 2U);
    const auto second = service.acquire(2U, 3U);
    TUNER_CHECK(first && second);
    TUNER_CHECK(service.tune(1U, terrestrial(first.value().lease_id)) &&
                service.tune(2U, terrestrial(second.value().lease_id)));
    TUNER_CHECK(service.start_stream(1U, first.value().lease_id));
    const auto first_attachment = service.attach_stream(first.value().lease_id,
                                                        first.value().nonce);
    TUNER_CHECK(first_attachment && first_attachment.value().attachment_id == 77U);
    TUNER_CHECK(service.start_stream(2U, second.value().lease_id));
    const auto second_attachment = service.attach_stream(second.value().lease_id,
                                                         second.value().nonce);
    TUNER_CHECK(second_attachment && second_attachment.value().attachment_id == 78U);
    TUNER_CHECK(service.detach_stream(first_attachment.value()));
    TUNER_CHECK(service.release(1U, first.value().lease_id));
    TUNER_CHECK(service.detach_stream(first_attachment.value()).error() == Error::NOT_FOUND);
    TUNER_CHECK(service.detach_stream(second_attachment.value()));
    TUNER_CHECK(service.release(2U, second.value().lease_id));

    FakeBackend reused_backend;
    Nonce reused_nonce;
    Clock reused_clock;
    LeaseIds reused_lease_ids;
    AttachmentIds reused_attachment_ids;
    reused_lease_ids.values = {42U, 42U};
    reused_attachment_ids.values = {91U, 92U};
    TunerService reused(reused_backend, reused_nonce, reused_clock,
                        &reused_lease_ids, &reused_attachment_ids);
    const auto old_lease = reused.acquire(3U, 5U);
    TUNER_CHECK(old_lease && reused.tune(3U, satellite(old_lease.value().lease_id)) &&
                reused.start_stream(3U, old_lease.value().lease_id));
    const auto old_attachment = reused.attach_stream(old_lease.value().lease_id,
                                                     old_lease.value().nonce);
    TUNER_CHECK(old_attachment && reused.stop_stream(3U, old_lease.value().lease_id) &&
                reused.release(3U, old_lease.value().lease_id));
    const auto new_lease = reused.acquire(4U, 5U);
    TUNER_CHECK(new_lease && new_lease.value().lease_id == 42U &&
                reused.tune(4U, satellite(new_lease.value().lease_id)) &&
                reused.start_stream(4U, new_lease.value().lease_id));
    const auto new_attachment = reused.attach_stream(new_lease.value().lease_id,
                                                     new_lease.value().nonce);
    TUNER_CHECK(new_attachment && new_attachment.value().attachment_id == 92U);
    TUNER_CHECK(reused.detach_stream(old_attachment.value()).error() == Error::NOT_FOUND);
    TUNER_CHECK(reused.detach_stream(new_attachment.value()) &&
                reused.release(4U, new_lease.value().lease_id));
    return true;
}

bool test_connection_revoke_and_release_ordering()
{
    {
        FakeBackend backend;
        Nonce nonce;
        Clock clock;
        TunerService service(backend, nonce, clock);
        const auto first = service.acquire(1U, 0U);
        const auto second = service.acquire(1U, 1U);
        TUNER_CHECK(first && second &&
                    service.tune(1U, satellite(first.value().lease_id)) &&
                    service.tune(1U, satellite(second.value().lease_id)) &&
                    service.start_stream(1U, first.value().lease_id) &&
                    service.start_stream(1U, second.value().lease_id));
        backend.block_close = true;
        backend.blocked_close_receiver = 0U;
        std::atomic<bool> cleanup_done{false};
        std::thread cleanup([&]() {
            (void)service.disconnect_client(1U);
            cleanup_done.store(true);
        });
        bool close_entered = false;
        {
            std::unique_lock<std::mutex> lock(backend.mutex);
            close_entered = backend.capture_cv.wait_for(
                lock, std::chrono::seconds(5), [&]() noexcept {
                    return backend.close_entered;
                });
        }
        const auto attach_after_revoke = service.attach_stream(
            second.value().lease_id, second.value().nonce);
        {
            std::lock_guard<std::mutex> lock(backend.mutex);
            backend.allow_close = true;
            backend.capture_cv.notify_all();
        }
        cleanup.join();
        TUNER_CHECK(close_entered && cleanup_done.load() &&
                    !attach_after_revoke &&
                    attach_after_revoke.error() == Error::NOT_FOUND);
        TUNER_CHECK(service.status().value().receiver_states[0] == ReceiverState::free &&
                    service.status().value().receiver_states[1] == ReceiverState::free);
    }

    {
        FakeBackend backend;
        Nonce nonce;
        Clock clock;
        TunerService service(backend, nonce, clock);
        const auto acquired = service.acquire(1U, 2U);
        TUNER_CHECK(acquired && service.tune(1U, terrestrial(acquired.value().lease_id)) &&
                    service.start_stream(1U, acquired.value().lease_id));
        backend.block_close = true;
        backend.blocked_close_receiver = 2U;
        std::atomic<bool> release_ok{false};
        std::thread release([&]() {
            release_ok.store(static_cast<bool>(
                service.release(1U, acquired.value().lease_id)));
        });
        bool close_entered = false;
        {
            std::unique_lock<std::mutex> lock(backend.mutex);
            close_entered = backend.capture_cv.wait_for(
                lock, std::chrono::seconds(5), [&]() noexcept {
                    return backend.close_entered;
                });
        }
        std::atomic<bool> attach_ok{false};
        std::thread attach([&]() {
            attach_ok.store(static_cast<bool>(service.attach_stream(
                acquired.value().lease_id, acquired.value().nonce)));
        });
        {
            std::lock_guard<std::mutex> lock(backend.mutex);
            backend.allow_close = true;
            backend.capture_cv.notify_all();
        }
        release.join();
        attach.join();
        TUNER_CHECK(close_entered && release_ok.load() && !attach_ok.load());
        TUNER_CHECK(backend.capture_receivers.empty());
    }

    {
        FakeBackend backend;
        Nonce nonce;
        Clock clock;
        TunerService service(backend, nonce, clock);
        const auto acquired = service.acquire(1U, 3U);
        TUNER_CHECK(acquired && service.tune(1U, terrestrial(acquired.value().lease_id)) &&
                    service.start_stream(1U, acquired.value().lease_id));
        const auto attachment = service.attach_stream(acquired.value().lease_id,
                                                       acquired.value().nonce);
        TUNER_CHECK(attachment);
        backend.block_close = true;
        backend.blocked_close_receiver = 3U;
        std::atomic<bool> release_ok{false};
        std::thread release([&]() {
            release_ok.store(static_cast<bool>(
                service.release(1U, acquired.value().lease_id)));
        });
        bool close_entered = false;
        {
            std::unique_lock<std::mutex> lock(backend.mutex);
            close_entered = backend.capture_cv.wait_for(
                lock, std::chrono::seconds(5), [&]() noexcept {
                    return backend.close_entered;
                });
        }
        std::atomic<bool> stop_ok{false};
        std::thread stop([&]() {
            stop_ok.store(static_cast<bool>(
                service.stop_stream(1U, acquired.value().lease_id)));
        });
        {
            std::lock_guard<std::mutex> lock(backend.mutex);
            backend.allow_close = true;
            backend.capture_cv.notify_all();
        }
        release.join();
        stop.join();
        TUNER_CHECK(close_entered && release_ok.load() && !stop_ok.load());
        TUNER_CHECK(backend.capture_stop_receivers.size() == 1U);
    }
    return true;
}

}  // namespace

bool run_tuner_service_tests()
{
    return test_mapping_exclusive_and_generation() &&
           test_all_receiver_system_mapping() &&
           test_acquire_failures_and_lease_collision() &&
           test_tune_parameters_and_operation_order() &&
           test_tune_uses_one_end_to_end_budget() &&
           test_tune_validation_lock_timeout_and_retune() &&
           test_tune_failures_are_recoverable() &&
           test_lnb_tune_transaction_and_failure_rollback() &&
           test_disconnect_reaches_backend_power_authority() &&
           test_deterministic_concurrent_leases() &&
           test_pending_lease_id_reservation() &&
           test_destructor_releases_active_leases() &&
           test_owner_release_cleanup_and_shutdown() &&
           test_stream_control_failure_and_cleanup_contract() &&
           test_stream_authorization_and_mapping() &&
           test_stop_stream_requires_exact_final_snapshot() &&
           test_stream_expiry_and_wrap() &&
           test_stream_failure_and_error_state() &&
           test_stream_revoke_compensation_and_destructor_cleanup() &&
           test_attachment_id_collision_and_stale_identity() &&
           test_connection_revoke_and_release_ordering() &&
           test_stream_control_races_and_retry_identity();
}
