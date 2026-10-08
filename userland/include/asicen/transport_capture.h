// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "asicen/transport_transform.h"
#include "asicen/ts_framer.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace asicen {

class TransportCaptureDecoderV7 final {
public:
    explicit TransportCaptureDecoderV7(const std::uint8_t* seed,
                                       std::size_t seed_size);
    ~TransportCaptureDecoderV7();
    void clear() noexcept;
    bool valid() const { return valid_; }
    std::vector<std::uint8_t> push(const std::uint8_t* bytes, std::size_t size);
    std::uint64_t discarded_bytes() const { return framer_.discarded_bytes(); }
    std::size_t pending_bytes() const { return framer_.pending_bytes(); }

private:
    TsFramer framer_{TransportMode::AsicenMode7};
    TransportMaterialV7 material_{};
    bool valid_ = false;
};

}  // namespace asicen
