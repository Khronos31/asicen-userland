// SPDX-License-Identifier: GPL-2.0-only
#include "px4/tuner_service.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {
using namespace px4::userland;
using namespace px4::userland::ipc;

class Backend final : public TunerServiceBackend {
public:
    explicit Backend(std::uint8_t count) noexcept : count_(count) {}

    std::uint8_t receiver_count() const noexcept override { return count_; }
    bool receiver_supports(std::uint8_t receiver, System system) const noexcept override
    {
        if (receiver == 0U) return system == System::ISDB_T || system == System::ISDB_S;
        return system == (receiver % 2U == 0U ? System::ISDB_T : System::ISDB_S);
    }
    Result<void> open_receiver(std::uint8_t receiver) noexcept override
    {
        events.push_back("open" + std::to_string(receiver));
        return Result<void>::success();
    }
    Result<void> tune_terrestrial(std::uint8_t, std::uint32_t,
                                  std::uint32_t) noexcept override
    {
        events.push_back("tune-t");
        return Result<void>::success();
    }
    Result<void> tune_satellite(std::uint8_t, std::uint32_t,
                                std::uint32_t) noexcept override
    {
        events.push_back("tune-s");
        return Result<void>::success();
    }
    Result<bool> is_locked(std::uint8_t, System) noexcept override
    { return Result<bool>::success(true); }
    Result<void> select_satellite_slot(std::uint8_t, std::uint8_t,
                                       std::uint32_t) noexcept override
    { return Result<void>::success(); }
    Result<void> select_satellite_tsid(std::uint8_t, std::uint16_t,
                                       std::uint32_t) noexcept override
    { return Result<void>::success(); }
    Result<void> close_receiver(std::uint8_t) noexcept override
    {
        events.push_back("close");
        return Result<void>::success();
    }
    Result<void> start_capture(std::uint8_t, System) noexcept override
    {
        events.push_back("capture-start");
        return Result<void>::success();
    }
    Result<void> stop_capture(std::uint8_t, System) noexcept override
    {
        events.push_back("capture-stop");
        return Result<void>::success();
    }
    void request_stop() noexcept override { events.push_back("backend-stop"); }

    std::vector<std::string> events;
private:
    std::uint8_t count_;
};

class Nonce final : public TunerNonceSource {
public:
    Result<std::array<std::uint8_t, kNonceLength>> generate() noexcept override
    {
        std::array<std::uint8_t, kNonceLength> value{};
        value[0] = ++next_;
        return Result<std::array<std::uint8_t, kNonceLength>>::success(value);
    }
private:
    std::uint8_t next_ = 0U;
};

class Time final : public TunerServiceTime {
public:
    std::uint64_t monotonic_ms() noexcept override { return now_; }
    void sleep_ms(std::uint32_t ms) noexcept override { now_ += ms; }
private:
    std::uint64_t now_ = 1U;
};

class Stream final : public TunerStreamControl {
public:
    void request_stop() noexcept override { events.push_back("stream-stop"); }
    Result<void> attach(const TunerAttachment&) noexcept override
    {
        events.push_back("stream-attach");
        return Result<void>::success();
    }
    Result<void> detach(const TunerAttachment&) noexcept override
    {
        events.push_back("stream-detach");
        return Result<void>::success();
    }
    Result<TunerStreamFinalSnapshot> final_snapshot(
        const TunerAttachment&) const noexcept override
    {
        return Result<TunerStreamFinalSnapshot>::success({});
    }
    std::vector<std::string> events;
};

bool check_profile_count(std::uint8_t count)
{
    Backend backend(count);
    Nonce nonce;
    Time time;
    TunerService service(backend, nonce, time);
    const auto status = service.status();
    if (!status) { std::fprintf(stderr, "profile status %u\n", count); return false; }
    for (std::uint8_t receiver = 0U; receiver < kReceiverCount; ++receiver) {
        const auto expected = receiver < count ? ReceiverState::free
                                               : ReceiverState::absent;
        if (status.value().receiver_states[receiver] != expected) {
            std::fprintf(stderr, "profile state %u/%u\n", count, receiver);
            return false;
        }
    }
    const auto last = service.acquire(1U, static_cast<std::uint8_t>(count - 1U));
    if (!last || !service.release(1U, last.value().lease_id)) {
        std::fprintf(stderr, "profile acquire/release %u\n", count);
        return false;
    }
    const bool invalid = service.acquire(1U, count).error() == Error::INVALID_ARGUMENT;
    if (!invalid) std::fprintf(stderr, "profile upper bound %u\n", count);
    return invalid;
}

bool check_dual_stream_cleanup_and_stop_hooks()
{
    Backend backend(4U);
    Nonce nonce;
    Time time;
    Stream stream;
    TunerService service(backend, nonce, time, nullptr, nullptr, &stream);
    const auto lease = service.acquire(2U, 0U);
    if (!lease) { std::fprintf(stderr, "dual acquire\n"); return false; }

    const TuneRequestPayload terrestrial{lease.value().lease_id, System::ISDB_T,
        557142U, 0xffffU, 0xffffU, 6000000U, 0U, 1000U};
    const TuneRequestPayload satellite{lease.value().lease_id, System::ISDB_S,
        1049480U, 0xffffU, 5U, 0U, 0U, 1000U};
    if (!service.tune(2U, terrestrial) || !service.tune(2U, satellite)) {
        std::fprintf(stderr, "dual tune\n"); return false;
    }

    service.request_stop();
    if (backend.events != std::vector<std::string>{"open0", "tune-t", "tune-s",
                                                   "backend-stop"} ||
        !stream.events.empty())
        { std::fprintf(stderr, "stop hook counts\n"); return false; }
    backend.events.clear();

    if (!service.start_stream(2U, lease.value().lease_id)) {
        std::fprintf(stderr, "first start\n"); return false;
    }
    const auto attached = service.attach_stream(lease.value().lease_id,
                                                lease.value().nonce);
    if (!attached || attached.value().system != System::ISDB_S) {
        std::fprintf(stderr, "first attach\n"); return false;
    }
    if (!service.detach_stream(attached.value()) ||
        service.detach_stream(attached.value()).error() != Error::NOT_FOUND)
        { std::fprintf(stderr, "stale detach\n"); return false; }
    if (stream.events != std::vector<std::string>{"stream-attach", "stream-detach"})
        { std::fprintf(stderr, "first stream events\n"); return false; }

    if (!service.tune(2U, satellite) ||
        !service.start_stream(2U, lease.value().lease_id))
        { std::fprintf(stderr, "second tune/start\n"); return false; }
    const auto live = service.attach_stream(lease.value().lease_id,
                                            lease.value().nonce);
    if (!live || !service.disconnect_client(2U)) {
        std::fprintf(stderr, "disconnect cleanup\n"); return false;
    }
    const bool result = backend.events == std::vector<std::string>{"capture-start", "capture-stop",
                                                                   "tune-s", "capture-start",
                                                                   "capture-stop", "close"} &&
           stream.events == std::vector<std::string>{"stream-attach", "stream-detach",
                                                      "stream-attach", "stream-detach"};
    if (!result) {
        std::fprintf(stderr, "final cleanup events backend=%zu stream=%zu\n",
                     backend.events.size(), stream.events.size());
        for (const auto& event : backend.events) std::fprintf(stderr, " backend %s\n", event.c_str());
        for (const auto& event : stream.events) std::fprintf(stderr, " stream %s\n", event.c_str());
    }
    return result;
}
}  // namespace

int main()
{
    const bool ok = check_profile_count(1U) && check_profile_count(2U) &&
                    check_profile_count(4U) && check_dual_stream_cleanup_and_stop_hooks();
    if (!ok) std::fprintf(stderr, "receiver facade compatibility check failed\n");
    return ok ? 0 : 1;
}
