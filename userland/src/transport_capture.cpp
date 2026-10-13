// SPDX-License-Identifier: GPL-2.0-only
#include "asicen/transport_capture.h"

#include <utility>

namespace asicen {

TransportCaptureDecoderV7::TransportCaptureDecoderV7(const std::uint8_t* seed,
                                                     std::size_t seed_size)
{
    valid_ = framer_.valid() && derive_transport_material_v7(seed, seed_size, &material_) &&
             transform_.prepare(material_.first_des_key.data(), material_.second_des_key.data(),
                                material_.xor_state.data());
}

TransportCaptureDecoderV7::~TransportCaptureDecoderV7()
{
    clear();
}

TransportCaptureDecoderV7::TransportCaptureDecoderV7(TransportCaptureDecoderV7&& other) noexcept
    : framer_(std::move(other.framer_)), material_(other.material_),
      transform_(std::move(other.transform_)), valid_(other.valid_)
{
    other.clear();
}

TransportCaptureDecoderV7& TransportCaptureDecoderV7::operator=(
    TransportCaptureDecoderV7&& other) noexcept
{
    if (this != &other) {
        clear();
        framer_ = std::move(other.framer_);
        material_ = other.material_;
        transform_ = std::move(other.transform_);
        valid_ = other.valid_;
        other.clear();
    }
    return *this;
}

void TransportCaptureDecoderV7::clear() noexcept
{
    transform_.clear();
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
        const TransportTransformV7* transform;
        TsFramer::PacketSink sink;
        void* context;
    } adapter{&transform_, sink, context};
    return framer_.push(
        input,
        [](void* opaque, px4::userland::ByteView packet) noexcept {
            auto& target = *static_cast<Adapter*>(opaque);
            std::array<std::uint8_t, kMpegTsPacketSize> decoded{};
            if (!target.transform->transform_packet(packet.data, packet.size, decoded.data())) {
                return Result<void>::failure(Error::PROTOCOL_ERROR);
            }
            return target.sink(target.context, {decoded.data(), decoded.size()});
        },
        &adapter);
}

}  // namespace asicen
