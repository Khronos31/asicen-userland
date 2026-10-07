#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace asicen {

struct CardMailboxFacts {
    static constexpr std::uint8_t kCommandRegister = 0x00;
    static constexpr std::uint8_t kLengthLowRegister = 0x02;
    static constexpr std::uint8_t kStatusLowRegister = 0x03;
    static constexpr std::uint8_t kStatusHighRegister = 0x04;
    static constexpr std::uint8_t kExtendedStatusRegister = 0x39;
    static constexpr std::uint8_t kPageRegister = 0x3a;
    static constexpr std::uint8_t kDataWindowBase = 0x40;
    static constexpr std::size_t kDataWindowSize = 0x40;
    static constexpr std::uint8_t kSubmitCommand = 0x0a;
    static constexpr std::uint8_t kConsumeCommand = 0x0c;
};

struct CardMailboxChunk {
    std::uint8_t page = 0;
    std::uint8_t register_address = CardMailboxFacts::kDataWindowBase;
    std::size_t buffer_offset = 0;
    std::size_t length = 0;
};

// Produces the page/window chunks used by the historical BCAS mailbox.
// This is transport-independent; it does not perform any I2C writes.
std::vector<CardMailboxChunk> build_card_mailbox_chunks(std::size_t length);

std::uint16_t decode_card_mailbox_length(std::uint8_t low,
                                         std::uint8_t high_bit_source);

}  // namespace asicen
