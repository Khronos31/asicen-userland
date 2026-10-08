// SPDX-License-Identifier: GPL-2.0-only
#include "asicen/transport_capture.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {
void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void fragmented_framing_and_transform() {
    std::array<std::uint8_t, 16> seed{};
    for (std::size_t i = 0; i < seed.size(); ++i)
        seed[i] = static_cast<std::uint8_t>(i * 7U + 3U);
    asicen::TransportCaptureDecoderV7 decoder(seed.data(), seed.size());
    check(decoder.valid(), "v7 decoder accepts exact seed");

    std::vector<std::uint8_t> raw(7, 0x55);
    for (std::size_t packet = 0; packet < 12; ++packet) {
        const std::size_t start = raw.size();
        raw.resize(start + asicen::kMpegTsPacketSize);
        raw[start] = 0x47;
        raw[start + 1] = static_cast<std::uint8_t>(packet);
        raw[start + 2] = 0x31;
        raw[start + 3] = 0x10;
        for (std::size_t i = 4; i < asicen::kMpegTsPacketSize; ++i)
            raw[start + i] = static_cast<std::uint8_t>(i + packet * 13U);
    }

    std::vector<std::uint8_t> decoded;
    const std::array<std::size_t, 5> chunks{{13, 377, 1, 819, 1021}};
    std::size_t offset = 0;
    std::size_t chunk_index = 0;
    while (offset < raw.size()) {
        const std::size_t amount = std::min(chunks[chunk_index++ % chunks.size()],
                                            raw.size() - offset);
        auto output = decoder.push(raw.data() + offset, amount);
        decoded.insert(decoded.end(), output.begin(), output.end());
        offset += amount;
    }
    check(decoded.size() == 12 * asicen::kMpegTsPacketSize,
          "fragmented input emits all complete aligned packets");
    check(decoder.discarded_bytes() == 7,
          "startup bytes before 8-sync alignment are accounted");

    asicen::TransportMaterialV7 material{};
    check(asicen::derive_transport_material_v7(seed.data(), seed.size(), &material),
          "synthetic material derives");
    for (std::size_t packet = 0; packet < 12; ++packet) {
        std::array<std::uint8_t, asicen::kMpegTsPacketSize> expected{};
        const auto* source = raw.data() + 7 + packet * asicen::kMpegTsPacketSize;
        std::copy_n(source, expected.size(), expected.data());
        check(asicen::transform_transport_packet_v7(
                  expected.data(), expected.size(), material.first_des_key.data(),
                  material.second_des_key.data(), material.xor_state.data(),
                  expected.data()), "single packet transform succeeds");
        check(std::equal(expected.begin(), expected.end(),
                         decoded.begin() + packet * expected.size()),
              "stream decoder matches direct packet transform");
        check(std::equal(source, source + 4,
                         decoded.begin() + packet * expected.size()),
              "transform preserves the four-byte TS header");
    }
    check(decoder.pending_bytes() == 0, "all synthetic packets are complete");
}

void invalid_seed_is_rejected() {
    std::array<std::uint8_t, 15> seed{};
    asicen::TransportCaptureDecoderV7 decoder(seed.data(), seed.size());
    check(!decoder.valid() && decoder.push(seed.data(), seed.size()).empty(),
          "invalid seed length is rejected without output");
}
}  // namespace

int main() {
    fragmented_framing_and_transform();
    invalid_seed_is_rejected();
}
