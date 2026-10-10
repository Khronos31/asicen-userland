#include "asicen/ts_framer.h"

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

    asicen::TsFramer bounded(asicen::TransportMode::Plain, 188 * 8);
    std::vector<std::uint8_t> junk(188 * 20, 0);
    std::vector<std::uint8_t> no_output;
    CHECK(bounded.push({junk.data(), junk.size()}, collect, &no_output).error() ==
              Error::SLOW_CONSUMER,
          "chunk beyond bounded carry is rejected before copying");
    for (std::size_t offset = 0; offset < junk.size(); offset += 188U) {
        CHECK(push(bounded, junk.data() + offset, 188U, &no_output) && no_output.empty(),
              "bounded junk no output");
    }
    CHECK(bounded.pending_bytes() <= 188 * 8, "bounded pending");
    CHECK(bounded.discarded_bytes() > 0, "bounded discard count");

    auto four = packets(4U, 0x47U);
    asicen::TsFramer four_probe(asicen::TransportMode::Plain);
    std::vector<std::uint8_t> output;
    CHECK(push(four_probe, four.data(), four.size(), &output) && output.size() == four.size(),
          "four complete packets establish the common sync boundary");
    std::vector<std::uint8_t> incomplete(187U, 0U);
    incomplete.insert(incomplete.end(), four.begin(), four.begin() + 3U * 188U + 1U);
    asicen::TsFramer complete_probe(asicen::TransportMode::Plain);
    CHECK(push(complete_probe, incomplete.data(), incomplete.size(), &output) && output.empty() &&
              !complete_probe.synchronized(),
          "prefixed partial fourth packet cannot establish synchronization");
    CHECK(push(complete_probe, four.data() + 3U * 188U + 1U, 187U, &output) &&
              output.size() == four.size(),
          "fourth packet tail completes synchronization after a prefix");
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
    asicen::TsFramer retry(asicen::TransportMode::Plain, four.size());
    CHECK(retry.push({four.data(), four.size()}, sink, &sink_state).error() == Error::BUSY &&
              retry.pending_bytes() == four.size(),
          "failed sink preserves the exact packet and all following input");
    CHECK(retry.push({four.data(), 1U}, sink, &sink_state).error() == Error::SLOW_CONSUMER &&
              retry.pending_bytes() == four.size(),
          "backlogged input is rejected before copying beyond capacity");
    CHECK(retry.counters().input_bytes_accepted == four.size() &&
              retry.counters().emitted_packets == 0U,
          "failed and rejected sink calls do not count unaccepted packets or input");
    sink_state.accept = true;
    CHECK(retry.push({}, sink, &sink_state).has_value() && retry.pending_bytes() == 0U &&
              sink_state.output == four,
          "empty push retries every unconsumed packet exactly once");
    CHECK(retry.push({four.data(), 1048577U}, sink, &sink_state).error() == Error::INVALID_ARGUMENT,
          "oversized push is rejected before reading input");
    CHECK(retry.push({nullptr, 1U}, sink, &sink_state).error() == Error::INVALID_ARGUMENT,
          "nonempty null input is rejected");
    std::vector<std::uint8_t> relock{0U};
    relock.insert(relock.end(), four.begin(), four.end());
    CHECK(push(four_probe, relock.data(), relock.size(), &output) && output == four &&
              four_probe.sync_loss_events() == 1U && four_probe.discarded_bytes() == 1U,
          "post-lock garbage reports one loss and reacquires four-packet alignment");
    CHECK(retry.counters().emitted_packets == 4U && retry.counters().buffered_bytes == 0U,
          "retry counts each accepted packet exactly once");
    four_probe.reset();
    CHECK(four_probe.sync_loss_events() == 0U && four_probe.pending_bytes() == 0U,
          "reset clears carry and synchronization counters");

    return true;
}

int main()
{
    return run_tests() ? 0 : 1;
}
