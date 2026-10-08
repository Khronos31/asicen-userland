// SPDX-License-Identifier: GPL-2.0-or-later
#include "px4/card_service.h"
#include "px4/control_client.h"
#include "px4/control_server.h"
#include "px4/posix_tuner_nonce.h"
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

class Backend final : public TunerServiceBackend {
public:
    std::uint8_t receiver_count() const noexcept override { return 4U; }
    bool receiver_supports(std::uint8_t receiver, System system) const noexcept override
    {
        if (receiver >= 4U) return false;
        return system == (receiver % 2U == 0U ? System::ISDB_S : System::ISDB_T);
    }
    Result<void> open_receiver(std::uint8_t receiver) noexcept override
    {
        std::unique_lock<std::mutex> lock(mutex);
        entered[receiver].store(true);
        condition.notify_all();
        if (receiver == 0U)
            condition.wait(lock, [&] { return release_zero.load() || stopping.load(); });
        return stopping.load() ? Result<void>::failure(Error::NOT_READY)
                               : Result<void>::success();
    }
    Result<void> tune_terrestrial(std::uint8_t, std::uint32_t, std::uint32_t) noexcept override
    { return Result<void>::success(); }
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
    void request_stop() noexcept override
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping.store(true);
        }
        condition.notify_all();
    }
    std::array<std::atomic<bool>, 4> entered{};
    std::atomic<bool> release_zero{false};
    std::atomic<bool> stopping{false};
    std::mutex mutex;
    std::condition_variable condition;
};

class CardBackend final : public CardServiceBackend {
public:
    Result<void> set_power(bool) noexcept override { return Result<void>::success(); }
    Result<void> initialize_uart() noexcept override { return Result<void>::success(); }
    Result<bool> detect_card() noexcept override
    { return Result<bool>::failure(Error::UNSUPPORTED); }
};

class Session final : public CardProtocolSession {
public:
    Result<void> initialize() noexcept override { return Result<void>::failure(Error::UNSUPPORTED); }
    Result<std::size_t> transmit(ByteView, MutableByteView) noexcept override
    { return Result<std::size_t>::failure(Error::UNSUPPORTED); }
    bool initialized() const noexcept override { return false; }
    const CardAtr& atr() const noexcept override { return atr_; }
    void invalidate() noexcept override {}
private:
    CardAtr atr_{};
};

class Time final : public TunerServiceTime {
public:
    std::uint64_t monotonic_ms() noexcept override { return 1U; }
    void sleep_ms(std::uint32_t) noexcept override {}
};

bool acquire(PosixControlClient& client, std::uint8_t receiver,
             std::atomic<bool>& completed, bool& success)
{
    std::array<std::uint8_t, kMaxControlPayload> payload{};
    const auto encoded = encode_payload(AcquireRequestPayload{receiver},
                                        MutableByteView{payload.data(), payload.size()});
    if (!encoded) return false;
    auto response = client.request(MessageType::ACQUIRE,
        ByteView{payload.data(), encoded.value()}, Timeout{2000U});
    success = static_cast<bool>(response);
    completed.store(true);
    return true;
}

bool wait_for(const std::atomic<bool>& flag, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!flag.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return flag.load();
}
}  // namespace

int main()
{
    std::string pattern = test::temporary_directory_template("asicen-routing-");
    char* made = ::mkdtemp(pattern.data());
    if (made == nullptr) return 1;
    const std::string root(made);
    Backend backend;
    CardBackend card_backend;
    Session session;
    CardService card(card_backend, session);
    PosixTunerNonceSource nonce;
    Time time;
    TunerService tuner(backend, nonce, time);
    const EndpointConfig endpoint{root.c_str(), "routing", kControlEndpointName};
    auto server_result = PosixControlServer::create(endpoint, card, tuner, {}, true,
                                                     0x03U, nullptr, 4U, false);
    if (!server_result) return 2;
    auto server = std::move(server_result.value());
    std::atomic<bool> polling{true};
    std::thread poller([&] {
        while (polling.load()) (void)server->poll_once(Timeout{20U});
    });
    auto zero_result = PosixControlClient::connect(endpoint, 0U, Timeout{2000U});
    auto one_result = PosixControlClient::connect(endpoint, 0U, Timeout{2000U});
    auto two_result = PosixControlClient::connect(endpoint, 0U, Timeout{2000U});
    if (!zero_result || !one_result || !two_result) {
        backend.request_stop();
        polling.store(false);
        poller.join();
        (void)server->shutdown();
        return 3;
    }
    auto zero = std::move(zero_result.value());
    auto one = std::move(one_result.value());
    auto two = std::move(two_result.value());
    std::atomic<bool> zero_done{false}, one_done{false}, two_done{false};
    bool zero_ok = false, one_ok = false, two_ok = false;
    std::thread zero_request([&] { (void)acquire(*zero, 0U, zero_done, zero_ok); });
    const bool zero_entered = wait_for(backend.entered[0], std::chrono::seconds(1));
    std::thread one_request([&] { (void)acquire(*one, 1U, one_done, one_ok); });
    std::thread two_request([&] { (void)acquire(*two, 2U, two_done, two_ok); });
    const bool two_overtook = wait_for(two_done, std::chrono::seconds(1));
    const bool same_lane_held = !backend.entered[1].load();
    polling.store(false);
    poller.join();
    const auto shutdown_started = std::chrono::steady_clock::now();
    const auto stopped = server->shutdown();
    zero_request.join();
    one_request.join();
    two_request.join();
    const auto shutdown_elapsed = std::chrono::steady_clock::now() - shutdown_started;
    server.reset();
    std::filesystem::remove_all(root);
    const bool ok = zero_entered && two_overtook && same_lane_held &&
                    zero_done.load() && one_done.load() && two_ok && !zero_ok &&
                    !one_ok && stopped && shutdown_elapsed < std::chrono::seconds(2);
    if (!ok) std::fprintf(stderr, "ASICEN worker lane mapping check failed\n");
    return ok ? 0 : 4;
}
