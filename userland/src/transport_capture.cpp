// SPDX-License-Identifier: GPL-2.0-only
#include "asicen/transport_capture.h"

namespace asicen {

TransportCaptureDecoderV7::TransportCaptureDecoderV7(const std::uint8_t* seed,
                                                     std::size_t seed_size)
{
    valid_ = framer_.valid() && derive_transport_material_v7(seed, seed_size, &material_);
}

TransportCaptureDecoderV7::~TransportCaptureDecoderV7()
{
    clear();
}

void TransportCaptureDecoderV7::clear() noexcept
{
    volatile std::uint8_t* bytes = reinterpret_cast<volatile std::uint8_t*>(&material_);
    for (std::size_t i = 0; i < sizeof(material_); ++i) {
        bytes[i] = 0U;
    }
    valid_ = false;
}

px4::userland::Result<void> TransportCaptureDecoderV7::push(px4::userland::ByteView input,
                                                            TsFramer::PacketSink sink,
                                                            void* context) noexcept
{
    using px4::userland::Error;
    using px4::userland::Result;
    if (!valid_) {
        return Result<void>::failure(Error::NOT_READY);
    }
    if (sink == nullptr) {
        return Result<void>::failure(Error::INVALID_ARGUMENT);
    }
    struct Adapter {
        const TransportMaterialV7* material;
        TsFramer::PacketSink sink;
        void* context;
    } adapter{&material_, sink, context};
    return framer_.push(
        input,
        [](void* opaque, px4::userland::ByteView packet) noexcept {
            auto& target = *static_cast<Adapter*>(opaque);
            std::array<std::uint8_t, kMpegTsPacketSize> decoded{};
            if (!transform_transport_packet_v7(packet.data, packet.size,
                                               target.material->first_des_key.data(),
                                               target.material->second_des_key.data(),
                                               target.material->xor_state.data(), decoded.data())) {
                return Result<void>::failure(Error::PROTOCOL_ERROR);
            }
            return target.sink(target.context, {decoded.data(), decoded.size()});
        },
        &adapter);
}

}  // namespace asicen
