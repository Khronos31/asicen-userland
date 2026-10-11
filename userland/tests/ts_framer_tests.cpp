#include "asicen/ts_framer.h"

#include <algorithm>
#include <array>
#include <cstdint>
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

bool check(bool value, const char* name)
{
    if (!value) {
        std::cerr << "FAIL: " << name << '\n';
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

bool push(asicen::TsFramer& framer, const std::uint8_t* data, std::size_t size,
          std::vector<std::uint8_t>* output)
{
    output->clear();
    return framer.push({data, size}, collect, output).has_value();
}

std::vector<std::uint8_t> packets(std::size_t count, std::uint8_t sync)
{
    std::vector<std::uint8_t> data(count * asicen::kMpegTsPacketSize, 0xff);
    for (std::size_t i = 0; i < count; ++i) {
        data[i * asicen::kMpegTsPacketSize] = sync;
        data[i * asicen::kMpegTsPacketSize + 1] = 0x1f;
        data[i * asicen::kMpegTsPacketSize + 2] = 0xff;
        data[i * asicen::kMpegTsPacketSize + 3] = static_cast<std::uint8_t>(0x10 | (i & 0x0f));
    }
    return data;
}

bool false_payload_sync_is_rejected(asicen::TransportMode mode, std::size_t false_count,
                                    std::size_t payload_offset, std::size_t chunk_size,
                                    bool reacquire)
{
    constexpr std::size_t packet_size = asicen::kMpegTsPacketSize;
    const std::uint8_t sync =
        mode == asicen::TransportMode::AsicenMode0 || mode == asicen::TransportMode::AsicenMode3
            ? 0xc7U
            : 0x47U;
    auto raw = packets(40U, sync);
    for (std::size_t i = 0U; i < false_count; ++i) {
        // These are payload bytes; every real packet header remains intact.
        auto* false_header = raw.data() + i * packet_size + payload_offset;
        false_header[0] = sync;
        false_header[1] = 0x80U;
        false_header[2] = 0x44U;
        false_header[3] = static_cast<std::uint8_t>(0x10U | i);
    }
    std::vector<std::uint8_t> input(raw.begin() + payload_offset, raw.end());
    std::vector<std::uint8_t> expected(raw.begin() + packet_size, raw.end());
    for (std::size_t i = 0U; i < expected.size(); i += packet_size) {
        expected[i] = 0x47U;
    }
    std::size_t alignment = 0U;
    CHECK(asicen::find_ts_alignment(input.data(), input.size(), mode, &alignment) &&
              alignment == packet_size - payload_offset,
          "four through seven payload syncs do not displace the true packet boundary");

    asicen::TsFramer framer(mode);
    std::vector<std::uint8_t> output;
    std::size_t preceding_bytes = 0U;
    if (reacquire) {
        const auto opening = packets(8U, sync);
        CHECK(framer.push({opening.data(), opening.size()}, collect, &output).has_value() &&
                  output.size() == opening.size() && framer.synchronized(),
              "initial chunk establishes synchronization before the boundary fault");
        preceding_bytes = opening.size();
        output.clear();
        // A missing boundary sync forces a new proof at the next chunk boundary.
        input.insert(input.begin(), 0U);
    }
    for (std::size_t offset = 0U; offset < input.size();) {
        const std::size_t amount = std::min(chunk_size, input.size() - offset);
        CHECK(framer.push({input.data() + offset, amount}, collect, &output).has_value(),
              "false-sync fixture accepts every bounded chunk");
        offset += amount;
    }
    CHECK(output == expected, "partial start and chunk boundaries preserve every true packet byte");
    const auto counters = framer.counters();
    const std::size_t losses = reacquire ? 1U : 0U;
    CHECK(counters.input_bytes_accepted == preceding_bytes + input.size() &&
              counters.emitted_packets == (preceding_bytes + expected.size()) / packet_size &&
              counters.discarded_sync_search_bytes == packet_size - payload_offset + losses &&
              counters.sync_loss_events == losses && counters.buffered_bytes == 0U,
          "false-sync rejection preserves accepted, emitted, discarded, loss and carry accounting");
    CHECK(framer.discarded_bytes() == counters.discarded_sync_search_bytes &&
              framer.sync_loss_events() == counters.sync_loss_events &&
              framer.pending_bytes() == 0U && framer.synchronized(),
          "counter accessors agree after initial acquisition or one genuine sync loss");
    return true;
}

bool false_payload_sync_regressions()
{
    const std::array<asicen::TransportMode, 4> modes{{asicen::TransportMode::Plain,
                                                   asicen::TransportMode::AsicenMode0,
                                                   asicen::TransportMode::AsicenMode3,
                                                   asicen::TransportMode::AsicenMode7}};
    const std::array<std::size_t, 5> payload_offsets{{4U, 17U, 100U, 183U, 184U}};
    const std::array<std::size_t, 10> chunk_sizes{{1U, 187U, 188U, 189U, 751U, 752U, 1317U,
                                                  1504U, 4096U, 8192U}};
    for (const auto mode : modes) {
        for (std::size_t false_count = 4U; false_count <= 7U; ++false_count) {
            for (const auto payload_offset : payload_offsets) {
                for (const auto chunk_size : chunk_sizes) {
                    for (const bool reacquire : {false, true}) {
                        if (!false_payload_sync_is_rejected(mode, false_count, payload_offset,
                                                           chunk_size, reacquire)) {
                            std::cerr << "mode=" << static_cast<unsigned>(mode)
                                      << " false_count=" << false_count
                                      << " payload_offset=" << payload_offset
                                      << " chunk_size=" << chunk_size
                                      << " reacquire=" << reacquire << '\n';
                            return false;
                        }
                    }
                }
            }
        }
    }
    return true;
}
}  // namespace

bool run_tests()
{
    CHECK(asicen::ts_sync_candidate(0x47, asicen::TransportMode::AsicenMode3), "mode3 plain sync");
    CHECK(asicen::ts_sync_candidate(0xc7, asicen::TransportMode::AsicenMode3), "mode3 marked sync");
    CHECK(!asicen::ts_sync_candidate(0xc7, asicen::TransportMode::Plain),
          "plain rejects marked sync");

    auto marked = packets(10, 0xc7);
    std::size_t off = 999;
    CHECK(asicen::find_ts_alignment(marked.data(), marked.size(),
                                    asicen::TransportMode::AsicenMode3, &off) &&
              off == 0,
          "find marked alignment");

    std::vector<std::uint8_t> garbage(13, 0x55);
    garbage.insert(garbage.end(), marked.begin(), marked.end());
    CHECK(asicen::find_ts_alignment(garbage.data(), garbage.size(),
                                    asicen::TransportMode::AsicenMode3, &off) &&
              off == 13,
          "find alignment after garbage");

    asicen::TsFramer framer(asicen::TransportMode::AsicenMode3);
    std::vector<std::uint8_t> first;
    CHECK(push(framer, garbage.data(), 500, &first), "initial fragment is accepted");
    CHECK(first.empty(), "fragment insufficient for sync proof");
    std::vector<std::uint8_t> second;
    CHECK(push(framer, garbage.data() + 500, garbage.size() - 500, &second),
          "remaining fragment is accepted");
    CHECK(second.size() == 10 * asicen::kMpegTsPacketSize, "framer emits packets");
    CHECK(second[0] == 0x47 && second[188] == 0x47, "framer normalizes sync bit");
    CHECK(framer.discarded_bytes() == 13, "framer tracks garbage");

    auto plain = packets(8, 0x47);
    asicen::TsFramer plain_framer(asicen::TransportMode::Plain);
    std::vector<std::uint8_t> p;
    CHECK(push(plain_framer, plain.data(), plain.size(), &p), "plain input is accepted");
    CHECK(p.size() == plain.size() && p[0] == 0x47, "plain framing");

    // Leave room for the next chunk as well as the retained eight-packet probe.
    asicen::TsFramer bounded(asicen::TransportMode::Plain, 188 * 16);
    std::vector<std::uint8_t> junk(188 * 20, 0);
    std::vector<std::uint8_t> no_output;
    CHECK(bounded.push({junk.data(), junk.size()}, collect, &no_output).error() ==
              Error::SLOW_CONSUMER,
          "chunk beyond bounded carry is rejected before copying");
    for (std::size_t offset = 0; offset < junk.size(); offset += 188U) {
        CHECK(push(bounded, junk.data() + offset, 188U, &no_output) && no_output.empty(),
              "bounded junk no output");
    }
    CHECK(bounded.pending_bytes() < 188 * 8, "bounded pending retains less than one sync proof");
    CHECK(bounded.discarded_bytes() > 0, "bounded discard count");

    auto eight = packets(8U, 0x47U);
    asicen::TsFramer eight_probe(asicen::TransportMode::Plain);
    std::vector<std::uint8_t> output;
    CHECK(push(eight_probe, eight.data(), 7U * 188U, &output) && output.empty() &&
              !eight_probe.synchronized(),
          "seven complete packets are insufficient for synchronization");
    CHECK(push(eight_probe, eight.data() + 7U * 188U, 188U, &output) && output == eight,
          "the eighth complete packet establishes synchronization");
    std::vector<std::uint8_t> incomplete(187U, 0U);
    incomplete.insert(incomplete.end(), eight.begin(), eight.begin() + 7U * 188U + 1U);
    asicen::TsFramer complete_probe(asicen::TransportMode::Plain);
    CHECK(push(complete_probe, incomplete.data(), incomplete.size(), &output) && output.empty() &&
              !complete_probe.synchronized(),
          "prefixed partial eighth packet cannot establish synchronization");
    CHECK(push(complete_probe, eight.data() + 7U * 188U + 1U, 187U, &output) && output == eight,
          "eighth packet tail completes synchronization after a prefix");
    struct SinkState {
        bool accept = false;
        std::vector<std::uint8_t> output;
    } sink_state;
    const auto sink = [](void* context, ByteView packet) noexcept {
        const auto* bytes = packet.data;
        const auto size = packet.size;
        auto& state = *static_cast<SinkState*>(context);
        if (!state.accept) {
            return Result<void>::failure(Error::BUSY);
        }
        state.output.insert(state.output.end(), bytes, bytes + size);
        return Result<void>::success();
    };
    asicen::TsFramer retry(asicen::TransportMode::Plain, eight.size());
    CHECK(retry.push({eight.data(), eight.size()}, sink, &sink_state).error() == Error::BUSY &&
              retry.pending_bytes() == eight.size(),
          "failed sink preserves the exact packet and all following input");
    CHECK(retry.push({eight.data(), 1U}, sink, &sink_state).error() == Error::SLOW_CONSUMER &&
              retry.pending_bytes() == eight.size(),
          "backlogged input is rejected before copying beyond capacity");
    CHECK(retry.counters().input_bytes_accepted == eight.size() &&
              retry.counters().emitted_packets == 0U,
          "failed and rejected sink calls do not count unaccepted packets or input");
    sink_state.accept = true;
    CHECK(retry.push({}, sink, &sink_state).has_value() && retry.pending_bytes() == 0U &&
              sink_state.output == eight,
          "empty push retries every unconsumed packet exactly once");
    CHECK(retry.push({eight.data(), 1048577U}, sink, &sink_state).error() == Error::INVALID_ARGUMENT,
          "oversized push is rejected before reading input");
    CHECK(retry.push({nullptr, 1U}, sink, &sink_state).error() == Error::INVALID_ARGUMENT,
          "nonempty null input is rejected");
    std::vector<std::uint8_t> relock{0U};
    relock.insert(relock.end(), eight.begin(), eight.end());
    CHECK(push(eight_probe, relock.data(), relock.size(), &output) && output == eight &&
              eight_probe.sync_loss_events() == 1U && eight_probe.discarded_bytes() == 1U,
          "post-lock garbage reports one loss and reacquires eight-packet alignment");
    CHECK(retry.counters().emitted_packets == 8U && retry.counters().buffered_bytes == 0U,
          "retry counts each accepted packet exactly once");
    eight_probe.reset();
    CHECK(eight_probe.sync_loss_events() == 0U && eight_probe.pending_bytes() == 0U,
          "reset clears carry and synchronization counters");

    return false_payload_sync_regressions();
}

int main()
{
    return run_tests() ? 0 : 1;
}
