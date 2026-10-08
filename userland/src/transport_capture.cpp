// SPDX-License-Identifier: GPL-2.0-only
#include "asicen/transport_capture.h"

namespace asicen {

TransportCaptureDecoderV7::TransportCaptureDecoderV7(const std::uint8_t* seed,
                                                     std::size_t seed_size) {
    valid_ = derive_transport_material_v7(seed, seed_size, &material_);
}

std::vector<std::uint8_t> TransportCaptureDecoderV7::push(
    const std::uint8_t* bytes, std::size_t size) {
    if (!valid_) return {};
    auto packets = framer_.push(bytes, size);
    for (std::size_t offset = 0; offset + kMpegTsPacketSize <= packets.size();
         offset += kMpegTsPacketSize) {
        if (!transform_transport_packet_v7(
                packets.data() + offset, kMpegTsPacketSize,
                material_.first_des_key.data(), material_.second_des_key.data(),
                material_.xor_state.data(), packets.data() + offset))
            return {};
    }
    return packets;
}

}  // namespace asicen
