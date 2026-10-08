// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace asicen {

class LinkSeedStateIo {
public:
    virtual ~LinkSeedStateIo() = default;
    virtual bool read_controller05(std::uint8_t* value) = 0;
    virtual bool read_link_seed(std::uint8_t* values, std::size_t size) = 0;
    virtual bool write_controller05(std::uint8_t value) = 0;
    virtual bool write_link_seed_byte(std::uint8_t reg, std::uint8_t value) = 0;
};

// Snapshots and reversibly applies the user-supplied 16-byte link seed. This
// state object intentionally has no logging or serialization API.
class LinkSeedState final {
public:
    ~LinkSeedState();
    bool snapshot(LinkSeedStateIo* io);
    bool apply(LinkSeedStateIo* io, const std::uint8_t* seed, std::size_t size);
    bool restore_and_verify(LinkSeedStateIo* io);
    bool snapshotted() const { return snapshotted_; }

private:
    std::array<std::uint8_t, 16> original_seed_{};
    std::uint8_t original_controller05_ = 0;
    bool snapshotted_ = false;
};

}  // namespace asicen
