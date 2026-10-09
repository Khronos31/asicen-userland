// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "asicen/device_profile.h"
#include "px4/card_service.h"
#include "px4/tuner_service.h"

#include <atomic>
#include <array>
#include <cstdint>
#include <map>
#include <mutex>

namespace asicen {

class MockTunerBackend final : public px4::userland::TunerServiceBackend {
public:
    MockTunerBackend() noexcept = default;
    explicit MockTunerBackend(const DeviceProfile& profile) noexcept;
    // Count 1 models one shared T/S receiver; 2 and 4 use split S/T pairs.
    explicit MockTunerBackend(std::uint8_t receiver_count) noexcept;
    std::uint8_t receiver_count() const noexcept override;
    bool receiver_supports(std::uint8_t receiver,
                           px4::userland::ipc::System system) const noexcept override;
    px4::userland::Result<void> open_receiver(std::uint8_t receiver) noexcept override;
    px4::userland::Result<void> tune_terrestrial(std::uint8_t receiver,
        std::uint32_t frequency_khz, std::uint32_t timeout_ms) noexcept override;
    px4::userland::Result<void> tune_satellite(std::uint8_t receiver,
        std::uint32_t frequency_khz, std::uint32_t timeout_ms) noexcept override;
    px4::userland::Result<bool> is_locked(std::uint8_t receiver,
        px4::userland::ipc::System system) noexcept override;
    px4::userland::Result<void> select_satellite_slot(std::uint8_t receiver,
        std::uint8_t slot, std::uint32_t timeout_ms) noexcept override;
    px4::userland::Result<void> select_satellite_tsid(std::uint8_t receiver,
        std::uint16_t tsid, std::uint32_t timeout_ms) noexcept override;
    px4::userland::Result<void> close_receiver(std::uint8_t receiver) noexcept override;
    px4::userland::Result<void> begin_tune_power(std::uint8_t receiver,
        px4::userland::ipc::System system, std::uint8_t lnb_voltage) noexcept override;
    px4::userland::Result<void> commit_tune_power(std::uint8_t receiver) noexcept override;
    px4::userland::Result<void> rollback_tune_power(std::uint8_t receiver) noexcept override;
    // Synthetic state for offline contract checks only. This is neither a
    // hardware capability claim nor a measured output voltage.
    px4::userland::Result<std::uint8_t> simulated_lnb_voltage(
        std::uint8_t receiver) const noexcept;
    // px4-compatible 15 V gate. Mirrors the --allow-lnb-power daemon flag;
    // default true keeps direct backend tests unchanged.
    void set_allow_lnb_power(bool allow) noexcept { allow_lnb_power_ = allow; }
    bool allow_lnb_power() const noexcept { return allow_lnb_power_; }
    px4::userland::Result<void> start_capture(std::uint8_t receiver,
        px4::userland::ipc::System system) noexcept override;
    px4::userland::Result<void> stop_capture(std::uint8_t receiver,
        px4::userland::ipc::System system) noexcept override;
    px4::userland::Result<void> shutdown() noexcept override;
    void request_stop() noexcept override;

private:
    struct PowerState final {
        std::uint8_t voltage = 0U;
        std::uint8_t previous = 0U;
        std::uint8_t requested = 0U;
        bool pending = false;
    };
    std::uint8_t receiver_count_ = 4U;
    bool combined_isdb_ts_ = false;
    bool allow_lnb_power_ = true;
    std::atomic<bool> stopping_{false};
    mutable std::mutex power_mutex_;
    std::array<PowerState, 4U> power_{};
};

class MockTunerStream final : public px4::userland::TunerStreamControl {
public:
    px4::userland::Result<void> attach(
        const px4::userland::TunerAttachment& attachment) noexcept override;
    px4::userland::Result<void> detach(
        const px4::userland::TunerAttachment& attachment) noexcept override;
    px4::userland::Result<px4::userland::TunerStreamCounters> stats(
        const px4::userland::TunerAttachment& attachment) const noexcept override;
    px4::userland::Result<px4::userland::TunerStreamFinalSnapshot> final_snapshot(
        const px4::userland::TunerAttachment& attachment) const noexcept override;
    px4::userland::Result<void> release_final(
        const px4::userland::TunerAttachment& attachment) noexcept override;
    px4::userland::Result<px4::userland::TunerStreamReadResult> read(
        const px4::userland::TunerAttachment& attachment,
        px4::userland::MutableByteView output,
        px4::userland::Timeout timeout) noexcept override;
    px4::userland::Result<px4::userland::TunerStreamTerminal> terminal(
        const px4::userland::TunerAttachment& attachment) const noexcept override;

private:
    struct State final {
        px4::userland::TunerAttachment identity{};
        px4::userland::TunerStreamCounters counters{};
        px4::userland::TunerStreamFinalSnapshot final{};
        bool active = true;
        bool detached = false;
    };
    mutable std::mutex mutex_;
    std::map<std::uint64_t, State> streams_;
};

class UnsupportedCardBackend final : public px4::userland::CardServiceBackend {
public:
    px4::userland::Result<void> set_power(bool on) noexcept override;
    px4::userland::Result<void> initialize_uart() noexcept override;
    px4::userland::Result<bool> detect_card() noexcept override;
};

class UnsupportedCardSession final : public px4::userland::CardProtocolSession {
public:
    px4::userland::Result<void> initialize() noexcept override;
    px4::userland::Result<std::size_t> transmit(
        px4::userland::ByteView apdu,
        px4::userland::MutableByteView response) noexcept override;
    bool initialized() const noexcept override;
    const px4::userland::CardAtr& atr() const noexcept override;
    void invalidate() noexcept override;
private:
    px4::userland::CardAtr atr_{};
};

}  // namespace asicen
