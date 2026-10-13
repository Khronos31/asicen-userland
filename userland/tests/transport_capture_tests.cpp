// SPDX-License-Identifier: GPL-2.0-only
#include "asicen/transport_capture.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <type_traits>
#include <utility>
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
          "startup bytes before four-packet alignment are accounted");

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

std::vector<std::uint8_t> packets(std::size_t count)
{
    std::vector<std::uint8_t> raw(count * asicen::kMpegTsPacketSize);
    for (std::size_t packet = 0; packet < count; ++packet) {
        auto* bytes = raw.data() + packet * asicen::kMpegTsPacketSize;
        std::fill_n(bytes, asicen::kMpegTsPacketSize, static_cast<std::uint8_t>(0x80U + packet));
        bytes[0] = 0x47;
        bytes[1] = 0x01;
        bytes[2] = 0x31;
        bytes[3] = static_cast<std::uint8_t>(0x10U | (packet & 0x0fU));
    }
    return raw;
}

bool move_and_clear_lifecycle()
{
    using Decoder = asicen::TransportCaptureDecoderV7;
    static_assert(!std::is_copy_constructible<Decoder>::value, "decoder owns secret state");
    static_assert(std::is_nothrow_move_constructible<Decoder>::value, "decoder move is noexcept");
    static_assert(std::is_nothrow_move_assignable<Decoder>::value, "decoder move is noexcept");
    std::array<std::uint8_t, 16> seed{};
    const auto raw = packets(10);
    Decoder baseline(seed.data(), seed.size());
    std::vector<std::uint8_t> expected;
    CHECK(baseline.push({raw.data(), raw.size()}, collect, &expected).has_value(),
          "baseline packet sequence decodes");

    Decoder source(seed.data(), seed.size());
    std::vector<std::uint8_t> output;
    constexpr std::size_t split = 3 * asicen::kMpegTsPacketSize + 17;
    CHECK(source.push({raw.data(), split}, collect, &output).has_value() && output.empty(),
          "partial synchronization probe is retained before move");
    Decoder moved(std::move(source));
    CHECK(!source.valid() && source.push({}, collect, &output).error() == Error::NOT_READY,
          "move construction invalidates source without emitting");
    CHECK(moved.valid() && moved.pending_bytes() == split &&
              moved.counters().input_bytes_accepted == split,
          "move construction transfers pending bytes and counters");

    seed.fill(0xa5);
    Decoder destination(seed.data(), seed.size());
    CHECK(destination.push({raw.data(), 19}, collect, &output).has_value(),
          "replacement target initially has another seed and partial input");
    destination = std::move(moved);
    CHECK(!moved.valid() && moved.push({}, collect, &output).error() == Error::NOT_READY,
          "move assignment invalidates source");
    CHECK(destination.pending_bytes() == split &&
              destination.counters().input_bytes_accepted == split,
          "move assignment replaces old framing state");
    auto* same = &destination;
    destination = std::move(*same);
    CHECK(destination.valid(), "self move leaves decoder usable");
    CHECK(destination.push({raw.data() + split, raw.size() - split}, collect, &output).has_value() &&
              output == expected,
          "moved prepared schedules and carry bytes produce identical ordered output");
    const auto before_clear = destination.counters();
    destination.clear();
    destination.clear();
    CHECK(!destination.valid() &&
              destination.push({raw.data(), raw.size()}, collect, &output).error() == Error::NOT_READY &&
              output == expected,
          "repeated clear prevents subsequent output");
    CHECK(destination.counters().input_bytes_accepted == before_clear.input_bytes_accepted &&
              destination.counters().emitted_packets == before_clear.emitted_packets,
          "clear preserves final counters");

    Decoder fresh(seed.data(), seed.size());
    CHECK(fresh.valid() && fresh.counters().input_bytes_accepted == 0 && fresh.pending_bytes() == 0,
          "fresh decoder has independent schedules and reset framing state");
    return true;
}

struct RetrySink {
    bool reject = true;
    std::vector<std::uint8_t> output;
};

Result<void> reject_once(void* opaque, ByteView packet) noexcept
{
    auto& state = *static_cast<RetrySink*>(opaque);
    if (state.reject) {
        state.reject = false;
        return Result<void>::failure(Error::SLOW_CONSUMER);
    }
    return collect(&state.output, packet);
}

bool retry_errors_and_sync_reacquisition()
{
    std::array<std::uint8_t, 16> seed{};
    auto raw = packets(5);
    const auto tail = packets(5);
    raw.insert(raw.end(), 11, 0x55);
    raw.insert(raw.end(), tail.begin(), tail.end());
    asicen::TransportCaptureDecoderV7 decoder(seed.data(), seed.size());
    RetrySink retry;
    CHECK(decoder.push({raw.data(), raw.size()}, reject_once, &retry).error() == Error::SLOW_CONSUMER,
          "sink error propagates unchanged");
    CHECK(retry.output.empty() && decoder.counters().input_bytes_accepted == raw.size() &&
              decoder.counters().emitted_packets == 0 && decoder.pending_bytes() == raw.size(),
          "rejected packet and later bytes remain buffered without emission");
    CHECK(decoder.push({}, reject_once, &retry).has_value(), "empty push retries the rejected packet");
    CHECK(retry.output.size() == 10 * asicen::kMpegTsPacketSize &&
              decoder.counters().emitted_packets == 10 && decoder.pending_bytes() == 0 &&
              decoder.discarded_bytes() == 11 && decoder.sync_loss_events() == 1,
          "retry preserves counters and reacquires sync after the inserted gap");
    asicen::TransportCaptureDecoderV7 direct(seed.data(), seed.size());
    std::vector<std::uint8_t> expected;
    CHECK(direct.push({raw.data(), raw.size()}, collect, &expected).has_value() &&
              expected == retry.output,
          "sink retry preserves decoded bytes and order across reacquisition");
    const auto accepted = decoder.counters().input_bytes_accepted;
    CHECK(decoder.push({nullptr, 1}, collect, &expected).error() == Error::INVALID_ARGUMENT &&
              decoder.push({}, nullptr, &expected).error() == Error::INVALID_ARGUMENT &&
              decoder.counters().input_bytes_accepted == accepted && expected == retry.output,
          "invalid input and sink produce no state or output change");
    asicen::TransportCaptureDecoderV7 invalid(nullptr, 16);
    CHECK(!invalid.valid(), "null seed cannot prepare a decoder");
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
    if (!move_and_clear_lifecycle() || !retry_errors_and_sync_reacquisition()) {
        return 1;
    }
    std::cout << "transport capture lifecycle/retry tests passed\n";
}
