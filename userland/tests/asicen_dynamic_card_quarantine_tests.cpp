// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/card_mailbox_hardware.h"
#include "asicen/card_only_service.h"
#include "px4/card_service.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdint>

namespace {
using namespace px4::userland;
using namespace px4::userland::ipc;

#define CHECK(condition)                                                            \
    do {                                                                            \
        if (!(condition)) {                                                         \
            std::fprintf(stderr, "check failed %s:%d: %s\n", __FILE__, __LINE__,  \
                         #condition);                                               \
            return false;                                                           \
        }                                                                           \
    } while (0)

std::uint8_t reverse_bits(std::uint8_t value)
{
    std::uint8_t result = 0U;
    for (unsigned bit = 0U; bit < 8U; ++bit)
        result = static_cast<std::uint8_t>((result << 1U) | ((value >> bit) & 1U));
    return result;
}

class Mailbox final : public asicen::FrontendTransport {
public:
    int control(const asicen::ControlTransfer& transfer, unsigned char* output) override
    {
        if (output == nullptr || transfer.length == 0U) return -1;
        ++calls;
        std::fill_n(output, transfer.length, 0U);
        if (transfer.request == asicen::Request::I2cRead) {
            const auto reg = static_cast<std::uint8_t>(transfer.value >> 8U);
            if (static_cast<std::uint8_t>(transfer.value) != 0x4aU) return -1;
            output[0] = 1U;
            for (std::size_t i = 0U; i + 1U < transfer.length; ++i) {
                const auto address = static_cast<std::uint8_t>(reg + i);
                output[i + 1U] = address >= 0x40U && address < 0x80U
                    ? window[static_cast<std::size_t>(page) * 64U + address - 0x40U]
                    : registers[address];
            }
            return transfer.length;
        }
        if (transfer.request == asicen::Request::I2cWrite) {
            const auto reg = static_cast<std::uint8_t>(transfer.value >> 8U);
            const auto value = static_cast<std::uint8_t>(transfer.index);
            output[0] = fail_cleanup && (reg == 0x04U || reg == 0x00U) ? 0U : 1U;
            if (output[0] == 0U) return transfer.length;
            registers[reg] = value;
            if (reg == 0x3aU) page = value;
            if (reg == 0x00U && value == 0x80U) ++reset_prepares;
            if (reg == 0x00U && value == 0U && reset_prepares != 0U) {
                ++controller_inits;
                registers[0x04U] = 0x81U;
            }
            if (reg == 0x00U && value == 0x09U) load_atr();
            if (reg == 0x00U && value == 0x0cU) registers[0x03U] = 0U;
            return transfer.length;
        }
        if (transfer.request == asicen::Request::I2cBufferFill) {
            output[0] = 1U;
            const std::size_t offset = transfer.value & 0xffU;
            const std::array<std::uint8_t, 3U> bytes{{
                static_cast<std::uint8_t>(transfer.value >> 8U),
                static_cast<std::uint8_t>(transfer.index),
                static_cast<std::uint8_t>(transfer.index >> 8U)}};
            for (std::size_t i = 0U; i + 1U < transfer.length; ++i)
                staging[offset + i] = bytes[i];
            return transfer.length;
        }
        if (transfer.request == asicen::Request::I2cBufferSend) {
            output[0] = 1U;
            const auto reg = staging[0];
            for (std::size_t i = 1U; i + 1U < transfer.length; ++i) {
                const std::size_t offset = static_cast<std::size_t>(page) * 64U +
                                           reg - 0x40U + i - 1U;
                window[offset] = staging[i];
            }
            return transfer.length;
        }
        return -1;
    }
    void delay_ms(unsigned) override {}
    bool cancelled() const override { return false; }
    bool expired() const override { return false; }

    void load_atr()
    {
        const std::array<std::uint8_t, 13U> atr{{
            0x3bU, 0xf0U, 0x12U, 0x00U, 0xffU, 0x91U, 0x81U,
            0xb1U, 0x7cU, 0x45U, 0x1fU, 0x01U, 0x9bU}};
        registers[0x04U] = 0x81U;
        registers[0x03U] = static_cast<std::uint8_t>(atr.size());
        std::fill(window.begin(), window.end(), 0U);
        for (std::size_t i = 0U; i < atr.size(); ++i) window[i] = reverse_bits(atr[i]);
    }

    std::array<std::uint8_t, 256U> registers{};
    std::array<std::uint8_t, 512U> window{};
    std::array<std::uint8_t, 64U> staging{};
    std::atomic<unsigned> calls{0U};
    unsigned reset_prepares = 0U;
    unsigned controller_inits = 0U;
    std::uint8_t page = 0U;
    bool fail_cleanup = false;
};

class OperationGuard final : public asicen::CardOperationGuard {
public:
    Result<void> begin_card_operation(std::uint32_t timeout_ms,
                                      const volatile std::sig_atomic_t*) noexcept override
    {
        if (quarantined || timeout_ms == 0U)
            return Result<void>::failure(Error::NOT_READY);
        ++operations;
        return Result<void>::success();
    }
    void end_card_operation() noexcept override {}
    Result<void> begin_card_cleanup(std::uint32_t timeout_ms) noexcept override
    {
        if (quarantined || timeout_ms == 0U)
            return Result<void>::failure(Error::USB_IO);
        ++cleanups;
        return Result<void>::success();
    }
    void end_card_cleanup(bool success) noexcept override
    {
        if (!success) quarantined = true;
    }
    void request_card_stop() noexcept override {}

    unsigned operations = 0U;
    unsigned cleanups = 0U;
    bool quarantined = false;
};

bool test_failed_cleanup_quarantines_and_refuses_retry()
{
    Mailbox transport;
    transport.fail_cleanup = true;
    asicen::W3u3CardMailboxHardware mailbox(transport);
    CardSession raw_session(mailbox, mailbox);
    OperationGuard guard;
    asicen::W3u3CardServiceBackend backend(mailbox, guard, nullptr);
    asicen::W3u3CardProtocolSession session(raw_session, guard, nullptr);
    CardService card(backend, session);

    const auto connected = card.connect(77U, ShareMode::shared);
    CHECK(!connected && connected.error() == Error::PROTOCOL_ERROR);
    CHECK(guard.quarantined);
    CHECK(guard.operations != 0U && guard.cleanups != 0U);
    CHECK(transport.calls.load() != 0U);
    const unsigned calls_before_retry = transport.calls.load();
    const auto retry = backend.detect_card();
    CHECK(!retry && retry.error() == Error::NOT_READY);
    CHECK(transport.calls.load() == calls_before_retry);
    return true;
}
}  // namespace

int main()
{
    if (!test_failed_cleanup_quarantines_and_refuses_retry()) return 1;
    std::puts("PASS card backend/session cleanup quarantine");
    return 0;
}
