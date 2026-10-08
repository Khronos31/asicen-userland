// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/card_mailbox_hardware.h"

#include "asicen/card_mailbox.h"
#include "asicen/protocol.h"
#include "asicen/write_protocol.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <thread>
#include <vector>

namespace asicen {
namespace {
using px4::userland::Error;
using px4::userland::Result;

std::uint8_t reverse_bits(std::uint8_t value) noexcept {
    value = static_cast<std::uint8_t>(((value & 0x55U) << 1U) | ((value >> 1U) & 0x55U));
    value = static_cast<std::uint8_t>(((value & 0x33U) << 2U) | ((value >> 2U) & 0x33U));
    return static_cast<std::uint8_t>((value << 4U) | (value >> 4U));
}

}  // namespace

bool W3u3CardMailboxHardware::interrupted() const noexcept {
    return !cleanup_mode_ && (transport_.cancelled() || transport_.expired());
}

Result<void> W3u3CardMailboxHardware::execute(const ControlTransfer& transfer,
                                             unsigned char* response) noexcept {
    if (interrupted()) return Result<void>::failure(Error::TIMEOUT);
    if (response == nullptr || transfer.length == 0U)
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    const int transferred = transport_.control(transfer, response);
    if (transferred < 0)
        return Result<void>::failure(interrupted() ? Error::TIMEOUT : Error::USB_IO);
    if (transferred != transfer.length)
        return Result<void>::failure(Error::PROTOCOL_ERROR);
    if (!parse_status_response(response, static_cast<std::size_t>(transferred), nullptr, 0U))
        return Result<void>::failure(Error::PROTOCOL_ERROR);
    return Result<void>::success();
}

Result<void> W3u3CardMailboxHardware::write_reg(std::uint8_t reg,
                                               std::uint8_t value) noexcept {
    ControlTransfer transfer{};
    if (!make_i2c_write_chunk(0x4aU, reg, &value, 1U, false, &transfer, 500U))
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    std::array<unsigned char, 2> response{};
    return execute(transfer, response.data());
}

Result<std::uint8_t> W3u3CardMailboxHardware::read_reg(std::uint8_t reg) noexcept {
    const ControlTransfer transfer = make_i2c_read(0x4aU, reg, 1U, 0U, 500U);
    std::array<unsigned char, 2> response{};
    const auto result = execute(transfer, response.data());
    if (!result) return Result<std::uint8_t>::failure(result.error());
    return Result<std::uint8_t>::success(response[1]);
}

Result<std::uint16_t> W3u3CardMailboxHardware::available_length() noexcept {
    const auto low = read_reg(CardMailboxFacts::kStatusLowRegister);
    if (!low) return Result<std::uint16_t>::failure(low.error());
    const auto high = read_reg(CardMailboxFacts::kExtendedStatusRegister);
    if (!high) return Result<std::uint16_t>::failure(high.error());
    return Result<std::uint16_t>::success(decode_card_mailbox_length(low.value(), high.value()));
}

Result<bool> W3u3CardMailboxHardware::detect_card() noexcept {
    if (controller_initialized_) {
        const auto status = read_reg(CardMailboxFacts::kStatusHighRegister);
        if (!status) return Result<bool>::failure(status.error());
        return Result<bool>::success((status.value() & 0x01U) != 0U);
    }
    // DTV_PollingThread reset/presence prelude. Presence is separate from the
    // low/high mailbox length fields and is checked before the type-0f setup.
    for (const auto& step : std::array<std::pair<std::uint8_t, std::uint8_t>, 4>{
             {{0x00U, 0x80U}, {0x00U, 0x00U}, {0x04U, 0x00U}, {0x00U, 0x00U}}}) {
        const auto written = write_reg(step.first, step.second);
        if (!written) return Result<bool>::failure(written.error());
        if (step.first == 0x00U && step.second == 0x80U) sleep_ms(10U);
    }
    sleep_ms(100U);
    const auto status = read_reg(CardMailboxFacts::kStatusHighRegister);
    if (!status) return Result<bool>::failure(status.error());
    if ((status.value() & 0x01U) == 0U)
        return Result<bool>::success(false);
    controller_initialized_ = true;
    return Result<bool>::success(true);
}

Result<void> W3u3CardMailboxHardware::reset_card(
    px4::userland::It930xCardDelay&) noexcept {
    if (!controller_initialized_) return Result<void>::failure(Error::NOT_READY);
    sleep_ms(50U);
    const auto command = write_reg(0x00U, 0x08U);
    if (!command) return command;
    const auto reset = write_reg(0x04U, 0x80U);
    if (!reset) return reset;
    sleep_ms(200U);
    const auto activate = write_reg(0x00U, 0x09U);
    if (!activate) return activate;
    atr_pending_ = true;
    return Result<void>::success();
}

Result<bool> W3u3CardMailboxHardware::data_ready() noexcept {
    if (interrupted()) return Result<bool>::failure(Error::TIMEOUT);
    const auto status = read_reg(CardMailboxFacts::kStatusHighRegister);
    if (!status) return Result<bool>::failure(status.error());
    if ((status.value() & 0x01U) == 0U)
        return Result<bool>::failure(Error::CARD_REMOVED);
    const auto length = available_length();
    if (!length) return Result<bool>::failure(length.error());
    if (atr_pending_) {
        if (length.value() < 13U) return Result<bool>::success(false);
        if (length.value() > px4::userland::kCardAtrMaxLength)
            return Result<bool>::failure(Error::PROTOCOL_ERROR);
        std::array<std::uint8_t, px4::userland::kCardAtrMaxLength> atr{};
        const auto read = read_window(length.value(), atr.data());
        if (!read) return Result<bool>::failure(read.error());
        const auto parsed = px4::userland::parse_card_atr(
            px4::userland::ByteView{atr.data(), length.value()});
        if (parsed) {
            if (parsed.value().edc != px4::userland::CardEdc::lrc ||
                parsed.value().baud_rate != px4::userland::It930xCardBaudRate::baud_19200)
                return Result<bool>::failure(Error::UNSUPPORTED);
            return Result<bool>::success(true);
        }
        if (parsed.error() == Error::NOT_READY) return Result<bool>::success(false);
        return Result<bool>::failure(parsed.error());
    }
    if (length.value() > 255U) return Result<bool>::failure(Error::PROTOCOL_ERROR);
    if (length.value() < 3U) return Result<bool>::success(false);
    std::array<std::uint8_t, 3> header{};
    const auto read = read_window(header.size(), header.data());
    if (!read) return Result<bool>::failure(read.error());
    const std::size_t expected = 4U + static_cast<std::size_t>(header[2]);
    if (expected > 255U) return Result<bool>::failure(Error::PROTOCOL_ERROR);
    return Result<bool>::success(length.value() >= expected);
}

Result<void> W3u3CardMailboxHardware::read_window(std::uint16_t length,
                                                 std::uint8_t* output) noexcept {
    if (output == nullptr || length == 0U || length > 255U)
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    for (const CardMailboxChunk& page : build_card_mailbox_chunks(length)) {
        const auto selected = write_reg(CardMailboxFacts::kPageRegister, page.page);
        if (!selected) return selected;
        std::size_t in_page = 0U;
        while (in_page < page.length) {
            if (interrupted()) return Result<void>::failure(Error::TIMEOUT);
            const std::size_t count = std::min<std::size_t>(8U, page.length - in_page);
            const auto address = static_cast<std::uint8_t>(page.register_address + in_page);
            const ControlTransfer transfer = make_i2c_read(0x4aU, address,
                static_cast<std::uint16_t>(count), 0U, 500U);
            std::array<unsigned char, 9> response{};
            const auto done = execute(transfer, response.data());
            if (!done) return done;
            for (std::size_t i = 0; i < count; ++i)
                output[page.buffer_offset + in_page + i] = reverse_bits(response[i + 1U]);
            in_page += count;
        }
    }
    return Result<void>::success();
}

Result<void> W3u3CardMailboxHardware::write_window(
    px4::userland::ByteView input) noexcept {
    if (input.data == nullptr || input.size == 0U || input.size > 255U)
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    for (const CardMailboxChunk& page : build_card_mailbox_chunks(input.size)) {
        const auto selected = write_reg(CardMailboxFacts::kPageRegister, page.page);
        if (!selected) return selected;
        std::size_t in_page = 0U;
        while (in_page < page.length) {
            if (interrupted()) return Result<void>::failure(Error::TIMEOUT);
            const std::size_t count = std::min<std::size_t>(8U, page.length - in_page);
            const std::uint8_t address = static_cast<std::uint8_t>(page.register_address + in_page);
            if (count == 1U) {
                const auto written = write_reg(address,
                    reverse_bits(input.data[page.buffer_offset + in_page]));
                if (!written) return written;
                ++in_page;
                continue;
            }
            std::array<std::uint8_t, 9> payload{};
            payload[0] = address;
            for (std::size_t i = 0; i < count; ++i)
                payload[i + 1U] = reverse_bits(input.data[page.buffer_offset + in_page + i]);
            const auto sequence = build_i2c_write_sequence(0x4aU, 0U,
                payload.data(), count + 1U, 2U, 500U);
            if (sequence.empty()) return Result<void>::failure(Error::INVALID_ARGUMENT);
            for (const ControlTransfer& transfer : sequence) {
                std::array<unsigned char, 16> response{};
                const auto done = execute(transfer, response.data());
                if (!done) return done;
            }
            in_page += count;
        }
    }
    return Result<void>::success();
}

Result<std::size_t> W3u3CardMailboxHardware::read_data(
    px4::userland::MutableByteView output) noexcept {
    if (output.data == nullptr || output.size == 0U)
        return Result<std::size_t>::failure(Error::INVALID_ARGUMENT);
    const auto status_before = read_reg(CardMailboxFacts::kStatusHighRegister);
    if (!status_before) return Result<std::size_t>::failure(status_before.error());
    if ((status_before.value() & 0x01U) == 0U)
        return Result<std::size_t>::failure(Error::CARD_REMOVED);
    const auto available = available_length();
    if (!available) return Result<std::size_t>::failure(available.error());
    std::size_t logical = 0U;
    if (atr_pending_) {
        logical = available.value();
        if (logical < 13U || logical > px4::userland::kCardAtrMaxLength)
            return Result<std::size_t>::failure(Error::PROTOCOL_ERROR);
        std::array<std::uint8_t, px4::userland::kCardAtrMaxLength> atr{};
        const auto atr_read = read_window(static_cast<std::uint16_t>(logical), atr.data());
        if (!atr_read) return Result<std::size_t>::failure(atr_read.error());
        const auto parsed = px4::userland::parse_card_atr(
            px4::userland::ByteView{atr.data(), logical});
        if (!parsed) return Result<std::size_t>::failure(parsed.error());
        logical = parsed.value().length;
    } else {
        if (available.value() > 255U)
            return Result<std::size_t>::failure(Error::PROTOCOL_ERROR);
        if (available.value() < 3U) return Result<std::size_t>::failure(Error::NOT_READY);
        std::array<std::uint8_t, 3> header{};
        const auto header_read = read_window(header.size(), header.data());
        if (!header_read) return Result<std::size_t>::failure(header_read.error());
        logical = 4U + static_cast<std::size_t>(header[2]);
        if (logical > 255U) return Result<std::size_t>::failure(Error::PROTOCOL_ERROR);
        if (available.value() < logical) return Result<std::size_t>::failure(Error::NOT_READY);
    }
    if (logical > output.size)
        return Result<std::size_t>::failure(Error::BUFFER_TOO_SMALL);
    std::array<std::uint8_t, 255> frame{};
    const auto copied = read_window(static_cast<std::uint16_t>(logical), frame.data());
    if (!copied) return Result<std::size_t>::failure(copied.error());
    const auto status = read_reg(CardMailboxFacts::kStatusHighRegister);
    if (!status) return Result<std::size_t>::failure(status.error());
    if ((status.value() & 0x01U) == 0U)
        return Result<std::size_t>::failure(Error::CARD_REMOVED);
    const auto current_length = available_length();
    if (!current_length) return Result<std::size_t>::failure(current_length.error());
    if (current_length.value() != logical || current_length.value() > 255U)
        return Result<std::size_t>::failure(Error::PROTOCOL_ERROR);
    if (atr_pending_) {
        std::array<std::uint8_t, px4::userland::kCardAtrMaxLength> verify{};
        const auto reread = read_window(static_cast<std::uint16_t>(logical), verify.data());
        if (!reread) return Result<std::size_t>::failure(reread.error());
        if (!std::equal(frame.begin(), frame.begin() + static_cast<std::ptrdiff_t>(logical),
                        verify.begin()))
            return Result<std::size_t>::failure(Error::PROTOCOL_ERROR);
        const auto parsed = px4::userland::parse_card_atr(
            px4::userland::ByteView{frame.data(), logical});
        if (!parsed) return Result<std::size_t>::failure(parsed.error());
        if (parsed.value().edc != px4::userland::CardEdc::lrc ||
            parsed.value().baud_rate != px4::userland::It930xCardBaudRate::baud_19200)
            return Result<std::size_t>::failure(Error::UNSUPPORTED);
    } else {
        std::array<std::uint8_t, 3> verify_header{};
        const auto reread = read_window(verify_header.size(), verify_header.data());
        if (!reread) return Result<std::size_t>::failure(reread.error());
        if (!std::equal(verify_header.begin(), verify_header.end(), frame.begin()))
            return Result<std::size_t>::failure(Error::PROTOCOL_ERROR);
        std::uint8_t lrc = 0U;
        for (std::size_t i = 0; i < logical; ++i)
            lrc = static_cast<std::uint8_t>(lrc ^ frame[i]);
        if (lrc != 0U) return Result<std::size_t>::failure(Error::PROTOCOL_ERROR);
    }
    std::copy_n(frame.begin(), logical, output.data);
    const auto consumed = write_reg(CardMailboxFacts::kCommandRegister,
                                    CardMailboxFacts::kConsumeCommand);
    if (!consumed) return Result<std::size_t>::failure(consumed.error());
    atr_pending_ = false;
    return Result<std::size_t>::success(logical);
}

Result<void> W3u3CardMailboxHardware::write_data(
    px4::userland::ByteView input) noexcept {
    if (input.data == nullptr || input.size == 0U || input.size > 255U)
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    const auto low = write_reg(0x02U, static_cast<std::uint8_t>(input.size));
    if (!low) return low;
    const auto high = write_reg(0x38U, static_cast<std::uint8_t>((input.size >> 8U) & 1U));
    if (!high) return high;
    const auto data = write_window(input);
    if (!data) return data;
    const auto submitted = write_reg(CardMailboxFacts::kCommandRegister,
                                     CardMailboxFacts::kSubmitCommand);
    if (!submitted) return submitted;
    sleep_ms(100U);
    return interrupted() ? Result<void>::failure(Error::TIMEOUT) : Result<void>::success();
}

Result<void> W3u3CardMailboxHardware::set_baud_rate(
    px4::userland::It930xCardBaudRate baud_rate) noexcept {
    if (baud_rate != px4::userland::It930xCardBaudRate::baud_19200)
        return Result<void>::failure(Error::UNSUPPORTED);
    return write_reg(0x01U, 0x01U);
}

Result<void> W3u3CardMailboxHardware::shutdown_controller() noexcept {
    // Cleanup deliberately ignores caller cancellation but still uses the
    // backend's independent finite cleanup deadline.
    cleanup_mode_ = true;
    auto status = write_reg(0x04U, 0x00U);
    const auto command = write_reg(0x00U, 0x00U);
    sleep_ms(50U);
    controller_initialized_ = false;
    atr_pending_ = false;
    cleanup_mode_ = false;
    if (!status) return status;
    return command;
}

std::uint64_t W3u3CardMailboxHardware::monotonic_ms() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

void W3u3CardMailboxHardware::sleep_ms(std::uint32_t milliseconds) noexcept {
    if (interrupted()) return;
    transport_.delay_ms(milliseconds);
}

}  // namespace asicen
