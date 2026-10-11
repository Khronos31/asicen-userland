// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef ASICEN_USERLAND_CARD_MAILBOX_HARDWARE_H
#define ASICEN_USERLAND_CARD_MAILBOX_HARDWARE_H

#include "asicen/frontend_sequence.h"
#include "asicen/device_profile.h"
#include "px4/card.h"

#include <array>
#include <chrono>
#include <cstdint>

namespace asicen {

// V2 Windows BDA post-submit delay (file/RVA 0x14134), in milliseconds.
constexpr unsigned card_mailbox_submit_delay_ms(ModelId model, std::size_t length) noexcept
{
    return model == ModelId::W3u3V2 ? (length > 70U ? 140U : (length < 10U ? 150U : 100U)) : 100U;
}

// Source-backed W3U3 controller mailbox adapter for the upstream portable
// CardSession. The caller owns the absolute deadline/cancellation policy via
// FrontendTransport; this adapter never performs USB outside that transport.
class W3u3CardMailboxHardware final : public px4::userland::CardHardware,
                                      public px4::userland::CardTime {
public:
    explicit W3u3CardMailboxHardware(FrontendTransport& transport,
                                     ModelId model = ModelId::W3u3) noexcept
        : transport_(transport), model_(model)
    {
    }

    px4::userland::Result<bool> detect_card() noexcept override;
    px4::userland::Result<void> reset_card(px4::userland::It930xCardDelay&) noexcept override;
    px4::userland::Result<bool> data_ready() noexcept override;
    px4::userland::Result<std::size_t>
    read_data(px4::userland::MutableByteView output) noexcept override;
    px4::userland::Result<void> write_data(px4::userland::ByteView input) noexcept override;
    px4::userland::Result<void>
    set_baud_rate(px4::userland::It930xCardBaudRate baud_rate) noexcept override;
    std::uint64_t monotonic_ms() noexcept override;
    void sleep_ms(std::uint32_t milliseconds) noexcept override;

    px4::userland::Result<void> shutdown_controller() noexcept;

private:
    px4::userland::Result<void> write_reg(std::uint8_t reg, std::uint8_t value) noexcept;
    px4::userland::Result<std::uint8_t> read_reg(std::uint8_t reg) noexcept;
    px4::userland::Result<std::uint16_t> available_length() noexcept;
    px4::userland::Result<void> read_window(std::uint16_t length, std::uint8_t* output) noexcept;
    px4::userland::Result<void> write_window(px4::userland::ByteView input) noexcept;
    px4::userland::Result<void> execute(const ControlTransfer& transfer,
                                        unsigned char* response) noexcept;
    bool interrupted() const noexcept;
    px4::userland::Error interruption_error() const noexcept;

    FrontendTransport& transport_;
    ModelId model_;
    bool atr_pending_ = false;
    bool controller_initialized_ = false;
    bool cleanup_mode_ = false;
};

}  // namespace asicen

#endif  // ASICEN_USERLAND_CARD_MAILBOX_HARDWARE_H
