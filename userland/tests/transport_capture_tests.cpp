// SPDX-License-Identifier: GPL-2.0-only
#include "asicen/transport_capture.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {
using px4::userland::ByteView;
using px4::userland::Error;
using px4::userland::Result;
Result<void> collect(void* opaque, ByteView packet) noexcept
{
    auto& bytes = *static_cast<std::vector<std::uint8_t>*>(opaque);
    bytes.insert(bytes.end(), packet.data, packet.data + packet.size);
    return Result<void>::success();
}

bool check(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

#define CHECK(...)                                                                                 \
    do {                                                                                           \
        if (!check(__VA_ARGS__)) {                                                                 \
            return false;                                                                          \
        }                                                                                          \
    } while (false)

bool fragmented_framing_and_transform()
{
    std::array<std::uint8_t, 16> seed{};
    for (std::size_t i = 0; i < seed.size(); ++i) {
        seed[i] = static_cast<std::uint8_t>(i * 7U + 3U);
    }
    asicen::TransportCaptureDecoderV7 decoder(seed.data(), seed.size());
    CHECK(decoder.valid(), "v7 decoder accepts exact seed");

    std::vector<std::uint8_t> raw(7, 0x55);
    for (std::size_t packet = 0; packet < 12; ++packet) {
        const std::size_t start = raw.size();
        raw.resize(start + asicen::kMpegTsPacketSize);
        raw[start] = 0x47;
        raw[start + 1] = static_cast<std::uint8_t>(packet);
        raw[start + 2] = 0x31;
        raw[start + 3] = 0x10;
        for (std::size_t i = 4; i < asicen::kMpegTsPacketSize; ++i) {
            raw[start + i] = static_cast<std::uint8_t>(i + packet * 13U);
        }
    }

    std::vector<std::uint8_t> decoded;
    const std::array<std::size_t, 5> chunks{{13, 377, 1, 819, 1021}};
    std::size_t offset = 0;
    std::size_t chunk_index = 0;
    while (offset < raw.size()) {
        const std::size_t amount =
            std::min(chunks[chunk_index++ % chunks.size()], raw.size() - offset);
        CHECK(decoder.push({raw.data() + offset, amount}, collect, &decoded).has_value(),
              "decoder accepts each bounded fragment");
        offset += amount;
    }
    CHECK(decoded.size() == 12 * asicen::kMpegTsPacketSize,
          "fragmented input emits all complete aligned packets");
    CHECK(decoder.discarded_bytes() == 7,
          "startup bytes before eight-packet alignment are accounted");

    asicen::TransportMaterialV7 material{};
    CHECK(asicen::derive_transport_material_v7(seed.data(), seed.size(), &material),
          "synthetic material derives");
    for (std::size_t packet = 0; packet < 12; ++packet) {
        std::array<std::uint8_t, asicen::kMpegTsPacketSize> expected{};
        const auto* source = raw.data() + 7 + packet * asicen::kMpegTsPacketSize;
        std::copy_n(source, expected.size(), expected.data());
        CHECK(asicen::transform_transport_packet_v7(
                  expected.data(), expected.size(), material.first_des_key.data(),
                  material.second_des_key.data(), material.xor_state.data(), expected.data()),
              "single packet transform succeeds");
        CHECK(std::equal(expected.begin(), expected.end(),
                         decoded.begin() + packet * expected.size()),
              "stream decoder matches direct packet transform");
        CHECK(std::equal(source, source + 4, decoded.begin() + packet * expected.size()),
              "transform preserves the four-byte TS header");
    }
    CHECK(decoder.pending_bytes() == 0, "all synthetic packets are complete");
    return true;
}

bool false_payload_sync_does_not_reach_transform()
{
    constexpr std::size_t packet_size = asicen::kMpegTsPacketSize;
    constexpr std::size_t payload_offset = 100U;
    const std::array<std::uint8_t, 16> seed{{3U, 10U, 17U, 24U, 31U, 38U, 45U, 52U,
                                           59U, 66U, 73U, 80U, 87U, 94U, 101U, 108U}};
    asicen::TransportMaterialV7 material{};
    CHECK(asicen::derive_transport_material_v7(seed.data(), seed.size(), &material),
          "false-sync fixture derives synthetic material");
    const std::array<std::size_t, 10> chunks{{1U, 187U, 188U, 189U, 751U, 752U, 1317U,
                                             1504U, 4096U, 8192U}};
    for (std::size_t false_count = 4U; false_count <= 7U; ++false_count) {
        std::vector<std::uint8_t> raw(40U * packet_size, 0xffU);
        for (std::size_t i = 0U; i < 40U; ++i) {
            auto* packet = raw.data() + i * packet_size;
            packet[0] = 0x47U;
            packet[1] = 0x01U;
            packet[2] = 0x00U;
            packet[3] = static_cast<std::uint8_t>(0x10U | (i & 0x0fU));
            if (i < false_count) {
                packet[payload_offset] = 0x47U;
                packet[payload_offset + 1U] = 0x80U;
                packet[payload_offset + 2U] = 0x44U;
                packet[payload_offset + 3U] = static_cast<std::uint8_t>(0x10U | i);
            }
        }
        // Build the oracle from the real complete packets, without a framer.
        std::vector<std::uint8_t> expected(39U * packet_size);
        for (std::size_t i = 1U; i < 40U; ++i) {
            CHECK(asicen::transform_transport_packet_v7(
                      raw.data() + i * packet_size, packet_size, material.first_des_key.data(),
                      material.second_des_key.data(), material.xor_state.data(),
                      expected.data() + (i - 1U) * packet_size),
                  "real packet transform builds the byte-exact expected output");
        }
        for (const auto chunk_size : chunks) {
            asicen::TransportCaptureDecoderV7 decoder(seed.data(), seed.size());
            std::vector<std::uint8_t> decoded;
            for (std::size_t offset = payload_offset; offset < raw.size();) {
                const auto amount = std::min(chunk_size, raw.size() - offset);
                CHECK(decoder.push({raw.data() + offset, amount}, collect, &decoded).has_value(),
                      "decoder accepts the partial-packet false-sync fixture");
                offset += amount;
            }
            if (!check(decoded == expected,
                       "only the true packet alignment reaches the transport transform")) {
                std::cerr << "false_count=" << false_count << " chunk_size=" << chunk_size
                          << '\n';
                return false;
            }
            const auto counters = decoder.counters();
            CHECK(counters.input_bytes_accepted == raw.size() - payload_offset &&
                      counters.emitted_packets == 39U &&
                      counters.discarded_sync_search_bytes == packet_size - payload_offset &&
                      counters.sync_loss_events == 0U && counters.buffered_bytes == 0U,
                  "decoder retains initial-search and emission accounting without false losses");
        }
    }
    return true;
}

bool invalid_seed_is_rejected()
{
    std::array<std::uint8_t, 15> seed{};
    asicen::TransportCaptureDecoderV7 decoder(seed.data(), seed.size());
    std::vector<std::uint8_t> output;
    CHECK(!decoder.valid() &&
              decoder.push({seed.data(), seed.size()}, collect, &output).error() ==
                  Error::NOT_READY &&
              output.empty(),
          "invalid seed length is rejected without output");
    return true;
}
}  // namespace

int main()
{
    if (!fragmented_framing_and_transform()) {
        return 1;
    }
    if (!invalid_seed_is_rejected()) {
        return 1;
    }
    if (!false_payload_sync_does_not_reach_transform()) {
        return 1;
    }
}
