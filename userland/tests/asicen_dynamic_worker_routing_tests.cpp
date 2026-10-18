// SPDX-License-Identifier: GPL-2.0-or-later
#include "control_workers.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <poll.h>
#include <thread>
#include <vector>

namespace {
using namespace px4::userland;
using namespace px4::userland::ipc;
using namespace px4::userland::ipc::posix;

#define CHECK(condition)                                                            \
    do {                                                                            \
        if (!(condition)) {                                                         \
            std::fprintf(stderr, "check failed %s:%d: %s\n", __FILE__, __LINE__,  \
                         #condition);                                               \
            return false;                                                           \
        }                                                                           \
    } while (false)

class CardBackend final : public CardServiceBackend {
public:
    Result<void> set_power(bool) noexcept override { return Result<void>::success(); }
    Result<void> initialize_uart() noexcept override { return Result<void>::success(); }
    Result<bool> detect_card() noexcept override { return Result<bool>::success(false); }
};

class CardSession final : public CardProtocolSession {
public:
    Result<void> initialize() noexcept override { return Result<void>::success(); }
    Result<std::size_t> transmit(ByteView, MutableByteView) noexcept override
    { return Result<std::size_t>::failure(Error::UNSUPPORTED); }
    bool initialized() const noexcept override { return false; }
    const CardAtr& atr() const noexcept override { return atr_; }
    void invalidate() noexcept override {}
private:
    CardAtr atr_{};
};

class Nonce final : public TunerNonceSource {
public:
    Result<std::array<std::uint8_t, kNonceLength>> generate() noexcept override
    {
        std::array<std::uint8_t, kNonceLength> nonce{};
        nonce[0U] = 1U;
        return Result<std::array<std::uint8_t, kNonceLength>>::success(nonce);
    }
};

class Time final : public TunerServiceTime {
public:
    std::uint64_t monotonic_ms() noexcept override { return 1U; }
    void sleep_ms(std::uint32_t) noexcept override {}
};

class Backend final : public TunerServiceBackend {
public:
    std::uint8_t receiver_count() const noexcept override { return 4U; }
    bool receiver_supports(std::uint8_t receiver, System system) const noexcept override
    {
        return receiver < 4U &&
               system == (receiver % 2U == 0U ? System::ISDB_S : System::ISDB_T);
    }
    Result<void> open_receiver(std::uint8_t receiver) noexcept override
    {
        if (receiver >= entered.size()) return Result<void>::failure(Error::INVALID_ARGUMENT);
        {
            std::lock_guard<std::mutex> lock(mutex);
            entered[receiver].store(true, std::memory_order_release);
        }
        condition.notify_all();
        if (receiver == 0U) {
            std::unique_lock<std::mutex> lock(mutex);
            condition.wait(lock, [&] { return stopping.load(std::memory_order_acquire); });
        }
        return stopping.load(std::memory_order_acquire)
                   ? Result<void>::failure(Error::NOT_READY)
                   : Result<void>::success();
    }
    Result<void> tune_terrestrial(std::uint8_t, std::uint32_t,
                                  std::uint32_t) noexcept override
    { return Result<void>::success(); }
    Result<void> tune_satellite(std::uint8_t, std::uint32_t,
                                std::uint32_t) noexcept override
    { return Result<void>::success(); }
    Result<bool> is_locked(std::uint8_t, System) noexcept override
    { return Result<bool>::success(true); }
    Result<void> select_satellite_slot(std::uint8_t, std::uint8_t,
                                       std::uint32_t) noexcept override
    { return Result<void>::success(); }
    Result<void> select_satellite_tsid(std::uint8_t, std::uint16_t,
                                       std::uint32_t) noexcept override
    { return Result<void>::success(); }
    Result<void> close_receiver(std::uint8_t) noexcept override
    { return Result<void>::success(); }
    void request_stop() noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex);
        stopping.store(true, std::memory_order_release);
        condition.notify_all();
    }

    std::array<std::atomic<bool>, 4U> entered{};
    std::atomic<bool> stopping{false};
    std::mutex mutex;
    std::condition_variable condition;
};

bool wait_for(const std::atomic<bool>& flag, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!flag.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return flag.load(std::memory_order_acquire);
}

void drain_completions(ControlWorkerLanes& workers,
                       std::vector<ControlWorkerCompletion>& completions)
{
    for (const auto lane : {ControlWorkerLane::tuner_dev1,
                            ControlWorkerLane::tuner_dev2, ControlWorkerLane::card}) {
        ControlWorkerCompletion completion{};
        while (workers.try_pop(lane, completion)) completions.push_back(completion);
    }
}

bool test_receiver_routing_overtake_and_shutdown()
{
    CardBackend card_backend;
    CardSession card_session;
    CardService card(card_backend, card_session);
    Backend backend;
    Nonce nonce;
    Time time;
    TunerService tuner(backend, nonce, time);
    auto created = ControlWorkerLanes::create(card, tuner);
    CHECK(created);
    auto workers = std::move(created.value());

    ControlWorkerTask receiver0{};
    receiver0.operation = ControlWorkerOperation::tuner_acquire;
    receiver0.client_id = 10U;
    receiver0.receiver = 0U;
    CHECK(workers->submit(ControlWorkerLane::tuner_dev1, receiver0));
    CHECK(wait_for(backend.entered[0], std::chrono::seconds(1)));

    ControlWorkerTask receiver1 = receiver0;
    receiver1.client_id = 11U;
    receiver1.receiver = 1U;
    CHECK(workers->submit(ControlWorkerLane::tuner_dev1, receiver1));

    ControlWorkerTask receiver2 = receiver0;
    receiver2.client_id = 12U;
    receiver2.receiver = 2U;
    CHECK(workers->submit(ControlWorkerLane::tuner_dev2, receiver2));

    ControlWorkerTask invalid{};
    invalid.operation = ControlWorkerOperation::tuner_acquire;
    invalid.client_id = 13U;
    invalid.receiver = 4U;
    const auto invalid_four = workers->submit(ControlWorkerLane::tuner_dev2, invalid);
    CHECK(!invalid_four && invalid_four.error() == Error::INVALID_ARGUMENT);

    std::vector<ControlWorkerCompletion> completions;
    const auto overtake_deadline = std::chrono::steady_clock::now() +
                                   std::chrono::seconds(1);
    bool receiver2_done = false;
    while (!receiver2_done && std::chrono::steady_clock::now() < overtake_deadline) {
        drain_completions(*workers, completions);
        for (const auto& completion : completions) {
            if (completion.receiver == 2U) receiver2_done = true;
        }
        if (!receiver2_done) {
            pollfd descriptor{workers->wake_fd(), POLLIN, 0};
            (void)::poll(&descriptor, 1U, 10);
            workers->drain_wake();
        }
    }
    CHECK(receiver2_done);
    CHECK(backend.entered[2U].load(std::memory_order_acquire));
    CHECK(!backend.entered[1U].load(std::memory_order_acquire));
    for (const auto& completion : completions) {
        if (completion.receiver == 2U) CHECK(completion.error == Error::OK);
    }

    const auto shutdown_started = std::chrono::steady_clock::now();
    tuner.request_stop();
    CHECK(workers->request_stop());
    const auto join_deadline = shutdown_started + std::chrono::seconds(2);
    while (workers->live_thread_count() != 0U &&
           std::chrono::steady_clock::now() < join_deadline) {
        drain_completions(*workers, completions);
        pollfd descriptor{workers->wake_fd(), POLLIN, 0};
        (void)::poll(&descriptor, 1U, 10);
        workers->drain_wake();
    }
    drain_completions(*workers, completions);
    CHECK(workers->live_thread_count() == 0U);
    CHECK(workers->join());
    const auto shutdown_elapsed = std::chrono::steady_clock::now() - shutdown_started;
    CHECK(shutdown_elapsed < std::chrono::seconds(2));

    bool receiver0_cancelled = false;
    bool receiver1_cancelled = false;
    bool receiver2_succeeded = false;
    for (const auto& completion : completions) {
        if (completion.receiver == 0U)
            receiver0_cancelled = completion.error == Error::NOT_READY;
        else if (completion.receiver == 1U)
            receiver1_cancelled = completion.error == Error::NOT_READY;
        else if (completion.receiver == 2U)
            receiver2_succeeded = completion.error == Error::OK;
    }
    CHECK(receiver0_cancelled && receiver1_cancelled && receiver2_succeeded);
    CHECK(tuner.shutdown());
    return true;
}
}  // namespace

int main()
{
    if (!test_receiver_routing_overtake_and_shutdown()) return 1;
    std::puts("PASS actual worker receiver routing/overtake/shutdown");
    return 0;
}
