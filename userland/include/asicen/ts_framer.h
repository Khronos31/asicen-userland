#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace asicen {

constexpr std::size_t kMpegTsPacketSize = 188;
constexpr std::size_t kTsSyncProbePackets = 8;

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
bool find_ts_alignment(const std::uint8_t* data,
                       std::size_t size,
                       TransportMode mode,
                       std::size_t* offset);

class TsFramer final {
public:
    explicit TsFramer(TransportMode mode,
                      std::size_t max_pending_bytes = kMpegTsPacketSize * 64);

    // Appends arbitrary raw USB bytes and returns complete, framing-normalized
    // TS packets. Payload transformation/decryption is intentionally separate.
    std::vector<std::uint8_t> push(const std::uint8_t* data, std::size_t size);

    void reset();
    bool synchronized() const;
    std::size_t pending_bytes() const;
    std::uint64_t discarded_bytes() const;

private:
    void trim_unsynchronized();

    TransportMode mode_;
    std::size_t max_pending_bytes_;
    std::vector<std::uint8_t> pending_;
    bool synchronized_ = false;
    std::uint64_t discarded_bytes_ = 0;
};

}  // namespace asicen
