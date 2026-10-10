// SPDX-License-Identifier: GPL-2.0-only
#include "asicen/link_seed_diagnostic.h"

namespace asicen {

bool LinkSeedDiagnostic::snapshot_idle(LinkSeedDiagnosticIo* io)
{
    if (io == nullptr || active_)
        return false;
    std::uint8_t controller = 0xff;
    if (!io->read_controller05(&controller) || controller != 0x00)
        return false;
    active_ = true;
    return true;
}

bool LinkSeedDiagnostic::apply(LinkSeedDiagnosticIo* io, const std::uint8_t* seed, std::size_t size)
{
    if (io == nullptr || !active_ || seed == nullptr || size != 16)
        return false;
    for (std::size_t i = 0; i < size; ++i) {
        if (!io->write_link_seed_byte(static_cast<std::uint8_t>(0x10U + i), seed[i]))
            return false;
    }
    if (!io->write_controller05(0xa0U))
        return false;
    std::uint8_t controller = 0;
    return io->read_controller05(&controller) && controller == 0xa0U;
}

bool LinkSeedDiagnostic::clear_and_verify_controller(LinkSeedDiagnosticIo* io)
{
    if (io == nullptr || !active_)
        return false;
    bool writes_ok = true;
    for (std::uint8_t reg = 0x10; reg <= 0x1f; ++reg) {
        if (!io->write_link_seed_byte(reg, 0))
            writes_ok = false;
    }
    if (!io->write_controller05(0x00))
        writes_ok = false;
    std::uint8_t controller = 0xff;
    const bool controller_ok = io->read_controller05(&controller) && controller == 0x00;
    const bool cleaned = writes_ok && controller_ok;
    if (cleaned)
        active_ = false;
    return cleaned;
}

} // namespace asicen
