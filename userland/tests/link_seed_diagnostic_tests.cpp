// SPDX-License-Identifier: GPL-2.0-only
#include "asicen/link_seed_diagnostic.h"
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

class FakeIo final : public asicen::LinkSeedDiagnosticIo {
public:
    std::array<std::uint8_t, 16> write_window{};
    std::uint8_t controller = 0;
    int fail_write_at = -1;
    int writes = 0;
    bool fail_read05 = false;
    std::vector<std::string> trace;

    bool read_controller05(std::uint8_t* out) override {
        trace.emplace_back("read05");
        if (fail_read05 || out == nullptr) return false;
        *out = controller;
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
        if (writes == fail_write_at || reg < 0x10 || reg > 0x1f) return false;
        write_window[reg - 0x10] = value;
        return true;
    }
};

void wire_numbers_are_exact() {
    const auto controller_read = asicen::make_i2c_read(0x4a, 0x05, 1, 0, 1000);
    check(static_cast<std::uint8_t>(controller_read.request) == 0x02 &&
              controller_read.value == 0x054a && controller_read.index == 0 &&
              controller_read.length == 2 &&
              controller_read.direction == asicen::Direction::In,
          "controller05 snapshot is request02 mode0 with status+one byte");

    const std::uint8_t value = 0x5a;
    asicen::ControlTransfer seed_write{};
    check(asicen::make_i2c_write_chunk(0x4a, 0x10, &value, 1, false,
                                       &seed_write, 1000),
          "single seed byte write packet builds");
    check(static_cast<std::uint8_t>(seed_write.request) == 0x03 &&
              seed_write.value == 0x104a && seed_write.index == 0x005a &&
              seed_write.length == 2 && seed_write.direction == asicen::Direction::In,
          "seed byte wire fields match recovered write helper");

    const std::uint8_t ready = 0xa0;
    asicen::ControlTransfer output_write{};
    check(asicen::make_i2c_write_chunk(0x4a, 0x05, &ready, 1, false,
                                       &output_write, 1000) &&
              output_write.value == 0x054a && output_write.index == 0x00a0,
          "controller05 ready request encodes value a0");
    const std::uint8_t clear = 0;
    asicen::ControlTransfer clear_write{};
    check(asicen::make_i2c_write_chunk(0x4a, 0x1f, &clear, 1, false,
                                       &clear_write, 1000) &&
              clear_write.value == 0x1f4a && clear_write.index == 0,
          "cleanup sends zero at final seed-window address");
}

void idle_only_apply_and_honest_cleanup() {
    FakeIo io;
    asicen::LinkSeedDiagnostic state;
    std::array<std::uint8_t, 16> supplied{};
    for (std::size_t i = 0; i < supplied.size(); ++i)
        supplied[i] = static_cast<std::uint8_t>(i + 1U);
    check(state.snapshot_idle(&io), "only idle controller state snapshots");
    check(io.trace == std::vector<std::string>({"read05"}),
          "snapshot reads controller05 only; seed window is not called a snapshot");

    io.trace.clear();
    check(state.apply(&io, supplied.data(), supplied.size()),
          "seed writes acknowledged and controller05 readback succeeds");
    check(io.trace.size() == 18 && io.trace.front() == "write:16:1" &&
              io.trace[15] == "write:31:16" && io.trace[16] == "write05:160" &&
              io.trace[17] == "read05",
          "apply writes 10..1f, sets a0, and verifies only controller05");
    check(io.controller == 0xa0, "controller05 reaches ready value");

    io.trace.clear();
    check(state.clear_and_verify_controller(&io),
          "zero writes acknowledged and controller05 returns to zero");
    check(io.trace.size() == 18 && io.trace.front() == "write:16:0" &&
              io.trace[15] == "write:31:0" && io.trace[16] == "write05:0" &&
              io.trace[17] == "read05",
          "cleanup zeros all registers then verifies controller05 only");
    check(!state.active(), "successful cleanup releases diagnostic state");

    io.controller = 0x20;
    asicen::LinkSeedDiagnostic non_idle;
    io.trace.clear();
    check(!non_idle.snapshot_idle(&io) && io.trace.size() == 1,
          "nonzero controller05 aborts before any seed write");
}

void partial_application_gets_zero_cleanup() {
    FakeIo io;
    asicen::LinkSeedDiagnostic state;
    std::array<std::uint8_t, 16> supplied{};
    check(state.snapshot_idle(&io), "partial case snapshots idle controller");
    io.fail_write_at = 4;
    check(!state.apply(&io, supplied.data(), supplied.size()),
          "partial seed write is reported");
    io.fail_write_at = -1;
    io.trace.clear();
    check(state.clear_and_verify_controller(&io),
          "partial write path sends acknowledged zero cleanup");
    check(io.trace.front() == "write:16:0" && io.trace[15] == "write:31:0" &&
              io.controller == 0,
          "partial path does not write purported previous seed bytes");
}

void cleanup_failure_is_explicit_and_retryable() {
    FakeIo io;
    asicen::LinkSeedDiagnostic state;
    std::array<std::uint8_t, 16> supplied{};
    check(state.snapshot_idle(&io) && state.apply(&io, supplied.data(), supplied.size()),
          "cleanup failure case initializes");
    io.fail_read05 = true;
    check(!state.clear_and_verify_controller(&io) && state.active(),
          "failed controller readback is explicit and retains cleanup state");
    io.fail_read05 = false;
    check(state.clear_and_verify_controller(&io) && !state.active(),
          "cleanup can retry after readback failure");

    FakeIo failed_write;
    asicen::LinkSeedDiagnostic failed_state;
    check(failed_state.snapshot_idle(&failed_write), "write failure state snapshots");
    check(failed_state.apply(&failed_write, supplied.data(), supplied.size()),
          "write failure state applies");
    failed_write.writes = 0;
    failed_write.fail_write_at = 2;
    check(!failed_state.clear_and_verify_controller(&failed_write),
          "zero-write ACK failure is an explicit cleanup failure");
}
}  // namespace

int main() {
    wire_numbers_are_exact();
    idle_only_apply_and_honest_cleanup();
    partial_application_gets_zero_cleanup();
    cleanup_failure_is_explicit_and_retryable();
}
