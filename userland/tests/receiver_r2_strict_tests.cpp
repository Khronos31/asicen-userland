// SPDX-License-Identifier: GPL-2.0-only
//
// ASICEN R2 strict admission through the real control worker lane.  The
// ASICEN LNB commit consumes its transaction (a later rollback returns a
// no-op SUCCESS), so a late or cancelled success after commit must quarantine
// until the exact release/owner cleanup rather than claim compensation.
#include "control_workers.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace px4::userland;
using namespace px4::userland::ipc;
using namespace px4::userland::ipc::posix;

#define R2_CHECK(condition)                                                           \
    do {                                                                               \
        if (!(condition)) {                                                            \
            std::fprintf(stderr, "ASICEN R2 check failed at %d: %s\n", __LINE__,      \
                         #condition);                                                  \
            return false;                                                              \
        }                                                                              \
    } while (false)

constexpr std::chrono::milliseconds kWaitBound{10000};

template <typename F>
struct ScopeExit final {
    F fn;
    ~ScopeExit() noexcept { fn(); }
};

template <typename F>
ScopeExit<F> on_scope_exit(F fn)
{
    return ScopeExit<F>{fn};
}

class Gate final {
public:
    void arm(int receiver)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        receiver_ = receiver;
        entered_ = false;
        open_ = false;
    }
    void pass(int receiver)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (receiver_ != receiver) return;
        receiver_ = -1;
        entered_ = true;
        changed_.notify_all();
        changed_.wait_for(lock, kWaitBound, [&]() noexcept { return open_; });
    }
    bool wait_entered()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, kWaitBound, [&]() noexcept { return entered_; });
    }
    void open()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        open_ = true;
        receiver_ = -1;
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    int receiver_ = -1;
    bool entered_ = false;
    bool open_ = false;
};

class FakeCardBackend final : public CardServiceBackend {
public:
    Result<void> set_power(bool) noexcept override { return Result<void>::success(); }
    Result<void> initialize_uart() noexcept override { return Result<void>::success(); }
    Result<bool> detect_card() noexcept override { return Result<bool>::success(false); }
};

class FakeCardSession final : public CardProtocolSession {
public:
    Result<void> initialize() noexcept override { return Result<void>::success(); }
    Result<std::size_t> transmit(ByteView, MutableByteView) noexcept override
    {
        return Result<std::size_t>::failure(Error::UNSUPPORTED);
    }
    bool initialized() const noexcept override { return false; }
    const CardAtr& atr() const noexcept override { return atr_; }
    void invalidate() noexcept override {}

private:
    CardAtr atr_{};
};

class FakeNonce final : public TunerNonceSource {
public:
    Result<std::array<std::uint8_t, kNonceLength>> generate() noexcept override
    {
        entered_.store(true);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            changed_.notify_all();
        }
        std::unique_lock<std::mutex> lock(mutex_);
        if (gate_) changed_.wait_for(lock, kWaitBound, [&]() noexcept { return open_; });
        std::array<std::uint8_t, kNonceLength> value{};
        value[0U] = next_.fetch_add(1U);
        return Result<std::array<std::uint8_t, kNonceLength>>::success(value);
    }

    void arm() noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        entered_.store(false);
        open_ = false;
        gate_ = true;
    }
    bool wait_entered() noexcept
    {
        std::unique_lock<std::mutex> lock(mutex_);
        return changed_.wait_for(lock, kWaitBound,
                                 [&]() noexcept { return entered_.load(); });
    }
    void open() noexcept
    {
        std::lock_guard<std::mutex> lock(mutex_);
        open_ = true;
        gate_ = false;
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::atomic<bool> entered_{false};
    std::atomic<std::uint8_t> next_{1U};
    bool gate_ = false;
    bool open_ = false;
};

class FakeTime final : public TunerServiceTime {
public:
    std::uint64_t monotonic_ms() noexcept override { return now.load(); }
    void sleep_ms(std::uint32_t milliseconds) noexcept override
    {
        now.fetch_add(milliseconds);
    }
    std::atomic<std::uint64_t> now{0U};
};

// Faithful ASICEN-shaped LNB authority: a successful commit consumes the
// pending transaction, so a later rollback is a no-op SUCCESS that does not
// change the physical state.  The bridge power is reference counted.
class AsicenShapedBackend final : public TunerServiceBackend {
public:
    AsicenShapedBackend(FakeTime& time) noexcept : time_(time) {}

    std::uint8_t receiver_count() const noexcept override { return 2U; }
    bool receiver_supports(std::uint8_t receiver, System) const noexcept override
    {
        return receiver < 2U;
    }

    Result<void> open_receiver(std::uint8_t) noexcept override
    {
        return Result<void>::success();
    }
    Result<void> tune_terrestrial(std::uint8_t, std::uint32_t,
                                  std::uint32_t) noexcept override
    {
        return Result<void>::success();
    }
    Result<void> tune_satellite(std::uint8_t receiver, std::uint32_t,
                                std::uint32_t) noexcept override
    {
        tune_gate.pass(receiver);
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
    Result<void> close_receiver(std::uint8_t receiver) noexcept override
    {
        if (disconnected) return Result<void>::success();
        release(receiver);
        if (close_error != Error::OK) return Result<void>::failure(close_error);
        return Result<void>::success();
    }
    Result<void> begin_tune_power(std::uint8_t receiver, System system,
                                  std::uint8_t voltage) noexcept override
    {
        if (system != System::ISDB_S) return Result<void>::success();
        pending[receiver] = true;
        pending_15v[receiver] = voltage == 15U;
        apply();
        return Result<void>::success();
    }
    Result<void> commit_tune_power(std::uint8_t receiver) noexcept override
    {
        ++scenario;
        if (!pending[receiver]) return Result<void>::failure(Error::INVALID_ARGUMENT);
        committed_15v[receiver] = pending_15v[receiver];
        pending[receiver] = false;
        if (commit_wait_ms != 0U) time_.sleep_ms(commit_wait_ms);
        apply();
        return Result<void>::success();
    }
    Result<void> rollback_tune_power(std::uint8_t receiver) noexcept override
    {
        ++rollback_calls;
        // Pre-commit: drop the pending transaction and re-apply the prior
        // committed aggregate.  After a successful commit there is no pending
        // transaction left, so this returns SUCCESS without changing the
        // physical/logical ON state, matching the real ASICEN rollback.
        pending[receiver] = false;
        apply();
        return Result<void>::success();
    }
    void mark_receiver_disconnected(std::uint8_t) noexcept override
    {
        disconnected = true;
        physical_on = false;
        io_calls.clear();
    }
    Result<void> shutdown() noexcept override { return Result<void>::success(); }
    Result<void> start_capture(std::uint8_t, System) noexcept override
    {
        return Result<void>::success();
    }
    Result<void> stop_capture(std::uint8_t, System) noexcept override
    {
        return Result<void>::success();
    }

    std::uint8_t ref_count() const noexcept
    {
        std::uint8_t count = 0U;
        for (std::size_t index = 0U; index < 2U; ++index) {
            if (pending[index] ? pending_15v[index] : committed_15v[index]) ++count;
        }
        return count;
    }
    void release(std::uint8_t receiver) noexcept
    {
        pending[receiver] = false;
        committed_15v[receiver] = false;
        apply();
    }
    void apply() noexcept
    {
        const bool desired = ref_count() != 0U;
        if (desired == physical_on) return;
        physical_on = desired;
        io_calls.push_back(desired ? "on" : "off");
    }

    FakeTime& time_;
    std::array<bool, 2U> pending{{false, false}};
    std::array<bool, 2U> pending_15v{{false, false}};
    std::array<bool, 2U> committed_15v{{false, false}};
    Gate tune_gate;
    bool physical_on = false;
    bool disconnected = false;
    Error close_error = Error::OK;
    std::uint32_t commit_wait_ms = 0U;
    int rollback_calls = 0;
    int scenario = 0;
    std::vector<std::string> io_calls;
};

struct Fixture final {
    FakeCardBackend card_backend;
    FakeCardSession card_session{};
    CardService card{card_backend, card_session};
    FakeNonce nonce{};
    FakeTime time{};
    AsicenShapedBackend backend{time};
    TunerService service{backend, nonce, time};
};

TuneRequestPayload satellite_payload(std::uint64_t lease_id, std::uint8_t voltage = 15U,
                                     std::uint32_t timeout_ms = 1000U)
{
    TuneRequestPayload payload{};
    payload.lease_id = lease_id;
    payload.system = System::ISDB_S;
    payload.frequency_khz = 1049480U;
    payload.stream_id = 0xffffU;
    payload.slot = 0U;
    payload.bandwidth_hz = 0U;
    payload.lnb_voltage = voltage;
    payload.timeout_ms = timeout_ms;
    return payload;
}

ControlWorkerTask tune_task(std::uint64_t client, std::uint8_t receiver,
                            std::uint64_t lease_id, const TuneRequestPayload& payload,
                            std::uint64_t accepted_at_ms)
{
    ControlWorkerTask task{};
    task.operation = ControlWorkerOperation::tuner_tune;
    task.kind = ControlWorkerTaskKind::client_request;
    task.client_id = client;
    task.receiver = receiver;
    task.lease_id = lease_id;
    task.tune = payload;
    task.accepted_at_ms = accepted_at_ms;
    return task;
}

bool test_worker_strict_execution()
{
    Fixture f;
    R2_CHECK(static_cast<bool>(f.service.configure_strict_capabilities()));
    R2_CHECK(f.service.acquire(1U, 0U));
    auto workers = ControlWorkerLanes::create(f.card, f.service);
    R2_CHECK(workers);
    R2_CHECK(workers.value()->submit(
        ControlWorkerLane::tuner_dev1,
        tune_task(1U, 0U, 1U, satellite_payload(1U), 0U)));
    ControlWorkerCompletion completion{};
    const auto deadline = std::chrono::steady_clock::now() + kWaitBound;
    bool found = false;
    while (!found && std::chrono::steady_clock::now() < deadline) {
        while (workers.value()->try_pop(ControlWorkerLane::tuner_dev1, completion)) {
            if (completion.operation == ControlWorkerOperation::tuner_tune) {
                found = true;
                break;
            }
        }
        if (!found) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    R2_CHECK(found && completion.error == Error::OK);
    R2_CHECK(f.backend.committed_15v[0U]);
    R2_CHECK(workers.value()->stop_and_join());
    return true;
}

bool test_worker_signal_failure_aborts_once()
{
    Fixture f;
    R2_CHECK(static_cast<bool>(f.service.configure_strict_capabilities()));
    R2_CHECK(f.service.acquire(1U, 0U));
    ControlWorkerStartupOptions options{};
    options.fail_tune_signal = true;
    auto workers = ControlWorkerLanes::create_for_test(f.card, f.service, options);
    R2_CHECK(workers);
    const auto rejected = workers.value()->submit(
        ControlWorkerLane::tuner_dev1,
        tune_task(1U, 0U, 1U, satellite_payload(1U), 0U));
    R2_CHECK(!rejected && rejected.error() == Error::INTERNAL);
    R2_CHECK(f.backend.io_calls.empty() && !f.backend.committed_15v[0U]);
    ControlWorkerCompletion completion{};
    R2_CHECK(!workers.value()->try_pop(ControlWorkerLane::tuner_dev1, completion));
    R2_CHECK(workers.value()->stop_and_join());
    R2_CHECK(f.service.try_admit_tune(1U, satellite_payload(1U), 0U));
    return true;
}

bool test_worker_registry_contention_busy()
{
    Fixture f;
    R2_CHECK(static_cast<bool>(f.service.configure_strict_capabilities()));
    R2_CHECK(f.service.acquire(1U, 0U));
    auto workers = ControlWorkerLanes::create(f.card, f.service);
    R2_CHECK(workers);
    const auto before = f.service.tune_metadata(0U);
    R2_CHECK(before);
    Result<TunerAcquireResult> other = Result<TunerAcquireResult>::failure(Error::INTERNAL);
    Result<void> rejected = Result<void>::failure(Error::INTERNAL);
    {
        f.nonce.arm();
        std::thread accepter([&]() { other = f.service.acquire(2U, 1U); });
        const auto guard = on_scope_exit([&]() noexcept {
            f.nonce.open();
            if (accepter.joinable()) accepter.join();
        });
        R2_CHECK(f.nonce.wait_entered());
        rejected = workers.value()->submit(
            ControlWorkerLane::tuner_dev1,
            tune_task(1U, 0U, 1U, satellite_payload(1U), 0U));
    }
    // The metadata check runs after the registry lock is released.
    R2_CHECK(!rejected && rejected.error() == Error::BUSY);
    const auto after = f.service.tune_metadata(0U);
    R2_CHECK(after && after.value().revision == before.value().revision &&
             after.value().current_valid == before.value().current_valid);
    R2_CHECK(other);
    R2_CHECK(workers.value()->stop_and_join());
    return true;
}

// Synthetic ASICEN committed-power authority: pre-commit rollback restores the
// prior aggregate, a consumed commit makes rollback a no-op SUCCESS, and close
// can fail ambiguously.  This models the authority's committed semantics; it is
// not evidence of real multi-peer arbitration.
bool test_precommit_rollback_and_close_failure()
{
    Fixture f;
    R2_CHECK(static_cast<bool>(f.service.configure_strict_capabilities()));
    R2_CHECK(f.service.acquire(1U, 0U));

    // Pre-commit rejection: revocation during the finite tune stops the next
    // stage and the ordinary rollback restores the prior off aggregate.
    f.backend.tune_gate.arm(0);
    auto admitted = f.service.try_admit_tune(1U, satellite_payload(1U), 0U);
    R2_CHECK(admitted);
    Result<void> executed = Result<void>::failure(Error::INTERNAL);
    {
        std::thread worker([&]() {
            executed = f.service.execute_admitted_tune(admitted.value());
        });
        const auto guard = on_scope_exit([&]() noexcept {
            f.backend.tune_gate.open();
            if (worker.joinable()) worker.join();
        });
        R2_CHECK(f.backend.tune_gate.wait_entered());
        R2_CHECK(f.service.revoke_tune_ticket(1U, admitted.value().lease_id));
    }
    R2_CHECK(!executed && executed.error() == Error::NOT_FOUND);
    R2_CHECK(f.backend.rollback_calls == 1);
    R2_CHECK(!f.backend.physical_on && !f.backend.committed_15v[0U]);

    // Ambiguous strict close failure after a committed tune quarantines instead
    // of announcing free.
    f.time.now.store(0U);
    auto tuned = f.service.try_admit_tune(1U, satellite_payload(1U), 0U);
    R2_CHECK(tuned && f.service.execute_admitted_tune(tuned.value()));
    R2_CHECK(f.backend.committed_15v[0U]);
    f.backend.close_error = Error::USB_IO;
    const auto released = f.service.release(1U, 1U);
    R2_CHECK(!released && released.error() == Error::USB_IO);
    R2_CHECK(f.service.status().value().receiver_states[0U] == ReceiverState::error);
    return true;
}

// Committed-power no-rollback semantics and exact-release cleanup.  The
// two-reference aggregate is a synthetic model of the committed authority; it
// is not evidence of real ASICEN multi-peer arbitration, whose single-active
// ownership is covered by the existing hardware seam tests.
bool test_postcommit_quarantine_owner_close_and_disconnect()
{
    Fixture f;
    R2_CHECK(static_cast<bool>(f.service.configure_strict_capabilities()));
    R2_CHECK(f.service.acquire(1U, 0U));
    R2_CHECK(f.service.acquire(2U, 1U));

    auto first = f.service.try_admit_tune(1U, satellite_payload(1U), 0U);
    R2_CHECK(first && f.service.execute_admitted_tune(first.value()));
    auto peer = f.service.try_admit_tune(2U, satellite_payload(2U), 0U);
    R2_CHECK(peer && f.service.execute_admitted_tune(peer.value()));
    R2_CHECK(f.backend.ref_count() == 2U && f.backend.physical_on);

    // Late commit: the ASICEN rollback would be a no-op SUCCESS, so the engine
    // must not call it and must not infer a reusable state.
    f.time.now.store(0U);
    f.backend.commit_wait_ms = 2000U;
    auto late = f.service.try_admit_tune(1U, satellite_payload(1U), 0U);
    R2_CHECK(late);
    const auto result = f.service.execute_admitted_tune(late.value());
    R2_CHECK(!result && result.error() == Error::TIMEOUT);
    R2_CHECK(f.backend.rollback_calls == 0);
    const auto status = f.service.status();
    R2_CHECK(status && status.value().receiver_states[0U] == ReceiverState::error);
    R2_CHECK(!f.service.start_stream(1U, 1U));
    R2_CHECK(!f.service.try_admit_tune(1U, satellite_payload(1U), 0U));
    f.backend.commit_wait_ms = 0U;

    // Owner logical teardown drives the eventual close, preserving the peer.
    R2_CHECK(f.service.disconnect_client(1U));
    R2_CHECK(f.backend.ref_count() == 1U && f.backend.physical_on);
    R2_CHECK(f.service.disconnect_client(2U));
    R2_CHECK(f.backend.ref_count() == 0U && !f.backend.physical_on);

    // A terminal transport disconnect performs no further power I/O.
    f.time.now.store(0U);
    R2_CHECK(f.service.acquire(3U, 0U));
    auto tune = f.service.try_admit_tune(3U, satellite_payload(3U), 0U);
    R2_CHECK(tune && f.service.execute_admitted_tune(tune.value()));
    f.backend.io_calls.clear();
    f.backend.mark_receiver_disconnected(0U);
    (void)f.service.disconnect_client(3U);
    R2_CHECK(f.backend.io_calls.empty());
    return true;
}

}  // namespace

int main()
{
    return test_worker_strict_execution() &&
                   test_worker_signal_failure_aborts_once() &&
                   test_worker_registry_contention_busy() &&
                   test_precommit_rollback_and_close_failure() &&
                   test_postcommit_quarantine_owner_close_and_disconnect()
               ? 0
               : 1;
}
