#include "asicen/ts_framer.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <new>

namespace asicen {

bool ts_sync_candidate(std::uint8_t value, TransportMode mode)
{
    if (mode == TransportMode::AsicenMode0 || mode == TransportMode::AsicenMode3) {
        return (value & 0x7fU) == 0x47U;
    }
    return value == 0x47U;
}

void normalize_ts_sync_byte(std::uint8_t* packet, TransportMode mode)
{
    if (packet == nullptr) {
        return;
    }
    if (mode == TransportMode::AsicenMode0 || mode == TransportMode::AsicenMode3) {
        packet[0] &= 0x7fU;
    }
}

bool find_ts_alignment(const std::uint8_t* data, std::size_t size, TransportMode mode,
                       std::size_t* offset)
{
    if (data == nullptr || offset == nullptr) {
        return false;
    }
    const std::size_t probe_span = kTsSyncProbePackets * kMpegTsPacketSize;
    if (size < probe_span) {
        return false;
    }

    const std::size_t max_offset = size - probe_span;
    for (std::size_t candidate = 0; candidate <= max_offset; ++candidate) {
        bool good = true;
        for (std::size_t packet = 0; packet < kTsSyncProbePackets; ++packet) {
            if (!ts_sync_candidate(data[candidate + packet * kMpegTsPacketSize], mode)) {
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
          std::max(std::min(max_pending_bytes, 1048576U + kTsSyncProbePackets * kMpegTsPacketSize),
                   kTsSyncProbePackets * kMpegTsPacketSize)),
      pending_(new (std::nothrow) std::uint8_t[max_pending_bytes_])
{
}

void TsFramer::discard_bytes(std::size_t size)
{
    pending_offset_ += size;
    pending_size_ -= size;
    discarded_bytes_ += size;
}

bool TsFramer::acquire_sync()
{
    const std::size_t required = kTsSyncProbePackets * kMpegTsPacketSize;
    if (pending_size_ < required) {
        return false;
    }
    std::size_t offset = 0U;
    if (find_ts_alignment(pending_.get() + pending_offset_, pending_size_, mode_, &offset)) {
        discard_bytes(offset);
        return true;
    }
    discard_bytes(pending_size_ - (required - 1U));
    return false;
}

px4::userland::Result<void> TsFramer::push(px4::userland::ByteView input, PacketSink sink,
                                           void* context) noexcept
{
    using px4::userland::Error;
    using px4::userland::Result;
    const auto* data = input.data;
    const auto size = input.size;
    if (sink == nullptr || (size != 0U && data == nullptr) || size > 1048576U) {
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    }
    if (pending_ == nullptr) {
        return Result<void>::failure(Error::INTERNAL);
    }
    if (pending_size_ > max_pending_bytes_ || size > max_pending_bytes_ - pending_size_) {
        return Result<void>::failure(Error::SLOW_CONSUMER);
    }
    if (size != 0U) {
        if (size > max_pending_bytes_ - pending_offset_ - pending_size_) {
            std::memmove(pending_.get(), pending_.get() + pending_offset_, pending_size_);
            pending_offset_ = 0U;
        }
        std::memcpy(pending_.get() + pending_offset_ + pending_size_, data, size);
        pending_size_ += size;
        input_bytes_accepted_ += size;
    }
    while (true) {
        if (!synchronized_) {
            if (!acquire_sync()) {
                return Result<void>::success();
            }
            synchronized_ = true;
        }
        if (pending_size_ < kMpegTsPacketSize) {
            return Result<void>::success();
        }
        if (!ts_sync_candidate(pending_[pending_offset_], mode_)) {
            synchronized_ = false;
            ++sync_loss_events_;
            discard_bytes(1U);
            continue;
        }
        std::array<std::uint8_t, kMpegTsPacketSize> packet{};
        std::memcpy(packet.data(), pending_.get() + pending_offset_, packet.size());
        normalize_ts_sync_byte(packet.data(), mode_);
        const auto result = sink(context, {packet.data(), packet.size()});
        if (!result) {
            return result;
        }
        ++emitted_packets_;
        pending_offset_ += kMpegTsPacketSize;
        pending_size_ -= kMpegTsPacketSize;
    }
}

TsFramer::Counters TsFramer::counters() const noexcept
{
    return {input_bytes_accepted_, emitted_packets_, static_cast<std::size_t>(discarded_bytes_),
            static_cast<std::size_t>(sync_loss_events_), pending_size_};
}

void TsFramer::reset()
{
    pending_offset_ = 0U;
    pending_size_ = 0U;
    synchronized_ = false;
    discarded_bytes_ = 0;
    sync_loss_events_ = 0U;
    input_bytes_accepted_ = 0U;
    emitted_packets_ = 0U;
}

bool TsFramer::synchronized() const
{
    return synchronized_;
}

std::size_t TsFramer::pending_bytes() const
{
    return pending_size_;
}

std::uint64_t TsFramer::discarded_bytes() const
{
    return discarded_bytes_;
}

std::uint64_t TsFramer::sync_loss_events() const
{
    return sync_loss_events_;
}

}  // namespace asicen
