// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef ASICEN_USERLAND_CARD_ONLY_SERVICE_H
#define ASICEN_USERLAND_CARD_ONLY_SERVICE_H

#include "asicen/card_mailbox_hardware.h"
#include "asicen/card_operation_guard.h"
#include "px4/card_service.h"
#include "px4/tuner_service.h"

#include <csignal>

namespace asicen {

class LibusbW3u3Hardware;

class W3u3CardServiceBackend final : public px4::userland::CardServiceBackend {
public:
    W3u3CardServiceBackend(W3u3CardMailboxHardware& mailbox, CardOperationGuard& guard,
                           const volatile std::sig_atomic_t* stop_requested) noexcept
        : mailbox_(mailbox), guard_(guard), stop_requested_(stop_requested)
    {
    }
    px4::userland::Result<void> set_power(bool on) noexcept override;
    px4::userland::Result<void> initialize_uart() noexcept override;
    px4::userland::Result<bool> detect_card() noexcept override;
    void request_stop() noexcept override;

private:
    W3u3CardMailboxHardware& mailbox_;
    CardOperationGuard& guard_;
    const volatile std::sig_atomic_t* stop_requested_;
};

class W3u3CardProtocolSession final : public px4::userland::CardProtocolSession {
public:
    W3u3CardProtocolSession(px4::userland::CardSession& session, CardOperationGuard& guard,
                            const volatile std::sig_atomic_t* stop_requested) noexcept
        : session_(session), guard_(guard), stop_requested_(stop_requested)
    {
    }
    px4::userland::Result<void> initialize() noexcept override;
    px4::userland::Result<std::size_t>
    transmit(px4::userland::ByteView apdu,
             px4::userland::MutableByteView response) noexcept override;
    bool initialized() const noexcept override;
    const px4::userland::CardAtr& atr() const noexcept override;
    void invalidate() noexcept override;
    void request_stop() noexcept override;

private:
    px4::userland::CardSession& session_;
    CardOperationGuard& guard_;
    const volatile std::sig_atomic_t* stop_requested_;
};

class CardOnlyTunerBackend final : public px4::userland::TunerServiceBackend {
public:
    explicit CardOnlyTunerBackend(std::uint8_t count = 2U) : count_(count) {}
    std::uint8_t receiver_count() const noexcept override;
    bool receiver_supports(std::uint8_t, px4::userland::ipc::System) const noexcept override;
    px4::userland::Result<void> open_receiver(std::uint8_t) noexcept override;
    px4::userland::Result<void> tune_terrestrial(std::uint8_t, std::uint32_t,
                                                 std::uint32_t) noexcept override;
    px4::userland::Result<void> tune_satellite(std::uint8_t, std::uint32_t,
                                               std::uint32_t) noexcept override;
    px4::userland::Result<bool> is_locked(std::uint8_t,
                                          px4::userland::ipc::System) noexcept override;
    px4::userland::Result<void> select_satellite_slot(std::uint8_t, std::uint8_t,
                                                      std::uint32_t) noexcept override;
    px4::userland::Result<void> select_satellite_tsid(std::uint8_t, std::uint16_t,
                                                      std::uint32_t) noexcept override;
    px4::userland::Result<void> close_receiver(std::uint8_t) noexcept override;

private:
    std::uint8_t count_;
};

int run_card_only_server(LibusbW3u3Hardware& hardware, const char* runtime_directory,
                         const char* instance, bool group,
                         const volatile std::sig_atomic_t* stop_requested,
                         bool (*stop_callback)() noexcept = nullptr) noexcept;

int run_live_card_stream_server(LibusbW3u3Hardware& hardware, const char* runtime_directory,
                                const char* instance, bool group,
                                const volatile std::sig_atomic_t* stop_requested,
                                bool (*stop_callback)() noexcept = nullptr) noexcept;

}  // namespace asicen

#endif  // ASICEN_USERLAND_CARD_ONLY_SERVICE_H
