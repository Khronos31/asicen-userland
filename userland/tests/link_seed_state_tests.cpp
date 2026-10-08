// SPDX-License-Identifier: GPL-2.0-only
#include "asicen/link_seed_state.h"
#include "asicen/protocol.h"
#include "asicen/write_protocol.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {
void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

class FakeIo final : public asicen::LinkSeedStateIo {
public:
    std::array<std::uint8_t, 16> seed{};
    std::uint8_t controller = 0x25;
    int fail_write_at = -1;
    int writes = 0;
    bool fail_readback = false;
    std::vector<std::string> trace;

    bool read_controller05(std::uint8_t* out) override {
        trace.emplace_back("read05");
        if (out == nullptr) return false;
        *out = controller;
        return true;
    }
    bool read_link_seed(std::uint8_t* out, std::size_t size) override {
        trace.emplace_back("read-seed");
        if (fail_readback || out == nullptr || size != seed.size()) return false;
        std::copy(seed.begin(), seed.end(), out);
        return true;
    }
    bool write_controller05(std::uint8_t value) override {
        trace.emplace_back("write05:" + std::to_string(value));
        ++writes;
        if (writes == fail_write_at) return false;
        controller = value;
        return true;
    }
    bool write_link_seed_byte(std::uint8_t reg, std::uint8_t value) override {
        trace.emplace_back("write:" + std::to_string(reg) + ":" +
                           std::to_string(value));
        ++writes;
        if (writes == fail_write_at) return false;
        if (reg < 0x10 || reg > 0x1f) return false;
        seed[reg - 0x10] = value;
        return true;
    }
};

void verify_wire_order_and_restore() {
    const auto seed_read = asicen::make_i2c_read(0x4a, 0x10, 16, 0, 1000);
    check(static_cast<std::uint8_t>(seed_read.request) == 0x02 &&
              seed_read.value == 0x104a && seed_read.index == 0 &&
              seed_read.length == 17 && seed_read.direction == asicen::Direction::In,
          "seed snapshot uses request02 slave4a register10 mode0 and 17-byte response");
    const std::uint8_t seed_byte = 0x5a;
    asicen::ControlTransfer seed_write{};
    check(asicen::make_i2c_write_chunk(0x4a, 0x10, &seed_byte, 1, false,
                                       &seed_write, 1000),
          "single link seed byte wire packet builds");
    check(static_cast<std::uint8_t>(seed_write.request) == 0x03 &&
              seed_write.value == 0x104a && seed_write.index == 0x005a &&
              seed_write.length == 2 && seed_write.direction == asicen::Direction::In,
          "seed write uses request03 with byte in wIndex and status response");
    const std::uint8_t output_value = 0xa0;
    asicen::ControlTransfer output_write{};
    check(asicen::make_i2c_write_chunk(0x4a, 0x05, &output_value, 1, false,
                                       &output_write, 1000) &&
              output_write.value == 0x054a && output_write.index == 0x00a0,
          "controller05 output write encodes observed register/value");

    FakeIo io;
    for (std::size_t i = 0; i < io.seed.size(); ++i)
        io.seed[i] = static_cast<std::uint8_t>(0x80U + i);
    const auto original_seed = io.seed;
    const auto original_controller = io.controller;
    std::array<std::uint8_t, 16> supplied{};
    for (std::size_t i = 0; i < supplied.size(); ++i)
        supplied[i] = static_cast<std::uint8_t>(i + 1U);

    asicen::LinkSeedState state;
    check(state.snapshot(&io), "snapshot succeeds");
    check(io.trace == std::vector<std::string>({"read05", "read-seed"}),
          "snapshot reads controller05 and contiguous seed state");
    io.trace.clear();
    check(state.apply(&io, supplied.data(), supplied.size()), "seed apply succeeds");
    check(io.trace.size() == 19 && io.trace.front() == "write:16:1" &&
              io.trace[15] == "write:31:16" && io.trace[16] == "write05:160" &&
              io.trace[17] == "read-seed" && io.trace[18] == "read05",
          "wire writes are registers 10..1f then controller05 a0 and readback");
    check(io.seed == supplied && io.controller == 0xa0,
          "apply writes supplied seed and ready state");
    io.trace.clear();
    check(state.restore_and_verify(&io), "restore readback succeeds");
    check(io.seed == original_seed && io.controller == original_controller,
          "restore recovers all original state");
    check(io.trace.front() == "write:16:128" && io.trace[15] == "write:31:143" &&
              io.trace[16] == "write05:37" && io.trace[17] == "read-seed" &&
              io.trace[18] == "read05",
          "restore writes seed, restores05 last, then verifies both snapshots");
}

void partial_write_unwinds() {
    FakeIo io;
    io.seed.fill(0x39);
    const auto original = io.seed;
    asicen::LinkSeedState state;
    std::array<std::uint8_t, 16> supplied{};
    check(state.snapshot(&io), "partial case snapshots");
    io.fail_write_at = 4;
    check(!state.apply(&io, supplied.data(), supplied.size()),
          "partial seed write is reported");
    io.fail_write_at = -1;
    check(state.restore_and_verify(&io), "partial application still restores");
    check(io.seed == original && io.controller == 0x25,
          "partial application cleanup restores both regions");
}

void readback_failure_is_not_success() {
    FakeIo io;
    asicen::LinkSeedState state;
    std::array<std::uint8_t, 16> supplied{};
    check(state.snapshot(&io) && state.apply(&io, supplied.data(), supplied.size()),
          "readback case setup succeeds");
    io.fail_readback = true;
    check(!state.restore_and_verify(&io), "restore readback failure is explicit");
    check(state.snapshotted(), "failed restore remains retryable");
    io.fail_readback = false;
    check(state.restore_and_verify(&io), "restore retry can verify snapshot");

    FakeIo apply_io;
    asicen::LinkSeedState apply_state;
    check(apply_state.snapshot(&apply_io), "apply readback case snapshots");
    apply_io.fail_readback = true;
    check(!apply_state.apply(&apply_io, supplied.data(), supplied.size()),
          "new seed is not accepted without readback");
    apply_io.fail_readback = false;
    check(apply_state.restore_and_verify(&apply_io),
          "failed apply readback remains restorable");
}

void cleanup_write_failure_is_explicit() {
    FakeIo io;
    asicen::LinkSeedState state;
    std::array<std::uint8_t, 16> supplied{};
    check(state.snapshot(&io) && state.apply(&io, supplied.data(), supplied.size()),
          "write failure case setup succeeds");
    io.writes = 0;
    io.fail_write_at = 2;
    check(!state.restore_and_verify(&io), "cleanup write failure is explicit");
}
}  // namespace

int main() {
    verify_wire_order_and_restore();
    partial_write_unwinds();
    readback_failure_is_not_success();
    cleanup_write_failure_is_explicit();
}
