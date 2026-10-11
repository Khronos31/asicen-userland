// SPDX-License-Identifier: GPL-2.0-only
#include "px4/tuner_service.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <vector>

namespace {

using namespace px4::userland;
using namespace px4::userland::ipc;

#define SETTLE_CHECK(condition)                                                     \
    do {                                                                            \
        if (!(condition)) {                                                         \
            std::fprintf(stderr, "settle check failed at line %d: %s\n",          \
                         __LINE__, #condition);                                    \
            return false;                                                           \
        }                                                                           \
    } while (false)

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

class Nonce final : public TunerNonceSource {
public:
    Result<std::array<std::uint8_t, kNonceLength>> generate() noexcept override
    {
        std::array<std::uint8_t, kNonceLength> value{};
        value[0U] = next++;
        return Result<std::array<std::uint8_t, kNonceLength>>::success(value);
    }

    std::uint8_t next = 1U;
};

class Backend final : public TunerServiceBackend {
public:
    Result<void> open_receiver(std::uint8_t) noexcept override
    {
        events.push_back(1);
        return Result<void>::success();
    }

    Result<void> tune_terrestrial(std::uint8_t, std::uint32_t,
                                  std::uint32_t) noexcept override
    {
        events.push_back(2);
        if (time != nullptr && tune_ms != 0U) time->sleep_ms(tune_ms);
        return Result<void>::success();
    }

    Result<void> tune_satellite(std::uint8_t, std::uint32_t,
                                std::uint32_t) noexcept override
    {
        events.push_back(2);
        return Result<void>::success();
    }

    Result<bool> is_locked(std::uint8_t, System) noexcept override
    {
        if (lock_results.empty()) return Result<bool>::success(true);
        const auto result = lock_results.front();
        lock_results.pop_front();
        events.push_back(3);
        return result;
    }

    bool requires_terrestrial_lock_settle() const noexcept override
    {
        return true;
    }

    Result<void> select_satellite_slot(std::uint8_t, std::uint8_t,
                                       std::uint32_t) noexcept override
    {
        events.push_back(4);
        return Result<void>::success();
    }

    Result<void> select_satellite_tsid(std::uint8_t, std::uint16_t,
                                       std::uint32_t) noexcept override
    {
        events.push_back(4);
        return Result<void>::success();
    }

    Result<void> close_receiver(std::uint8_t) noexcept override
    {
        return Result<void>::success();
    }

    Result<void> commit_tune_power(std::uint8_t) noexcept override
    {
        events.push_back(5);
        commit_time = time == nullptr ? 0U : time->monotonic_ms();
        return Result<void>::success();
    }

    Result<void> rollback_tune_power(std::uint8_t) noexcept override
    {
        ++rollbacks;
        return Result<void>::success();
    }

    std::deque<Result<bool>> lock_results;
    std::vector<int> events;
    TunerServiceTime* time = nullptr;
    // Time the frontend tune itself takes (e.g. the single-receiver 100 ms).
    std::uint32_t tune_ms = 0U;
    std::uint64_t commit_time = 0U;
    std::size_t rollbacks = 0U;
};

TuneRequestPayload terrestrial(std::uint64_t lease_id,
                               std::uint32_t timeout_ms = 1000U) noexcept
{
    return TuneRequestPayload{lease_id, System::ISDB_T, 557000U, 0xffffU,
                               0xffffU, 6000000U, 0U, timeout_ms};
}

TuneRequestPayload satellite(std::uint64_t lease_id) noexcept
{
    return TuneRequestPayload{lease_id, System::ISDB_S, 146875U, 0xffffU,
                               0U, 0U, 0U, 1000U};
}

bool test_immediate_lock_waits_before_commit()
{
    Backend backend;
    Clock clock;
    backend.time = &clock;
    Nonce nonce;
    TunerService service(backend, nonce, clock);
    const auto lease = service.acquire(1U, 2U);
    SETTLE_CHECK(lease);

    const auto result = service.tune(1U, terrestrial(lease.value().lease_id));
    SETTLE_CHECK(result);
    SETTLE_CHECK(clock.sleeps.size() == 1U && clock.sleeps[0U] == 340U);
    SETTLE_CHECK(clock.now == 340U && backend.commit_time == 340U);
    SETTLE_CHECK(backend.rollbacks == 0U && backend.events.back() == 5);
    return true;
}

bool test_delayed_lock_waits_only_remaining_settle()
{
    Backend backend;
    Clock clock;
    backend.time = &clock;
    backend.lock_results = {
        Result<bool>::success(false), Result<bool>::success(false),
        Result<bool>::success(false), Result<bool>::success(true)};
    Nonce nonce;
    TunerService service(backend, nonce, clock);
    const auto lease = service.acquire(1U, 2U);
    SETTLE_CHECK(lease);

    SETTLE_CHECK(service.tune(1U, terrestrial(lease.value().lease_id)));
    SETTLE_CHECK(clock.sleeps.size() == 4U);
    SETTLE_CHECK(clock.sleeps[0U] == 10U && clock.sleeps[1U] == 10U &&
                clock.sleeps[2U] == 10U && clock.sleeps[3U] == 310U);
    SETTLE_CHECK(clock.now == 340U && backend.commit_time == 340U);
    return true;
}

bool test_lock_after_340ms_has_no_extra_settle()
{
    Backend backend;
    Clock clock;
    backend.time = &clock;
    for (std::size_t count = 0U; count < 35U; ++count)
        backend.lock_results.push_back(Result<bool>::success(false));
    backend.lock_results.push_back(Result<bool>::success(true));
    Nonce nonce;
    TunerService service(backend, nonce, clock);
    const auto lease = service.acquire(1U, 2U);
    SETTLE_CHECK(lease);

    SETTLE_CHECK(service.tune(1U, terrestrial(lease.value().lease_id)));
    SETTLE_CHECK(clock.now == 350U && backend.commit_time == 350U);
    SETTLE_CHECK(clock.sleeps.size() == 35U);
    return true;
}

bool test_settle_starts_after_frontend_tune()
{
    Backend backend;
    Clock clock;
    backend.time = &clock;
    backend.tune_ms = 100U;
    Nonce nonce;
    TunerService service(backend, nonce, clock);
    const auto lease = service.acquire(1U, 2U);
    SETTLE_CHECK(lease);

    // The settle counts from the first lock check, not from the TUNE start,
    // so time spent inside the frontend tune does not shorten it.
    SETTLE_CHECK(service.tune(1U, terrestrial(lease.value().lease_id)));
    SETTLE_CHECK(clock.sleeps.size() == 2U && clock.sleeps[0U] == 100U &&
                clock.sleeps[1U] == 340U);
    SETTLE_CHECK(clock.now == 440U && backend.commit_time == 440U);
    return true;
}

bool test_insufficient_timeout_rolls_back_without_sleep()
{
    Backend backend;
    Clock clock;
    backend.time = &clock;
    Nonce nonce;
    TunerService service(backend, nonce, clock);
    const auto lease = service.acquire(1U, 2U);
    SETTLE_CHECK(lease);

    const auto result = service.tune(1U, terrestrial(lease.value().lease_id, 300U));
    SETTLE_CHECK(!result && result.error() == Error::TIMEOUT);
    SETTLE_CHECK(clock.now == 0U && clock.sleeps.empty());
    SETTLE_CHECK(backend.rollbacks == 1U && backend.commit_time == 0U);
    return true;
}

bool test_settle_reaching_deadline_times_out_without_sleep()
{
    Backend backend;
    Clock clock;
    backend.time = &clock;
    Nonce nonce;
    TunerService service(backend, nonce, clock);
    const auto lease = service.acquire(1U, 2U);
    SETTLE_CHECK(lease);

    const auto result = service.tune(1U, terrestrial(lease.value().lease_id, 340U));
    SETTLE_CHECK(!result && result.error() == Error::TIMEOUT);
    SETTLE_CHECK(clock.now == 0U && clock.sleeps.empty());
    SETTLE_CHECK(backend.rollbacks == 1U && backend.commit_time == 0U);
    return true;
}

bool test_satellite_does_not_settle()
{
    Backend backend;
    Clock clock;
    backend.time = &clock;
    Nonce nonce;
    TunerService service(backend, nonce, clock);
    const auto lease = service.acquire(1U, 0U);
    SETTLE_CHECK(lease);

    SETTLE_CHECK(service.tune(1U, satellite(lease.value().lease_id)));
    SETTLE_CHECK(clock.now == 0U && clock.sleeps.empty());
    SETTLE_CHECK(backend.commit_time == 0U && backend.rollbacks == 0U);
    return true;
}

}  // namespace

int main()
{
    return test_immediate_lock_waits_before_commit() &&
                   test_delayed_lock_waits_only_remaining_settle() &&
                   test_lock_after_340ms_has_no_extra_settle() &&
                   test_settle_starts_after_frontend_tune() &&
                   test_insufficient_timeout_rolls_back_without_sleep() &&
                   test_settle_reaching_deadline_times_out_without_sleep() &&
                   test_satellite_does_not_settle()
               ? 0
               : 1;
}
