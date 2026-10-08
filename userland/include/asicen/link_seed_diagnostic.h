// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstddef>
#include <cstdint>

namespace asicen {

class LinkSeedDiagnosticIo {
public:
    virtual ~LinkSeedDiagnosticIo() = default;
    virtual bool read_controller05(std::uint8_t* value) = 0;
    virtual bool write_controller05(std::uint8_t value) = 0;
    virtual bool write_link_seed_byte(std::uint8_t reg, std::uint8_t value) = 0;
};

// The chip's seed write window is not observable through the recovered I2C
// read path. This diagnostic requires controller05=0, applies a seed, then
// sends zero writes and verifies only controller05=0. It never claims that
// the prior seed was saved or that the zero writes erased the hardware latch.
class LinkSeedDiagnostic final {
public:
    bool snapshot_idle(LinkSeedDiagnosticIo* io);
    bool apply(LinkSeedDiagnosticIo* io, const std::uint8_t* seed,
               std::size_t size);
    bool clear_and_verify_controller(LinkSeedDiagnosticIo* io);
    bool active() const { return active_; }

private:
    bool active_ = false;
};

}  // namespace asicen
