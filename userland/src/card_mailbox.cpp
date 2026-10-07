#include "asicen/card_mailbox.h"

#include <algorithm>

namespace asicen {

std::vector<CardMailboxChunk> build_card_mailbox_chunks(std::size_t length) {
    std::vector<CardMailboxChunk> result;
    std::size_t offset = 0;
    while (offset < length) {
        const std::size_t within_page =
            offset % CardMailboxFacts::kDataWindowSize;
        const std::size_t room =
            CardMailboxFacts::kDataWindowSize - within_page;
        const std::size_t chunk = std::min(room, length - offset);

        result.push_back(CardMailboxChunk{
            static_cast<std::uint8_t>(offset / CardMailboxFacts::kDataWindowSize),
            static_cast<std::uint8_t>(
                CardMailboxFacts::kDataWindowBase + within_page),
            offset,
            chunk,
        });
        offset += chunk;
    }
    return result;
}

std::uint16_t decode_card_mailbox_length(std::uint8_t low,
                                         std::uint8_t high_bit_source) {
    return static_cast<std::uint16_t>(low) |
           (static_cast<std::uint16_t>(high_bit_source & 0x01U) << 8U);
}

}  // namespace asicen
