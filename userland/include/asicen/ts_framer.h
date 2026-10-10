#ifndef ASICEN_USERLAND_TS_FRAMER_H
#define ASICEN_USERLAND_TS_FRAMER_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include "px4/error.h"
#include "px4/transport.h"

namespace asicen {

constexpr std::size_t kMpegTsPacketSize = 188;
constexpr std::size_t kTsSyncProbePackets = 4;

enum class TransportMode : std::uint8_t {
    Plain = 0xff,
    AsicenMode0 = 0,
    AsicenMode3 = 3,
    AsicenMode7 = 7,
};

bool ts_sync_candidate(std::uint8_t value, TransportMode mode);
void normalize_ts_sync_byte(std::uint8_t* packet, TransportMode mode);

// Finds an offset with kTsSyncProbePackets consecutive 188-byte sync bytes.
// For modes 0/3, 0xc7 is accepted as the marked form of 0x47.
bool find_ts_alignment(const std::uint8_t* data, std::size_t size, TransportMode mode,
                       std::size_t* offset);

class TsFramer final {
public:
    struct Counters final {
        std::size_t input_bytes_accepted;
        std::size_t emitted_packets;
        std::size_t discarded_sync_search_bytes;
        std::size_t sync_loss_events;
        std::size_t buffered_bytes;
    };
    explicit TsFramer(TransportMode mode,
                      std::size_t max_pending_bytes = 1048576U +
                                                      kMpegTsPacketSize * kTsSyncProbePackets);

    using PacketSink = px4::userland::Result<void> (*)(void*, px4::userland::ByteView) noexcept;
    px4::userland::Result<void> push(px4::userland::ByteView input, PacketSink sink,
                                     void* context) noexcept;
    Counters counters() const noexcept;
    bool valid() const noexcept { return pending_ != nullptr; }
    // Input is copied only when it fits the bounded carry buffer. A rejected
    // sink leaves its packet and all later bytes intact; an empty push retries.

    void reset();
    bool synchronized() const;
    std::size_t pending_bytes() const;
    std::uint64_t discarded_bytes() const;
    std::uint64_t sync_loss_events() const;

private:
    bool acquire_sync();
    void discard_bytes(std::size_t size);

    TransportMode mode_;
    std::size_t max_pending_bytes_;
    std::unique_ptr<std::uint8_t[]> pending_;
    std::size_t pending_offset_ = 0U;
    std::size_t pending_size_ = 0U;
    bool synchronized_ = false;
    std::uint64_t discarded_bytes_ = 0;
    std::uint64_t sync_loss_events_ = 0U;
    std::size_t input_bytes_accepted_ = 0U;
    std::size_t emitted_packets_ = 0U;
};

}  // namespace asicen

#endif  // ASICEN_USERLAND_TS_FRAMER_H
