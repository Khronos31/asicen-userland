// SPDX-License-Identifier: GPL-2.0-only
#ifndef ASICEN_USERLAND_LINK_SEED_DIAGNOSTIC_H
#define ASICEN_USERLAND_LINK_SEED_DIAGNOSTIC_H

#include <cstddef>
#include <cstdint>
#include <array>

namespace asicen {

class LinkSeedDiagnosticIo {
  public:
    virtual ~LinkSeedDiagnosticIo() = default;
    virtual bool read_controller05(std::uint8_t* value) = 0;
    virtual bool write_controller05(std::uint8_t value) = 0;
    virtual bool write_link_seed_byte(std::uint8_t reg, std::uint8_t value) = 0;
};

enum class LinkSeedSnapshotResult : std::uint8_t {
    ready,
    unavailable,
    read_failed,
    controller_active,
};

// The chip's seed write window is not observable through the recovered I2C
// read path. This diagnostic requires controller05=0, applies a seed, then
// sends zero writes and verifies only controller05=0. It never claims that
// the prior seed was saved or that the zero writes erased the hardware latch.
class LinkSeedDiagnostic final {
  public:
    bool snapshot_idle(LinkSeedDiagnosticIo* io);
    LinkSeedSnapshotResult snapshot_idle_result(LinkSeedDiagnosticIo* io);
    bool apply(LinkSeedDiagnosticIo* io, const std::uint8_t* seed, std::size_t size);
    bool clear_and_verify_controller(LinkSeedDiagnosticIo* io);
    bool active() const
    {
        return active_;
    }

  private:
    bool active_ = false;
};

enum class LinkSeedLaneRelease : std::uint8_t {
    released,
    peer_active,
    cleanup_failed,
};

class LinkSeedCaptureCoordinator final {
  public:
    bool reserve(LinkSeedDiagnosticIo* io, std::uint8_t lane);
    bool apply(LinkSeedDiagnosticIo* io, std::uint8_t lane, const std::uint8_t* seed,
               std::size_t size);
    LinkSeedLaneRelease release(LinkSeedDiagnosticIo* io, std::uint8_t lane);
    void quarantine() { quarantined_ = true; }
    bool active(std::uint8_t lane) const;
    bool quarantined() const { return quarantined_; }
    const std::array<std::uint8_t, 16>& seed() const { return seed_; }

  private:
    LinkSeedDiagnostic diagnostic_;
    std::array<std::uint8_t, 16> seed_{};
    std::uint8_t lane_mask_ = 0U;
    bool seed_apply_attempted_ = false;
    bool seed_active_ = false;
    bool quarantined_ = false;
};

} // namespace asicen

#endif // ASICEN_USERLAND_LINK_SEED_DIAGNOSTIC_H
