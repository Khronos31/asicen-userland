// SPDX-License-Identifier: GPL-2.0-or-later
#include "px4/card_service.h"
#include "px4/control_server.h"
#include "px4/posix_tuner_nonce.h"
#include "px4/tuner_service.h"
#include "asicen/product_profile.h"
#include "test_temp_directory.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace {
using namespace px4::userland;
using namespace px4::userland::ipc;
using namespace px4::userland::ipc::posix;

class Tuner final : public TunerServiceBackend {
public:
    std::uint8_t receiver_count() const noexcept override
    { return asicen::profile::kReceiverCount; }
    bool receiver_supports(std::uint8_t receiver, System system) const noexcept override
    {
        if (receiver >= asicen::profile::kReceiverCount) return false;
        return system == (asicen::profile::is_satellite_receiver(receiver)
                              ? System::ISDB_S : System::ISDB_T);
    }
    Result<void> open_receiver(std::uint8_t) noexcept override { return Result<void>::success(); }
    Result<void> tune_terrestrial(std::uint8_t, std::uint32_t, std::uint32_t) noexcept override
    {
        std::unique_lock<std::mutex> lock(mutex);
        ++tune_calls;
        entered.store(true);
        condition.notify_all();
        condition.wait(lock, [&] { return stopped.load(); });
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
    Result<void> close_receiver(std::uint8_t) noexcept override { return Result<void>::success(); }
    void request_stop() noexcept override
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopped.store(true);
        }
        condition.notify_all();
    }
    std::atomic<bool> stopped{false};
    std::atomic<bool> entered{false};
    std::atomic<unsigned int> tune_calls{0U};
    std::mutex mutex;
    std::condition_variable condition;
};

class CardBackend final : public CardServiceBackend {
public:
    Result<void> set_power(bool) noexcept override { return Result<void>::success(); }
    Result<void> initialize_uart() noexcept override { return Result<void>::success(); }
    Result<bool> detect_card() noexcept override
    {
        std::unique_lock<std::mutex> lock(mutex);
        entered.store(true);
        condition.notify_all();
        condition.wait(lock, [&] { return stopped.load(); });
        return Result<bool>::failure(Error::NOT_READY);
    }
    void request_stop() noexcept override
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopped.store(true);
        }
        condition.notify_all();
    }
    std::atomic<bool> stopped{false};
    std::atomic<bool> entered{false};
    std::mutex mutex;
    std::condition_variable condition;
};

class ProbeCardSession final : public CardProtocolSession {
public:
    Result<void> initialize() noexcept override
    {
        std::unique_lock<std::mutex> lock(mutex);
        entered.store(true);
        condition.notify_all();
        condition.wait(lock, [&] { return stopped.load(); });
        return Result<void>::failure(Error::NOT_READY);
    }
    Result<std::size_t> transmit(ByteView, MutableByteView) noexcept override
    { return Result<std::size_t>::failure(Error::UNSUPPORTED); }
    bool initialized() const noexcept override { return false; }
    const CardAtr& atr() const noexcept override { return atr_; }
    void invalidate() noexcept override {}
    void request_stop() noexcept override
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopped.store(true);
        }
        condition.notify_all();
    }
    std::atomic<bool> stopped{false};
    std::atomic<bool> entered{false};
    std::mutex mutex;
    std::condition_variable condition;
private:
    CardAtr atr_{};
};

class Stream final : public TunerStreamControl {
public:
    Result<void> attach(const TunerAttachment&) noexcept override { return Result<void>::success(); }
    Result<void> detach(const TunerAttachment&) noexcept override { return Result<void>::success(); }
    Result<TunerStreamReadResult> read(const TunerAttachment&, MutableByteView,
                                       Timeout) noexcept override
    {
        std::unique_lock<std::mutex> lock(mutex);
        entered.store(true);
        condition.notify_all();
        condition.wait(lock, [&] { return stopped.load(); });
        return Result<TunerStreamReadResult>::failure(Error::NOT_READY);
    }
    void request_stop() noexcept override
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopped.store(true);
        }
        condition.notify_all();
    }
    std::atomic<bool> stopped{false};
    std::atomic<bool> entered{false};
    std::mutex mutex;
    std::condition_variable condition;
};

class Time final : public TunerServiceTime {
public:
    std::uint64_t monotonic_ms() noexcept override { return 1U; }
    void sleep_ms(std::uint32_t) noexcept override {}
};
}  // namespace

int main()
{
    std::string pattern = test::temporary_directory_template("asicen-shutdown-");
    char* made = ::mkdtemp(pattern.data());
    if (made == nullptr) return 1;
    const std::string root(made);
    Tuner tuner_backend;
    CardBackend card_backend;
    ProbeCardSession card_session;
    Stream stream;
    PosixTunerNonceSource nonce;
    Time time;
    CardService card(card_backend, card_session);
    TunerService tuner_service(tuner_backend, nonce, time, nullptr, nullptr, &stream);
    const EndpointConfig endpoint{root.c_str(), "hooks", kControlEndpointName};
    auto server = PosixControlServer::create(endpoint, card, tuner_service, {}, true,
                                              0x03U, &stream, 4U, false);
    if (!server) {
        std::filesystem::remove_all(root);
        return 2;
    }
    const auto acquired = tuner_service.acquire(77U, 1U);
    if (!acquired) return 3;
    const TuneRequestPayload tune{acquired.value().lease_id, System::ISDB_T,
                                  557142U, 0xffffU, 0xffffU, 6000000U, 0U, 30000U};
    std::atomic<bool> first_done{false};
    std::atomic<bool> second_done{false};
    std::thread first([&] {
        (void)tuner_service.tune(77U, tune);
        first_done.store(true);
    });
    const auto wait_deadline = std::chrono::steady_clock::now() +
                               std::chrono::seconds(1);
    while (!tuner_backend.entered.load() &&
           std::chrono::steady_clock::now() < wait_deadline)
        std::this_thread::yield();
    if (!tuner_backend.entered.load()) {
        tuner_backend.request_stop();
        first.join();
        return 4;
    }
    std::thread queued([&] {
        (void)tuner_service.tune(77U, tune);
        second_done.store(true);
    });
    std::atomic<bool> card_done{false};
    std::atomic<bool> session_done{false};
    std::atomic<bool> stream_done{false};
    std::thread delayed_card([&] {
        (void)card_backend.detect_card();
        card_done.store(true);
    });
    std::thread delayed_session([&] {
        (void)card_session.initialize();
        session_done.store(true);
    });
    const TunerAttachment attachment{};
    std::thread delayed_stream([&] {
        (void)stream.read(attachment, MutableByteView{nullptr, 0U}, Timeout{30000U});
        stream_done.store(true);
    });
    const auto all_entered = std::chrono::steady_clock::now() +
                             std::chrono::seconds(1);
    while ((!card_backend.entered.load() || !card_session.entered.load() ||
            !stream.entered.load()) && std::chrono::steady_clock::now() < all_entered)
        std::this_thread::yield();
    if (!card_backend.entered.load() || !card_session.entered.load() ||
        !stream.entered.load()) {
        tuner_backend.request_stop();
        card_backend.request_stop();
        card_session.request_stop();
        stream.request_stop();
        first.join();
        queued.join();
        delayed_card.join();
        delayed_session.join();
        delayed_stream.join();
        return 4;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto started = std::chrono::steady_clock::now();
    const auto stopped = server.value()->shutdown();
    first.join();
    queued.join();
    delayed_card.join();
    delayed_session.join();
    delayed_stream.join();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const bool ok = stopped && tuner_backend.stopped.load() &&
        card_backend.stopped.load() && card_session.stopped.load() &&
        stream.stopped.load() && elapsed < std::chrono::seconds(2) &&
        first_done.load() && second_done.load() && card_done.load() &&
        session_done.load() && stream_done.load() && tuner_backend.tune_calls.load() >= 1U;
    server.value().reset();
    std::filesystem::remove_all(root);
    if (!ok) std::fprintf(stderr, "shutdown hooks missing or exceeded 2 seconds\n");
    return ok ? 0 : 5;
}
