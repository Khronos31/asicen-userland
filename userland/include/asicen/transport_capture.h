// SPDX-License-Identifier: GPL-2.0-only
#ifndef ASICEN_USERLAND_TRANSPORT_CAPTURE_H
#define ASICEN_USERLAND_TRANSPORT_CAPTURE_H

#include "asicen/transport_transform.h"
#include "asicen/ts_framer.h"

#include <cstddef>
#include <cstdint>

namespace asicen {

class TransportCaptureDecoderV7 final {
public:
    explicit TransportCaptureDecoderV7(const std::uint8_t* seed, std::size_t seed_size);
    ~TransportCaptureDecoderV7();
    TransportCaptureDecoderV7(TransportCaptureDecoderV7&&) noexcept = default;
    TransportCaptureDecoderV7& operator=(TransportCaptureDecoderV7&&) noexcept = default;
    void clear() noexcept;
    bool valid() const { return valid_; }
    px4::userland::Result<void> push(px4::userland::ByteView input, TsFramer::PacketSink sink,
                                     void* context) noexcept;
    TsFramer::Counters counters() const noexcept { return framer_.counters(); }
    std::uint64_t discarded_bytes() const { return framer_.discarded_bytes(); }
    std::uint64_t sync_loss_events() const { return framer_.sync_loss_events(); }
    std::size_t pending_bytes() const { return framer_.pending_bytes(); }

private:
    TsFramer framer_{TransportMode::AsicenMode7};
    TransportMaterialV7 material_{};
    bool valid_ = false;
};

}  // namespace asicen

#endif  // ASICEN_USERLAND_TRANSPORT_CAPTURE_H
