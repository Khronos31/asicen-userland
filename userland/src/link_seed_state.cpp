// SPDX-License-Identifier: GPL-2.0-only
#include "asicen/link_seed_state.h"

#include <algorithm>

namespace asicen {

LinkSeedState::~LinkSeedState() {
    original_seed_.fill(0);
    original_controller05_ = 0;
}

bool LinkSeedState::snapshot(LinkSeedStateIo* io) {
    if (io == nullptr || snapshotted_) return false;
    std::uint8_t controller = 0;
    std::array<std::uint8_t, 16> seed{};
    if (!io->read_controller05(&controller) ||
        !io->read_link_seed(seed.data(), seed.size()))
        return false;
    original_controller05_ = controller;
    original_seed_ = seed;
    snapshotted_ = true;
    return true;
}

bool LinkSeedState::apply(LinkSeedStateIo* io, const std::uint8_t* seed,
                          std::size_t size) {
    if (io == nullptr || !snapshotted_ || seed == nullptr || size != 16)
        return false;
    for (std::size_t i = 0; i < size; ++i) {
        if (!io->write_link_seed_byte(static_cast<std::uint8_t>(0x10U + i), seed[i]))
            return false;
    }
    if (!io->write_controller05(0xa0U)) return false;
    std::array<std::uint8_t, 16> readback{};
    std::uint8_t controller = 0;
    const bool seed_readback_ok = io->read_link_seed(readback.data(), readback.size());
    const bool controller_readback_ok = io->read_controller05(&controller);
    return seed_readback_ok && controller_readback_ok &&
           std::equal(readback.begin(), readback.end(), seed) && controller == 0xa0U;
}

bool LinkSeedState::restore_and_verify(LinkSeedStateIo* io) {
    if (io == nullptr || !snapshotted_) return false;
    bool writes_ok = true;
    for (std::size_t i = 0; i < original_seed_.size(); ++i) {
        if (!io->write_link_seed_byte(static_cast<std::uint8_t>(0x10U + i),
                                      original_seed_[i]))
            writes_ok = false;
    }
    if (!io->write_controller05(original_controller05_)) writes_ok = false;

    std::uint8_t controller = 0;
    std::array<std::uint8_t, 16> seed{};
    const bool seed_readback_ok = io->read_link_seed(seed.data(), seed.size());
    const bool controller_readback_ok = io->read_controller05(&controller);
    const bool readback_ok = seed_readback_ok && controller_readback_ok;
    const bool matches = readback_ok && seed == original_seed_ &&
                         controller == original_controller05_;
    const bool restored = writes_ok && matches;
    if (restored) {
        snapshotted_ = false;
        original_seed_.fill(0);
        original_controller05_ = 0;
    }
    return restored;
}

}  // namespace asicen
