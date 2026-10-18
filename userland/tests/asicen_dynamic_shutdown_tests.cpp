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
    } while (0)

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
    std::uint64_t monotonic_ms() noexcept override { return 1U; }
    void sleep_ms(std::uint32_t) noexcept override {}
};

class TunerBackend final : public TunerServiceBackend {
public:
    std::uint8_t receiver_count() const noexcept override { return 4U; }
    bool receiver_supports(std::uint8_t receiver, System system) const noexcept override
    { return receiver < 4U && system == (receiver % 2U == 0U ? System::ISDB_S : System::ISDB_T); }
    Result<void> open_receiver(std::uint8_t) noexcept override
    { return Result<void>::success(); }
    Result<void> tune_terrestrial(std::uint8_t, std::uint32_t, std::uint32_t) noexcept override
    {
        if (!block_tune.load(std::memory_order_acquire)) return Result<void>::success();
        {
            std::lock_guard<std::mutex> lock(mutex);
            tune_entered.store(true, std::memory_order_release);
        }
        condition.notify_all();
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&] { return stopped.load(std::memory_order_acquire); });
        return Result<void>::failure(Error::NOT_READY);
    }
    Result<void> tune_satellite(std::uint8_t, std::uint32_t, std::uint32_t) noexcept override
    { return Result<void>::success(); }
    Result<bool> is_locked(std::uint8_t, System) noexcept override
    { return Result<bool>::success(true); }
    Result<void> select_satellite_slot(std::uint8_t, std::uint8_t, std::uint32_t) noexcept override
    { return Result<void>::success(); }
    Result<void> select_satellite_tsid(std::uint8_t, std::uint16_t, std::uint32_t) noexcept override
    { return Result<void>::success(); }
    Result<void> close_receiver(std::uint8_t) noexcept override
    { return Result<void>::success(); }
    Result<void> start_capture(std::uint8_t, System) noexcept override
    { return Result<void>::success(); }
    Result<void> stop_capture(std::uint8_t, System) noexcept override
    { return Result<void>::success(); }
    void request_stop() noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex);
        stopped.store(true, std::memory_order_release);
        condition.notify_all();
    }

    std::atomic<bool> block_tune{false};
    std::atomic<bool> tune_entered{false};
    std::atomic<bool> stopped{false};
    std::mutex mutex;
    std::condition_variable condition;
};

class CardBackend final : public CardServiceBackend {
public:
    Result<void> set_power(bool) noexcept override { return Result<void>::success(); }
    Result<void> initialize_uart() noexcept override { return Result<void>::success(); }
    Result<bool> detect_card() noexcept override
    {
        if (!block_detect.load(std::memory_order_acquire))
            return Result<bool>::success(present.load(std::memory_order_acquire));
        {
            std::lock_guard<std::mutex> lock(mutex);
            detect_entered.store(true, std::memory_order_release);
        }
        condition.notify_all();
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&] { return stopped.load(std::memory_order_acquire); });
        return Result<bool>::failure(Error::NOT_READY);
    }
    void request_stop() noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex);
        stopped.store(true, std::memory_order_release);
        condition.notify_all();
    }

    std::atomic<bool> block_detect{false};
    std::atomic<bool> detect_entered{false};
    std::atomic<bool> present{false};
    std::atomic<bool> stopped{false};
    std::mutex mutex;
    std::condition_variable condition;
};

class CardSession final : public CardProtocolSession {
public:
    Result<void> initialize() noexcept override
    {
        if (!block_initialize.load(std::memory_order_acquire))
            return Result<void>::success();
        {
            std::lock_guard<std::mutex> lock(mutex);
            initialize_entered.store(true, std::memory_order_release);
        }
        condition.notify_all();
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&] { return stopped.load(std::memory_order_acquire); });
        return Result<void>::failure(Error::NOT_READY);
    }
    Result<std::size_t> transmit(ByteView, MutableByteView) noexcept override
    { return Result<std::size_t>::failure(Error::NOT_READY); }
    bool initialized() const noexcept override { return false; }
    const CardAtr& atr() const noexcept override { return atr_; }
    void invalidate() noexcept override {}
    void request_stop() noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex);
        stopped.store(true, std::memory_order_release);
        condition.notify_all();
    }

    std::atomic<bool> initialize_entered{false};
    std::atomic<bool> block_initialize{true};
    std::atomic<bool> stopped{false};
    std::mutex mutex;
    std::condition_variable condition;
private:
    CardAtr atr_{};
};

class Stream final : public TunerStreamControl {
public:
    Result<void> attach(const TunerAttachment& value) noexcept override
    {
        attachment = value;
        return Result<void>::success();
    }
    Result<void> detach(const TunerAttachment&) noexcept override
    { return Result<void>::success(); }
    Result<TunerStreamCounters> stats(const TunerAttachment&) const noexcept override
    { return Result<TunerStreamCounters>::success({}); }
    Result<TunerStreamFinalSnapshot> final_snapshot(const TunerAttachment&) const noexcept override
    { return Result<TunerStreamFinalSnapshot>::success({}); }
    Result<void> release_final(const TunerAttachment&) noexcept override
    { return Result<void>::success(); }
    Result<TunerStreamReadResult> read(const TunerAttachment&, MutableByteView,
                                       Timeout) noexcept override
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            read_entered.store(true, std::memory_order_release);
        }
        condition.notify_all();
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&] { return stopped.load(std::memory_order_acquire); });
        return Result<TunerStreamReadResult>::failure(Error::NOT_READY);
    }
    void request_stop() noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex);
        stopped.store(true, std::memory_order_release);
        condition.notify_all();
    }

    TunerAttachment attachment{};
    std::atomic<bool> read_entered{false};
    std::atomic<bool> stopped{false};
    std::mutex mutex;
    std::condition_variable condition;
};

class CardFixture final {
public:
    CardBackend backend;
    CardSession session;
    CardService service{backend, session};
};

class TunerFixture final {
public:
    TunerBackend backend;
    Nonce nonce;
    Time time;
    Stream stream;
    TunerService service{backend, nonce, time, nullptr, nullptr, &stream};
};

void drain_completions(ControlWorkerLanes& workers,
                       std::vector<ControlWorkerCompletion>& completions)
{
    for (const auto lane : {ControlWorkerLane::tuner_dev1,
                            ControlWorkerLane::tuner_dev2, ControlWorkerLane::card}) {
        ControlWorkerCompletion completion{};
        while (workers.try_pop(lane, completion)) completions.push_back(completion);
    }
}

bool stop_and_collect(ControlWorkerLanes& workers,
                      std::vector<ControlWorkerCompletion>& completions,
                      std::chrono::steady_clock::time_point deadline)
{
    if (!workers.request_stop()) return false;
    while (workers.live_thread_count() != 0U && std::chrono::steady_clock::now() < deadline) {
        drain_completions(workers, completions);
        pollfd descriptor{workers.wake_fd(), POLLIN, 0};
        (void)::poll(&descriptor, 1U, 10);
        workers.drain_wake();
    }
    drain_completions(workers, completions);
    return workers.live_thread_count() == 0U && workers.join().has_value();
}

bool test_active_and_queued_tune_stop()
{
    CardFixture card;
    TunerFixture tuner;
    tuner.backend.block_tune.store(true, std::memory_order_release);
    auto created = ControlWorkerLanes::create(card.service, tuner.service);
    CHECK(created);
    auto workers = std::move(created.value());
    const auto lease = tuner.service.acquire(40U, 1U);
    CHECK(lease);

    ControlWorkerTask first{};
    first.operation = ControlWorkerOperation::tuner_tune;
    first.client_id = 40U;
    first.receiver = 1U;
    first.tune = TuneRequestPayload{lease.value().lease_id, System::ISDB_T,
                                    557142U, 0xffffU, 0xffffU, 6000000U, 0U, 30000U};
    CHECK(workers->submit(ControlWorkerLane::tuner_dev1, first));
    const auto entered_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!tuner.backend.tune_entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < entered_deadline)
        std::this_thread::yield();
    CHECK(tuner.backend.tune_entered.load(std::memory_order_acquire));

    ControlWorkerTask queued = first;
    queued.request_id = 2U;
    CHECK(workers->submit(ControlWorkerLane::tuner_dev1, queued));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(!workers->idle());

    const auto started = std::chrono::steady_clock::now();
    tuner.service.request_stop();
    std::vector<ControlWorkerCompletion> completions;
    CHECK(stop_and_collect(*workers, completions, started + std::chrono::seconds(2)));
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
    std::size_t tune_completions = 0U;
    for (const auto& completion : completions) {
        if (completion.operation == ControlWorkerOperation::tuner_tune) {
            ++tune_completions;
            CHECK(completion.error == Error::NOT_READY);
        }
    }
    CHECK(tune_completions == 2U);
    CHECK(tuner.service.shutdown());
    return true;
}

bool test_blocked_card_detect_is_stopped_by_worker_shutdown()
{
    CardFixture card;
    card.backend.present.store(true, std::memory_order_release);
    card.session.block_initialize.store(false, std::memory_order_release);
    CHECK(card.service.connect(50U, ShareMode::shared));
    card.backend.block_detect.store(true, std::memory_order_release);
    TunerFixture tuner;
    auto created = ControlWorkerLanes::create(card.service, tuner.service);
    CHECK(created);
    auto workers = std::move(created.value());
    ControlWorkerTask task{};
    task.operation = ControlWorkerOperation::card_presence;
    task.client_id = 51U;
    CHECK(workers->submit(ControlWorkerLane::card, task));
    const auto entered_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!card.backend.detect_entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < entered_deadline)
        std::this_thread::yield();
    CHECK(card.backend.detect_entered.load(std::memory_order_acquire));

    const auto started = std::chrono::steady_clock::now();
    card.service.request_stop();
    std::vector<ControlWorkerCompletion> completions;
    CHECK(stop_and_collect(*workers, completions, started + std::chrono::seconds(2)));
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
    CHECK(completions.size() == 1U);
    CHECK(completions[0].operation == ControlWorkerOperation::card_presence &&
          completions[0].error == Error::NOT_READY);
    return true;
}

bool test_blocked_session_initialize_is_stopped_by_worker_shutdown()
{
    CardFixture card;
    card.backend.present.store(true, std::memory_order_release);
    TunerFixture tuner;
    auto created = ControlWorkerLanes::create(card.service, tuner.service);
    CHECK(created);
    auto workers = std::move(created.value());
    ControlWorkerTask task{};
    task.operation = ControlWorkerOperation::card_connect;
    task.client_id = 52U;
    task.share_mode = ShareMode::shared;
    CHECK(workers->submit(ControlWorkerLane::card, task));
    const auto entered_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!card.session.initialize_entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < entered_deadline)
        std::this_thread::yield();
    CHECK(card.session.initialize_entered.load(std::memory_order_acquire));

    const auto started = std::chrono::steady_clock::now();
    card.service.request_stop();
    std::vector<ControlWorkerCompletion> completions;
    CHECK(stop_and_collect(*workers, completions, started + std::chrono::seconds(2)));
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
    CHECK(completions.size() == 1U);
    CHECK(completions[0].operation == ControlWorkerOperation::card_connect &&
          completions[0].error == Error::NOT_READY);
    return true;
}

bool test_blocked_service_stream_read_is_woken()
{
    TunerFixture tuner;
    const auto lease = tuner.service.acquire(61U, 1U);
    CHECK(lease);
    const TuneRequestPayload request{lease.value().lease_id, System::ISDB_T,
                                     557142U, 0xffffU, 0xffffU, 6000000U, 0U, 30000U};
    CHECK(tuner.service.tune(61U, request));
    CHECK(tuner.service.start_stream(61U, lease.value().lease_id));
    const auto attachment = tuner.service.attach_stream(lease.value().lease_id,
                                                        lease.value().nonce);
    CHECK(attachment);

    std::atomic<bool> read_done{false};
    Error read_error = Error::OK;
    std::thread reader([&] {
        std::array<std::uint8_t, 188U> bytes{};
        const auto result = tuner.stream.read(attachment.value(),
            MutableByteView{bytes.data(), bytes.size()}, Timeout{30000U});
        if (!result) read_error = result.error();
        read_done.store(true, std::memory_order_release);
    });
    const auto entered_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!tuner.stream.read_entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < entered_deadline)
        std::this_thread::yield();
    CHECK(tuner.stream.read_entered.load(std::memory_order_acquire));

    const auto started = std::chrono::steady_clock::now();
    tuner.service.request_stop();
    tuner.stream.request_stop();
    const auto done_deadline = started + std::chrono::seconds(2);
    while (!read_done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < done_deadline)
        std::this_thread::yield();
    reader.join();
    CHECK(read_done.load(std::memory_order_acquire));
    CHECK(read_error == Error::NOT_READY);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
    CHECK(tuner.service.shutdown());
    return true;
}
}  // namespace

int main()
{
    std::puts("RUN active and queued tune stop");
    std::fflush(stdout);
    if (!test_active_and_queued_tune_stop()) return 1;
    std::puts("RUN blocked card detect stop");
    std::fflush(stdout);
    if (!test_blocked_card_detect_is_stopped_by_worker_shutdown()) return 1;
    std::puts("RUN blocked session initialize stop");
    std::fflush(stdout);
    if (!test_blocked_session_initialize_is_stopped_by_worker_shutdown()) return 1;
    std::puts("RUN blocked service stream read stop");
    std::fflush(stdout);
    if (!test_blocked_service_stream_read_is_woken()) return 1;
    std::puts("PASS worker/service blocked-operation shutdown regressions");
    return 0;
}
