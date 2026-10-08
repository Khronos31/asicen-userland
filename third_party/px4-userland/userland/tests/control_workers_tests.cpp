// SPDX-License-Identifier: GPL-2.0-only
#include "control_workers.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <condition_variable>
#include <mutex>
#include <poll.h>
#include <thread>

namespace {

using namespace px4::userland;
using namespace px4::userland::ipc;
using namespace px4::userland::ipc::posix;

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            std::fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__,       \
                         #condition);                                                       \
            return false;                                                                   \
        }                                                                                   \
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
    {
        return Result<std::size_t>::failure(Error::UNSUPPORTED);
    }
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
        std::array<std::uint8_t, kNonceLength> value{};
        value[0U] = 1U;
        return Result<std::array<std::uint8_t, kNonceLength>>::success(value);
    }
};

class Time final : public TunerServiceTime {
public:
    std::uint64_t monotonic_ms() noexcept override { return 0U; }
    void sleep_ms(std::uint32_t) noexcept override {}
};

class TunerBackend final : public TunerServiceBackend {
public:
    Result<void> open_receiver(std::uint8_t receiver) noexcept override
    {
        if (receiver == 0U && block_dev1.load()) {
            {
                std::lock_guard<std::mutex> lock(dev1_mutex);
                dev1_started.store(true);
            }
            dev1_condition.notify_all();
            while (block_dev1.load()) std::this_thread::yield();
        }
        ++open_calls[receiver];
        return Result<void>::success();
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
    Result<void> close_receiver(std::uint8_t receiver) noexcept override
    {
        ++close_calls[receiver];
        return Result<void>::success();
    }

    std::atomic<bool> block_dev1{false};
    std::atomic<bool> dev1_started{false};
    std::mutex dev1_mutex;
    std::condition_variable dev1_condition;
    std::array<std::atomic<std::size_t>, kReceiverCount> open_calls{};
    std::array<std::atomic<std::size_t>, kReceiverCount> close_calls{};
};

class BlockedDev1Guard final {
public:
    BlockedDev1Guard(TunerBackend& backend) noexcept : backend_(backend) {}

    ~BlockedDev1Guard() noexcept
    {
        if (!active_) return;
        backend_.block_dev1.store(false);
        backend_.dev1_condition.notify_all();
    }

    void dismiss() noexcept { active_ = false; }

private:
    TunerBackend& backend_;
    bool active_ = true;
};

struct Fixture final {
    CardBackend card_backend;
    CardSession card_session;
    CardService card{card_backend, card_session};
    TunerBackend tuner_backend;
    Nonce nonce;
    Time time;
    TunerService tuner{tuner_backend, nonce, time};
};

bool drain_until_stopped(ControlWorkerLanes& workers, std::size_t expected,
                         std::size_t& observed) noexcept
{
    (void)workers.request_stop();
    while (workers.live_thread_count() != 0U) {
        ControlWorkerCompletion completion{};
        for (const ControlWorkerLane lane : {ControlWorkerLane::tuner_dev1,
                                             ControlWorkerLane::tuner_dev2,
                                             ControlWorkerLane::card}) {
            while (workers.try_pop(lane, completion)) ++observed;
        }
        pollfd descriptor{workers.wake_fd(), POLLIN, 0};
        (void)::poll(&descriptor, 1U, 10);
        workers.drain_wake();
    }
    ControlWorkerCompletion completion{};
    for (const ControlWorkerLane lane : {ControlWorkerLane::tuner_dev1,
                                         ControlWorkerLane::tuner_dev2,
                                         ControlWorkerLane::card}) {
        while (workers.try_pop(lane, completion)) ++observed;
    }
    CHECK(workers.join());
    if (observed != expected) {
        std::fprintf(stderr, "worker completions observed=%zu expected=%zu\n",
                     observed, expected);
        return false;
    }
    return true;
}

bool test_startup_rollback(Fixture& fixture)
{
    for (int lane = -1; lane < 3; ++lane) {
        const ControlWorkerStartupOptions options{false, lane};
        if (lane < 0) continue;
        const auto result = ControlWorkerLanes::create_for_test(
            fixture.card, fixture.tuner, options);
        CHECK(!result && result.error() == Error::INTERNAL);
    }
    const auto no_pipe = ControlWorkerLanes::create_for_test(
        fixture.card, fixture.tuner, ControlWorkerStartupOptions{true, -1});
    CHECK(!no_pipe && no_pipe.error() == Error::INTERNAL);
    return true;
}

bool test_fifo_and_wakeup(Fixture& fixture)
{
    auto created = ControlWorkerLanes::create(fixture.card, fixture.tuner);
    CHECK(created);
    std::unique_ptr<ControlWorkerLanes> workers = std::move(created.value());
    for (std::uint32_t request = 1U; request <= 3U; ++request) {
        ControlWorkerTask task{};
        task.operation = ControlWorkerOperation::tuner_status;
        task.client_id = 1U;
        task.request_id = request;
        CHECK(workers->submit(ControlWorkerLane::tuner_dev1, task));
    }
    std::array<std::uint32_t, 3U> ids{};
    std::size_t count = 0U;
    while (count < ids.size()) {
        ControlWorkerCompletion completion{};
        if (!workers->try_pop(ControlWorkerLane::tuner_dev1, completion)) {
            pollfd descriptor{workers->wake_fd(), POLLIN, 0};
            CHECK(::poll(&descriptor, 1U, 1000) >= 0);
            workers->drain_wake();
            continue;
        }
        ids[count++] = completion.request_id;
    }
    CHECK(ids[0U] == 1U && ids[1U] == 2U && ids[2U] == 3U);
    std::size_t observed = 0U;
    CHECK(drain_until_stopped(*workers, 0U, observed));
    return true;
}

bool test_dequeue_active_visibility(Fixture& fixture)
{
    fixture.tuner_backend.dev1_started.store(false);
    fixture.tuner_backend.block_dev1.store(true);
    auto created = ControlWorkerLanes::create(fixture.card, fixture.tuner);
    CHECK(created);
    std::unique_ptr<ControlWorkerLanes> workers = std::move(created.value());
    ControlWorkerTask task{};
    task.operation = ControlWorkerOperation::tuner_acquire;
    task.client_id = 11U;
    task.request_id = 110U;
    task.receiver = 0U;
    CHECK(workers->submit(ControlWorkerLane::tuner_dev1, task));
    BlockedDev1Guard blocked_guard(fixture.tuner_backend);
    {
        std::unique_lock<std::mutex> lock(fixture.tuner_backend.dev1_mutex);
        CHECK(fixture.tuner_backend.dev1_condition.wait_for(
            lock, std::chrono::seconds(10),
            [&]() { return fixture.tuner_backend.dev1_started.load(); }));
    }
    // The backend barrier proves the worker has dequeued the task and is
    // active; idle() must not report a transient empty state.
    CHECK(!workers->idle());
    fixture.tuner_backend.block_dev1.store(false);
    fixture.tuner_backend.dev1_condition.notify_all();
    std::size_t observed = 0U;
    CHECK(drain_until_stopped(*workers, 1U, observed));
    CHECK(fixture.tuner.disconnect_client(11U));
    return true;
}

bool test_lane_routing(Fixture& fixture)
{
    auto created = ControlWorkerLanes::create(fixture.card, fixture.tuner);
    CHECK(created);
    std::unique_ptr<ControlWorkerLanes> workers = std::move(created.value());
    ControlWorkerTask card{};
    card.operation = ControlWorkerOperation::card_status;
    CHECK(!workers->submit(ControlWorkerLane::tuner_dev1, card) &&
          workers->submit(ControlWorkerLane::card, card));
    ControlWorkerTask receiver{};
    receiver.operation = ControlWorkerOperation::tuner_acquire;
    receiver.client_id = 12U;

    receiver.receiver = 8U;
    const auto invalid_eight = workers->submit(ControlWorkerLane::tuner_dev2, receiver);
    CHECK(!invalid_eight && invalid_eight.error() == Error::INVALID_ARGUMENT);
    receiver.receiver = 255U;
    const auto invalid_255 = workers->submit(ControlWorkerLane::tuner_dev2, receiver);
    CHECK(!invalid_255 && invalid_255.error() == Error::INVALID_ARGUMENT);

    receiver.receiver = 4U;
    CHECK(!workers->submit(ControlWorkerLane::tuner_dev1, receiver) &&
          workers->submit(ControlWorkerLane::tuner_dev2, receiver));
    ControlWorkerTask status{};
    status.operation = ControlWorkerOperation::tuner_status;
    CHECK(!workers->submit(ControlWorkerLane::card, status) &&
          workers->submit(ControlWorkerLane::tuner_dev1, status));
    std::size_t observed = 0U;
    CHECK(drain_until_stopped(*workers, 3U, observed));
    CHECK(fixture.tuner.disconnect_client(12U));
    return true;
}

bool test_independent_lanes_and_backpressure(Fixture& fixture)
{
    fixture.tuner_backend.dev1_started.store(false);
    fixture.tuner_backend.block_dev1.store(true);
    auto created = ControlWorkerLanes::create(fixture.card, fixture.tuner);
    CHECK(created);
    std::unique_ptr<ControlWorkerLanes> workers = std::move(created.value());
    BlockedDev1Guard blocked_guard(fixture.tuner_backend);

    ControlWorkerTask slow{};
    slow.operation = ControlWorkerOperation::tuner_acquire;
    slow.client_id = 1U;
    slow.request_id = 100U;
    slow.receiver = 0U;
    CHECK(workers->submit(ControlWorkerLane::tuner_dev1, slow));
    {
        std::unique_lock<std::mutex> lock(fixture.tuner_backend.dev1_mutex);
        CHECK(fixture.tuner_backend.dev1_condition.wait_for(
            lock, std::chrono::seconds(10),
            [&]() { return fixture.tuner_backend.dev1_started.load(); }));
    }

    ControlWorkerTask dev2{};
    dev2.operation = ControlWorkerOperation::tuner_acquire;
    dev2.client_id = 2U;
    dev2.request_id = 201U;
    dev2.receiver = 4U;
    ControlWorkerTask card{};
    card.operation = ControlWorkerOperation::card_status;
    card.client_id = 3U;
    card.request_id = 301U;
    CHECK(workers->submit(ControlWorkerLane::tuner_dev2, dev2));
    CHECK(workers->submit(ControlWorkerLane::card, card));
    bool dev2_done = false;
    bool card_done = false;
    const auto completion_deadline = std::chrono::steady_clock::now() +
                                     std::chrono::seconds(10);
    while ((!dev2_done || !card_done) &&
           std::chrono::steady_clock::now() < completion_deadline) {
        for (const ControlWorkerLane lane : {ControlWorkerLane::tuner_dev2,
                                             ControlWorkerLane::card}) {
            ControlWorkerCompletion completion{};
            while (workers->try_pop(lane, completion)) {
                if (completion.request_id == 201U) dev2_done = true;
                if (completion.request_id == 301U) card_done = true;
            }
        }
        if (dev2_done && card_done) break;
        pollfd ready{workers->wake_fd(), POLLIN, 0};
        (void)::poll(&ready, 1U, 50);
        workers->drain_wake();
    }
    CHECK(dev2_done && card_done);

    std::size_t accepted = 0U;
    bool busy = false;
    for (std::size_t attempt = 0U; attempt < kControlWorkerQueueCapacity + 2U;
         ++attempt) {
        ControlWorkerTask task{};
        task.operation = ControlWorkerOperation::tuner_status;
        task.client_id = 4U;
        task.request_id = static_cast<std::uint32_t>(400U + attempt);
        task.receiver = 0U;
        const auto submitted = workers->submit(ControlWorkerLane::tuner_dev1, task);
        if (submitted) ++accepted;
        else {
            CHECK(submitted.error() == Error::BUSY);
            busy = true;
            break;
        }
    }
    CHECK(busy && accepted != 0U);
    fixture.tuner_backend.block_dev1.store(false);
    fixture.tuner_backend.dev1_condition.notify_all();
    std::size_t observed = 0U;
    CHECK(drain_until_stopped(*workers, accepted + 1U, observed));
    CHECK(fixture.tuner.disconnect_client(2U));
    return true;
}

bool test_completion_ring_backpressure(Fixture& fixture)
{
    auto created = ControlWorkerLanes::create(fixture.card, fixture.tuner);
    CHECK(created);
    std::unique_ptr<ControlWorkerLanes> workers = std::move(created.value());
    for (std::size_t index = 0U; index < kControlWorkerQueueCapacity; ++index) {
        ControlWorkerTask task{};
        task.operation = ControlWorkerOperation::tuner_status;
        task.client_id = 5U;
        task.request_id = static_cast<std::uint32_t>(index + 1U);
        CHECK(workers->submit(ControlWorkerLane::tuner_dev1, task));
    }
    pollfd descriptor{workers->wake_fd(), POLLIN, 0};
    CHECK(::poll(&descriptor, 1U, 1000) == 1);
    CHECK((descriptor.revents & POLLIN) != 0);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(10);
    while (workers->pending_completions_for_test(ControlWorkerLane::tuner_dev1) !=
               kControlWorkerQueueCapacity &&
           std::chrono::steady_clock::now() < deadline) {
        pollfd ready{workers->wake_fd(), POLLIN, 0};
        (void)::poll(&ready, 1U, 5);
    }
    CHECK(workers->pending_completions_for_test(ControlWorkerLane::tuner_dev1) ==
          kControlWorkerQueueCapacity);
    workers->drain_wake();
    // stop_and_join is the safe shutdown path when completions are not needed
    // by the caller anymore; it drains the full ring before pthread_join.
    CHECK(workers->stop_and_join());
    return workers->error() == Error::OK;
}

}  // namespace

bool run_control_workers_tests()
{
    std::unique_ptr<Fixture> fixture(new Fixture());
    return test_startup_rollback(*fixture) && test_fifo_and_wakeup(*fixture) &&
           test_dequeue_active_visibility(*fixture) && test_lane_routing(*fixture) &&
           test_independent_lanes_and_backpressure(*fixture) &&
           test_completion_ring_backpressure(*fixture);
}
