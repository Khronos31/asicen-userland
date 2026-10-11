#include "asicen/card_mailbox.h"

#include <algorithm>

namespace asicen {

std::vector<CardMailboxChunk> build_card_mailbox_chunks(std::size_t length)
{
    if (length > CardMailboxFacts::kDataWindowSize * 256U) {
        return {};
    }
    std::vector<CardMailboxChunk> result;
    std::size_t offset = 0;
    while (offset < length) {
        const std::size_t within_page = offset % CardMailboxFacts::kDataWindowSize;
        const std::size_t room = CardMailboxFacts::kDataWindowSize - within_page;
        const std::size_t chunk = std::min(room, length - offset);

        result.push_back(CardMailboxChunk{
            static_cast<std::uint8_t>(offset / CardMailboxFacts::kDataWindowSize),
            static_cast<std::uint8_t>(CardMailboxFacts::kDataWindowBase + within_page),
            offset,
            chunk,
        });
        offset += chunk;
    }
    return result;
}

std::vector<CardMailboxChunk> build_card_mailbox_io_chunks(std::size_t length)
{
    if (length > 511U) {
        return {};
    }
    std::vector<CardMailboxChunk> result;
    for (const CardMailboxChunk& page : build_card_mailbox_chunks(length)) {
        std::size_t offset = 0U;
        while (offset < page.length) {
            const std::size_t chunk = std::min<std::size_t>(8U, page.length - offset);
            result.push_back(CardMailboxChunk{
                page.page,
                static_cast<std::uint8_t>(page.register_address + offset),
                page.buffer_offset + offset,
                chunk,
            });
            offset += chunk;
        }
    }
    return result;
}

std::uint16_t decode_card_mailbox_length(std::uint8_t low, std::uint8_t high_bit_source)
{
    return static_cast<std::uint16_t>(low) |
           (static_cast<std::uint16_t>(high_bit_source & 0x01U) << 8U);
}

}  // namespace asicen
