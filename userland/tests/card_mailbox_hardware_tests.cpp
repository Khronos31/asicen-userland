// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/card_mailbox_hardware.h"

#include "asicen/protocol.h"
#include "asicen/write_protocol.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <map>
#include <deque>
#include <vector>

namespace {

std::uint8_t reverse_bits(std::uint8_t value);

class MailboxTransport final : public asicen::FrontendTransport {
public:
    int control(const asicen::ControlTransfer& transfer, unsigned char* response) override {
        ++calls;
        if (cancel_after_calls != 0U && calls >= cancel_after_calls) stopped = true;
        if (stopped) return -1;
        if (transfer.length == 0U || response == nullptr) return -1;
        const auto request = static_cast<std::uint8_t>(transfer.request);
        std::fill_n(response, transfer.length, 0U);
        if (fail_on_call == calls) return -1;
        if (short_on_call == calls) return static_cast<int>(transfer.length - 1U);
        if (nack_on_call == calls) {
            response[0] = 0U;
            return transfer.length;
        }
        if (request == static_cast<std::uint8_t>(asicen::Request::I2cRead)) {
            const std::uint8_t slave = static_cast<std::uint8_t>(transfer.value & 0xffU);
            const std::uint8_t reg = static_cast<std::uint8_t>(transfer.value >> 8U);
            if (slave != 0x4aU) return -1;
            response[0] = nack_reads ? 0U : 1U;
            const std::size_t count = transfer.length - 1U;
            for (std::size_t i = 0; i < count; ++i) {
                const std::uint8_t address = static_cast<std::uint8_t>(reg + i);
                if (address >= 0x40U && address < 0x80U) {
                    const std::size_t offset = static_cast<std::size_t>(page) * 64U +
                                               address - 0x40U;
                    response[i + 1U] = window[offset];
                } else {
                    if (address == 0x03U && change_length_after_low_read &&
                        ++low_reads == 2U) registers[0x03U] = changed_length_value;
                    response[i + 1U] = registers[address];
                }
            }
        } else if (request == static_cast<std::uint8_t>(asicen::Request::I2cWrite)) {
            const std::uint8_t reg = static_cast<std::uint8_t>(transfer.value >> 8U);
            const std::uint8_t value = static_cast<std::uint8_t>(transfer.index & 0xffU);
            response[0] = (nack_writes || (nack_consume && reg == 0x00U && value == 0x0cU))
                ? 0U : 1U;
            if (response[0] == 1U) {
                registers[reg] = value;
                if (reg == 0x3aU) page = value;
                if (reg >= 0x40U && reg < 0x80U) {
                    const std::size_t offset = static_cast<std::size_t>(page) * 64U +
                                               reg - 0x40U;
                    window[offset] = value;
                }
                if (reg == 0x00U && value == 0U && ++reset_writes == 2U)
                    registers[0x04U] = 0x81U;
                if (script_card && reg == 0x00U && value == 0x09U) load_atr();
                if (script_card && reg == 0x00U && value == 0x0aU) load_next_frame();
                if (reg == 0x00U && value == 0x0cU) registers[0x03U] = 0U;
            }
        } else if (request == static_cast<std::uint8_t>(asicen::Request::I2cBufferFill)) {
            response[0] = nack_writes ? 0U : 1U;
            const std::size_t offset = transfer.value & 0xffU;
            const std::size_t count = transfer.length - 1U;
            const std::array<std::uint8_t, 3> bytes{{
                static_cast<std::uint8_t>(transfer.value >> 8U),
                static_cast<std::uint8_t>(transfer.index & 0xffU),
                static_cast<std::uint8_t>(transfer.index >> 8U)}};
            for (std::size_t i = 0; i < count; ++i) staging[offset + i] = bytes[i];
        } else if (request == static_cast<std::uint8_t>(asicen::Request::I2cBufferSend)) {
            response[0] = nack_writes ? 0U : 1U;
            if (response[0] == 1U) {
                const std::size_t count = transfer.length - 1U;
                const std::uint8_t reg = staging[0];
                for (std::size_t i = 1U; i < count; ++i) {
                    const std::size_t offset = static_cast<std::size_t>(page) * 64U +
                                               reg - 0x40U + i - 1U;
                    window[offset] = staging[i];
                }
                max_staged_payload = std::max(max_staged_payload, count - 1U);
            }
        } else {
            return -1;
        }
        if (short_next) {
            short_next = false;
            return static_cast<int>(transfer.length - 1U);
        }
        return transfer.length;
    }
    void delay_ms(unsigned) override { if (cancel_on_delay) stopped = true; }
    bool cancelled() const override { return stopped; }
    bool expired() const override { return false; }

    std::array<std::uint8_t, 256> registers{};
    std::array<std::uint8_t, 512> window{};
    std::array<std::uint8_t, 64> staging{};
    unsigned calls = 0U;
    unsigned reset_writes = 0U;
    unsigned low_reads = 0U;
    unsigned fail_on_call = 0U;
    unsigned short_on_call = 0U;
    unsigned nack_on_call = 0U;
    std::size_t max_staged_payload = 0U;
    unsigned cancel_after_calls = 0U;
    std::uint8_t page = 0U;
    bool stopped = false;
    bool cancel_on_delay = false;
    bool short_next = false;
    bool nack_reads = false;
    bool nack_writes = false;
    bool nack_consume = false;
    bool change_length_after_low_read = false;
    std::uint8_t changed_length_value = 4U;
    bool script_card = false;
    std::deque<std::vector<std::uint8_t>> scripted_frames;

    void load_frame(const std::vector<std::uint8_t>& frame) {
        registers[0x04U] = 0x81U;
        registers[0x03U] = static_cast<std::uint8_t>(frame.size());
        registers[0x39U] = static_cast<std::uint8_t>((frame.size() >> 8U) & 1U);
        std::fill(window.begin(), window.end(), 0U);
        for (std::size_t i = 0; i < frame.size(); ++i)
            window[i] = reverse_bits(frame[i]);
    }
    void load_atr() {
        load_frame({0x3bU, 0xf0U, 0x12U, 0x00U, 0xffU, 0x91U, 0x81U,
                    0xb1U, 0x7cU, 0x45U, 0x1fU, 0x01U, 0x9bU});
    }
    void load_next_frame() {
        if (scripted_frames.empty()) return;
        load_frame(scripted_frames.front());
        scripted_frames.pop_front();
    }
};

std::uint8_t reverse_bits(std::uint8_t value) {
    std::uint8_t output = 0U;
    for (unsigned bit = 0; bit < 8U; ++bit)
        output = static_cast<std::uint8_t>((output << 1U) | ((value >> bit) & 1U));
    return output;
}

bool check(bool condition, const char* name) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", name);
    return condition;
}

bool test_detect_reset_and_baud() {
    MailboxTransport transport;
    asicen::W3u3CardMailboxHardware card(transport);
    const auto detected = card.detect_card();
    if (!check(detected && detected.value(), "source reset detects ready card")) return false;
    if (!check(transport.reset_writes == 2U, "detect reset write sequence")) return false;
    const auto reset = card.reset_card(card);
    if (!check(reset.has_value(), "type0f card activation")) return false;
    const auto baud = card.set_baud_rate(px4::userland::It930xCardBaudRate::baud_19200);
    if (!check(baud && transport.registers[0x01U] == 1U, "19200 baud mapping")) return false;
    const auto unsupported = card.set_baud_rate(px4::userland::It930xCardBaudRate::baud_38400);
    return check(!unsupported && unsupported.error() == px4::userland::Error::UNSUPPORTED,
                 "unsupported baud rejected");
}

bool test_complete_frame_and_consume() {
    MailboxTransport transport;
    transport.registers[0x04U] = 1U;
    transport.registers[0x03U] = 5U;
    const std::array<std::uint8_t, 5> frame{{0x00U, 0x00U, 0x01U, 0x01U, 0x00U}};
    for (std::size_t i = 0; i < frame.size(); ++i)
        transport.window[i] = reverse_bits(frame[i]);
    asicen::W3u3CardMailboxHardware card(transport);
    const auto ready = card.data_ready();
    if (!check(ready && ready.value(), "complete T1 frame ready")) return false;
    std::array<std::uint8_t, 8> output{};
    const auto read = card.read_data(px4::userland::MutableByteView{output.data(), output.size()});
    if (!check(read && read.value() == frame.size(), "read complete frame length")) return false;
    if (!check(std::equal(frame.begin(), frame.end(), output.begin()), "bit reverse frame bytes"))
        return false;
    return check(transport.registers[0x00U] == 0x0cU, "consume after complete frame");
}

bool test_invalid_logical_lengths() {
    for (const std::uint16_t length : {256U, 511U}) {
        MailboxTransport transport;
        transport.registers[0x04U] = 1U;
        transport.registers[0x03U] = static_cast<std::uint8_t>(length & 0xffU);
        transport.registers[0x39U] = static_cast<std::uint8_t>((length >> 8U) & 1U);
        asicen::W3u3CardMailboxHardware card(transport);
        const auto ready = card.data_ready();
        if (!check(!ready && ready.error() == px4::userland::Error::PROTOCOL_ERROR,
                   "oversized T1 logical length rejected")) return false;
        if (!check(transport.calls == 3U, "invalid length rejected before window reads"))
            return false;
    }
    return true;
}

bool test_page_crossing_read_and_write() {
    MailboxTransport transport;
    transport.registers[0x04U] = 1U;
    transport.registers[0x03U] = 65U;
    std::array<std::uint8_t, 65> frame{};
    frame[0] = 0U;
    frame[1] = 0U;
    frame[2] = 61U;
    std::uint8_t checksum = 0U;
    for (std::size_t i = 0; i + 1U < frame.size(); ++i) {
        if (i >= 3U) frame[i] = static_cast<std::uint8_t>(i * 7U);
        checksum = static_cast<std::uint8_t>(checksum ^ frame[i]);
    }
    frame.back() = checksum;
    for (std::size_t i = 0; i < frame.size(); ++i)
        transport.window[i] = reverse_bits(frame[i]);
    asicen::W3u3CardMailboxHardware card(transport);
    std::array<std::uint8_t, 80> output{};
    const auto read = card.read_data(px4::userland::MutableByteView{output.data(), output.size()});
    if (!check(read && read.value() == frame.size(), "65-byte frame crosses mailbox page"))
        return false;
    if (!check(std::equal(frame.begin(), frame.end(), output.begin()),
               "page-crossing read preserves frame")) return false;

    MailboxTransport write_transport;
    asicen::W3u3CardMailboxHardware writer(write_transport);
    const auto payload = writer.write_data(px4::userland::ByteView{frame.data(), frame.size()});
    if (!check(payload.has_value(), "65-byte write succeeds through two pages")) return false;
    if (!check(write_transport.registers[0x02U] == 65U &&
               write_transport.registers[0x38U] == 0U &&
               write_transport.registers[0x00U] == 0x0aU,
               "write length and submit registers")) return false;
    for (std::size_t i = 0; i < frame.size(); ++i) {
        if (write_transport.window[i] != reverse_bits(frame[i]))
            return check(false, "write bit reversal and page boundary");
    }
    return check(write_transport.max_staged_payload <= 8U,
                 "window writes capped at8 payload bytes");
}

std::vector<std::uint8_t> make_t1_frame(std::uint8_t pcb,
                                       const std::vector<std::uint8_t>& data) {
    std::vector<std::uint8_t> frame{0x00U, pcb,
        static_cast<std::uint8_t>(data.size())};
    frame.insert(frame.end(), data.begin(), data.end());
    std::uint8_t lrc = 0U;
    for (const auto byte : frame) lrc = static_cast<std::uint8_t>(lrc ^ byte);
    frame.push_back(lrc);
    return frame;
}

bool test_card_session_transmit_through_mailbox() {
    MailboxTransport transport;
    transport.script_card = true;
    transport.scripted_frames.push_back(make_t1_frame(0xe0U, {}));
    transport.scripted_frames.push_back(make_t1_frame(0xe1U, {251U}));
    std::vector<std::uint8_t> response_data(61U, 0U);
    response_data[59] = 0x90U;
    response_data[60] = 0x00U;
    transport.scripted_frames.push_back(make_t1_frame(0x00U, response_data));

    asicen::W3u3CardMailboxHardware hardware(transport);
    px4::userland::CardSession session(hardware, hardware);
    const auto initialized = session.initialize();
    if (!initialized) std::fprintf(stderr, "CardSession init error=%s calls=%u pending=%zu\n",
        px4::userland::error_string(initialized.error()), transport.calls,
        transport.scripted_frames.size());
    if (!check(initialized && session.atr().length == 13U,
               "upstream CardSession initializes through mailbox")) return false;
    const std::array<std::uint8_t, 5> apdu{{0x90U, 0x30U, 0U, 0U, 0U}};
    std::array<std::uint8_t, 80> response{};
    const auto transmitted = session.transmit(
        px4::userland::ByteView{apdu.data(), apdu.size()},
        px4::userland::MutableByteView{response.data(), response.size()});
    if (!check(transmitted && transmitted.value() == 61U,
               "initial-settings APDU crosses mailbox T1 path")) return false;
    return check(response[59] == 0x90U && response[60] == 0x00U,
                 "T1 response status word preserved");
}

bool test_short_nack_cancellation_and_changing_length() {
    {
        MailboxTransport transport;
        transport.short_next = true;
        asicen::W3u3CardMailboxHardware card(transport);
        const auto result = card.detect_card();
        if (!check(!result && result.error() == px4::userland::Error::PROTOCOL_ERROR,
                   "short transfer rejected")) return false;
        if (!check(transport.calls == 1U, "short transfer stops reset")) return false;
    }
    {
        MailboxTransport transport;
        transport.nack_writes = true;
        asicen::W3u3CardMailboxHardware card(transport);
        const auto result = card.detect_card();
        if (!check(!result && result.error() == px4::userland::Error::PROTOCOL_ERROR,
                   "I2C NACK rejected")) return false;
        if (!check(transport.calls == 1U, "NACK stops reset")) return false;
    }
    {
        MailboxTransport transport;
        transport.cancel_on_delay = true;
        asicen::W3u3CardMailboxHardware card(transport);
        const auto result = card.detect_card();
        if (!check(!result && result.error() == px4::userland::Error::TIMEOUT,
                   "cancellation stops reset")) return false;
    }
    {
        MailboxTransport transport;
        transport.nack_on_call = 3U;  // first page select
        asicen::W3u3CardMailboxHardware card(transport);
        const std::array<std::uint8_t, 1> payload{{0x5aU}};
        const auto result = card.write_data(
            px4::userland::ByteView{payload.data(), payload.size()});
        if (!check(!result && result.error() == px4::userland::Error::PROTOCOL_ERROR,
                   "page-select NACK rejected")) return false;
        if (!check(transport.calls == 3U, "page NACK stops data transfer")) return false;
    }
    {
        MailboxTransport transport;
        transport.cancel_after_calls = 3U;
        asicen::W3u3CardMailboxHardware card(transport);
        const std::array<std::uint8_t, 1> payload{{0x5aU}};
        const auto result = card.write_data(
            px4::userland::ByteView{payload.data(), payload.size()});
        if (!check(!result && result.error() == px4::userland::Error::TIMEOUT,
                   "page-select cancellation propagates")) return false;
    }
    {
        MailboxTransport transport;
        transport.nack_on_call = 5U;  // command submit after length/page/data
        asicen::W3u3CardMailboxHardware card(transport);
        const std::array<std::uint8_t, 1> payload{{0x5aU}};
        const auto result = card.write_data(
            px4::userland::ByteView{payload.data(), payload.size()});
        if (!check(!result && result.error() == px4::userland::Error::PROTOCOL_ERROR,
                   "submit NACK rejected")) return false;
        if (!check(transport.calls == 5U, "submit NACK stops wait")) return false;
    }
    {
        MailboxTransport transport;
        transport.cancel_after_calls = 5U;
        asicen::W3u3CardMailboxHardware card(transport);
        const std::array<std::uint8_t, 1> payload{{0x5aU}};
        const auto result = card.write_data(
            px4::userland::ByteView{payload.data(), payload.size()});
        if (!check(!result && result.error() == px4::userland::Error::TIMEOUT,
                   "submit cancellation propagates")) return false;
    }
    {
        MailboxTransport transport;
        transport.short_on_call = 4U;  // first staged payload transfer
        asicen::W3u3CardMailboxHardware card(transport);
        const std::array<std::uint8_t, 2> payload{{0x12U, 0x34U}};
        const auto result = card.write_data(
            px4::userland::ByteView{payload.data(), payload.size()});
        if (!check(!result && result.error() == px4::userland::Error::PROTOCOL_ERROR,
                   "short payload transfer rejected")) return false;
        if (!check(transport.calls == 4U, "short transfer stops staging")) return false;
    }
    {
        MailboxTransport transport;
        transport.registers[0x04U] = 1U;
        transport.registers[0x03U] = 5U;
        const std::array<std::uint8_t, 5> frame{{0U, 0U, 1U, 1U, 0U}};
        for (std::size_t i = 0; i < frame.size(); ++i)
            transport.window[i] = reverse_bits(frame[i]);
        transport.nack_consume = true;
        asicen::W3u3CardMailboxHardware card(transport);
        std::array<std::uint8_t, 8> output{};
        const auto result = card.read_data(
            px4::userland::MutableByteView{output.data(), output.size()});
        if (!check(!result && result.error() == px4::userland::Error::PROTOCOL_ERROR,
                   "consume NACK rejected")) return false;
        if (!check(transport.registers[0x00U] != 0x0cU,
                   "failed consume is not success")) return false;
    }
    {
        MailboxTransport transport;
        transport.registers[0x04U] = 1U;
        transport.registers[0x03U] = 5U;
        const std::array<std::uint8_t, 5> frame{{0U, 0U, 1U, 1U, 0U}};
        for (std::size_t i = 0; i < frame.size(); ++i)
            transport.window[i] = reverse_bits(frame[i]);
        transport.cancel_after_calls = 13U;  // consume after repeated header check
        asicen::W3u3CardMailboxHardware card(transport);
        std::array<std::uint8_t, 8> output{};
        const auto result = card.read_data(
            px4::userland::MutableByteView{output.data(), output.size()});
        if (!check(!result && result.error() == px4::userland::Error::TIMEOUT,
                   "consume cancellation propagates")) return false;
        if (!check(transport.registers[0x00U] != 0x0cU,
                   "cancelled consume does not claim completion")) return false;
    }
    {
        MailboxTransport transport;
        transport.registers[0x04U] = 1U;
        transport.registers[0x03U] = 5U;
        const std::array<std::uint8_t, 5> frame{{0U, 0U, 1U, 1U, 0U}};
        for (std::size_t i = 0; i < frame.size(); ++i)
            transport.window[i] = reverse_bits(frame[i]);
        transport.change_length_after_low_read = true;
        asicen::W3u3CardMailboxHardware card(transport);
        std::array<std::uint8_t, 8> output{};
        const auto read = card.read_data(px4::userland::MutableByteView{output.data(), output.size()});
        if (!check(!read && read.error() == px4::userland::Error::PROTOCOL_ERROR,
                   "changing available length rejected")) return false;
        if (!check(transport.registers[0x00U] != 0x0cU,
                   "changing frame is not consumed")) return false;
    }
    {
        MailboxTransport transport;
        transport.registers[0x04U] = 1U;
        transport.registers[0x03U] = 5U;
        const std::array<std::uint8_t, 5> frame{{0U, 0U, 1U, 1U, 0U}};
        for (std::size_t i = 0; i < frame.size(); ++i)
            transport.window[i] = reverse_bits(frame[i]);
        transport.change_length_after_low_read = true;
        transport.changed_length_value = 6U;
        asicen::W3u3CardMailboxHardware card(transport);
        std::array<std::uint8_t, 8> output{};
        const auto read = card.read_data(
            px4::userland::MutableByteView{output.data(), output.size()});
        if (!check(!read && read.error() == px4::userland::Error::PROTOCOL_ERROR,
                   "growing available length rejected")) return false;
        if (!check(transport.registers[0x00U] != 0x0cU,
                   "growth does not consume incomplete response")) return false;
    }
    return true;
}

}  // namespace

int main() {
    return test_detect_reset_and_baud() && test_complete_frame_and_consume() &&
           test_invalid_logical_lengths() &&
           test_page_crossing_read_and_write() &&
           test_card_session_transmit_through_mailbox() &&
           test_short_nack_cancellation_and_changing_length() ? 0 : 1;
}
