#include "asicen/ts_framer.h"

#include <algorithm>

namespace asicen {

bool ts_sync_candidate(std::uint8_t value, TransportMode mode) {
    if (mode == TransportMode::AsicenMode0 ||
        mode == TransportMode::AsicenMode3) {
        return (value & 0x7fU) == 0x47U;
    }
    return value == 0x47U;
}

void normalize_ts_sync_byte(std::uint8_t* packet, TransportMode mode) {
    if (packet == nullptr) {
        return;
    }
    if (mode == TransportMode::AsicenMode0 ||
        mode == TransportMode::AsicenMode3) {
        packet[0] &= 0x7fU;
    }
}

bool find_ts_alignment(const std::uint8_t* data,
                       std::size_t size,
                       TransportMode mode,
                       std::size_t* offset) {
    if (data == nullptr || offset == nullptr) {
        return false;
    }
    const std::size_t probe_span =
        (kTsSyncProbePackets - 1U) * kMpegTsPacketSize + 1U;
    if (size < probe_span) {
        return false;
    }

    const std::size_t max_offset = size - probe_span;
    for (std::size_t candidate = 0; candidate <= max_offset; ++candidate) {
        bool good = true;
        for (std::size_t packet = 0; packet < kTsSyncProbePackets; ++packet) {
            if (!ts_sync_candidate(
                    data[candidate + packet * kMpegTsPacketSize], mode)) {
                good = false;
                break;
            }
        }
        if (good) {
            *offset = candidate;
            return true;
        }
    }
    return false;
}

TsFramer::TsFramer(TransportMode mode, std::size_t max_pending_bytes)
    : mode_(mode),
      max_pending_bytes_(
          std::max(max_pending_bytes,
                   kTsSyncProbePackets * kMpegTsPacketSize)) {}

void TsFramer::trim_unsynchronized() {
    if (pending_.size() <= max_pending_bytes_) {
        return;
    }

    const std::size_t keep =
        std::min(max_pending_bytes_,
                 kTsSyncProbePackets * kMpegTsPacketSize);
    const std::size_t discard = pending_.size() - keep;
    discarded_bytes_ += discard;
    pending_.erase(pending_.begin(),
                   pending_.begin() + static_cast<std::ptrdiff_t>(discard));
}

std::vector<std::uint8_t> TsFramer::push(const std::uint8_t* data,
                                         std::size_t size) {
    if (data == nullptr || size == 0) {
        return {};
    }

    pending_.insert(pending_.end(), data, data + size);

    if (!synchronized_) {
        std::size_t offset = 0;
        if (!find_ts_alignment(pending_.data(), pending_.size(), mode_, &offset)) {
            trim_unsynchronized();
            return {};
        }
        if (offset != 0) {
            discarded_bytes_ += offset;
            pending_.erase(
                pending_.begin(),
                pending_.begin() + static_cast<std::ptrdiff_t>(offset));
        }
        synchronized_ = true;
    }

    std::vector<std::uint8_t> output;
    while (pending_.size() >= kMpegTsPacketSize) {
        if (!ts_sync_candidate(pending_[0], mode_)) {
            synchronized_ = false;
            std::size_t offset = 0;
            if (find_ts_alignment(pending_.data(), pending_.size(), mode_, &offset) &&
                offset != 0) {
                discarded_bytes_ += offset;
                pending_.erase(
                    pending_.begin(),
                    pending_.begin() + static_cast<std::ptrdiff_t>(offset));
                synchronized_ = true;
                continue;
            }
            // Retain enough tail for a future 8-packet alignment proof.
            trim_unsynchronized();
            break;
        }

        const std::size_t begin = output.size();
        output.insert(output.end(),
                      pending_.begin(),
                      pending_.begin() +
                          static_cast<std::ptrdiff_t>(kMpegTsPacketSize));
        normalize_ts_sync_byte(output.data() + begin, mode_);
        pending_.erase(
            pending_.begin(),
            pending_.begin() +
                static_cast<std::ptrdiff_t>(kMpegTsPacketSize));
    }
    return output;
}

void TsFramer::reset() {
    pending_.clear();
    synchronized_ = false;
    discarded_bytes_ = 0;
}

bool TsFramer::synchronized() const {
    return synchronized_;
}

std::size_t TsFramer::pending_bytes() const {
    return pending_.size();
}

std::uint64_t TsFramer::discarded_bytes() const {
    return discarded_bytes_;
}

}  // namespace asicen
