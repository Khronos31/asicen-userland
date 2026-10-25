// SPDX-License-Identifier: GPL-2.0-only
#include "asicen/link_seed_diagnostic.h"

namespace asicen {

bool LinkSeedDiagnostic::snapshot_idle(LinkSeedDiagnosticIo* io)
{
    return snapshot_idle_result(io) == LinkSeedSnapshotResult::ready;
}

LinkSeedSnapshotResult LinkSeedDiagnostic::snapshot_idle_result(LinkSeedDiagnosticIo* io)
{
    if (io == nullptr || active_)
        return LinkSeedSnapshotResult::unavailable;
    std::uint8_t controller = 0xff;
    if (!io->read_controller05(&controller))
        return LinkSeedSnapshotResult::read_failed;
    if (controller != 0x00)
        return LinkSeedSnapshotResult::controller_active;
    active_ = true;
    return LinkSeedSnapshotResult::ready;
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

bool LinkSeedCaptureCoordinator::reserve(LinkSeedDiagnosticIo* io, std::uint8_t lane)
{
    if (lane >= 4U || quarantined_ || (lane_mask_ & (1U << lane)) != 0U) return false;
    if (lane_mask_ == 0U) {
        if (diagnostic_.snapshot_idle_result(io) != LinkSeedSnapshotResult::ready) return false;
    }
    lane_mask_ |= static_cast<std::uint8_t>(1U << lane);
    return true;
}

bool LinkSeedCaptureCoordinator::apply(LinkSeedDiagnosticIo* io, std::uint8_t lane,
                                       const std::uint8_t* seed, std::size_t size)
{
    if (!active(lane) || io == nullptr || seed == nullptr || size != seed_.size() ||
        quarantined_) return false;
    if (seed_active_) return true;
    seed_apply_attempted_ = true;
    seed_ = {};
    for (std::size_t index = 0U; index < seed_.size(); ++index) seed_[index] = seed[index];
    if (diagnostic_.apply(io, seed_.data(), seed_.size())) {
        seed_active_ = true;
        return true;
    }
    return false;
}

LinkSeedLaneRelease LinkSeedCaptureCoordinator::release(LinkSeedDiagnosticIo* io,
                                                         std::uint8_t lane)
{
    if (!active(lane)) return quarantined_ ? LinkSeedLaneRelease::cleanup_failed
                                           : LinkSeedLaneRelease::released;
    lane_mask_ &= static_cast<std::uint8_t>(~(1U << lane));
    if (lane_mask_ != 0U) return LinkSeedLaneRelease::peer_active;
    bool clean = true;
    if (seed_apply_attempted_ && !quarantined_) {
        clean = diagnostic_.clear_and_verify_controller(io);
    }
    if (!clean) quarantined_ = true;
    if (!quarantined_) {
        diagnostic_ = LinkSeedDiagnostic{};
        seed_.fill(0U);
        seed_apply_attempted_ = false;
        seed_active_ = false;
    }
    return clean && !quarantined_ ? LinkSeedLaneRelease::released
                                  : LinkSeedLaneRelease::cleanup_failed;
}

bool LinkSeedCaptureCoordinator::active(std::uint8_t lane) const
{
    return lane < 4U && (lane_mask_ & (1U << lane)) != 0U;
}

} // namespace asicen
