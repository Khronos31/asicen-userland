#include "asicen/frontend_sequence.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

int failures = 0;

void check(bool value, const char* name) {
    if (!value) {
        std::cerr << "FAIL: " << name << '\n';
        ++failures;
    }
}

class GainTransport final : public asicen::FrontendTransport {
public:
    explicit GainTransport(std::vector<std::uint8_t> reads)
        : reads_(reads.begin(), reads.end()) {}

    int control(const asicen::ControlTransfer& transfer, unsigned char* data) override {
        const std::size_t call_index = transfers.size();
        transfers.push_back(transfer);
        if (call_index == fail_at) return -1;
        for (std::uint16_t i = 0; i < transfer.length; ++i) data[i] = 0;
        if (transfer.length > 0) data[0] = 1;
        if (call_index == bad_status_at && transfer.length > 0) data[0] = 0;
        if (transfer.request == asicen::Request::I2cWrite &&
            static_cast<std::uint8_t>(transfer.value >> 8U) == 0xfeU &&
            static_cast<std::uint8_t>(transfer.index & 0xffU) == 0xc6U) {
            current_read_reg_ = static_cast<std::uint8_t>(transfer.index >> 8U);
        }
        if (transfer.request == asicen::Request::I2cReadNoWait && transfer.length > 1) {
            if (next_read_ >= reads_.size()) return -1;
            data[1] = reads_[next_read_++];
            read_regs.push_back(current_read_reg_);
        }
        if (call_index == short_at && transfer.length > 0)
            return static_cast<int>(transfer.length - 1U);
        return transfer.length;
    }
    void delay_ms(unsigned) override {}
    bool cancelled() const override { return transfers.size() >= cancel_after; }

    std::vector<asicen::ControlTransfer> transfers;
    std::vector<std::uint8_t> read_regs;
    std::size_t fail_at = static_cast<std::size_t>(-1);
    std::size_t short_at = static_cast<std::size_t>(-1);
    std::size_t bad_status_at = static_cast<std::size_t>(-1);
    std::size_t cancel_after = static_cast<std::size_t>(-1);

private:
    std::vector<std::uint8_t> reads_;
    std::size_t next_read_ = 0;
    std::uint8_t current_read_reg_ = 0;
};

std::vector<std::array<std::uint8_t, 2>> decoded_tuner_writes(
    const std::vector<asicen::ControlTransfer>& transfers) {
    std::vector<std::array<std::uint8_t, 2>> writes;
    std::array<std::uint8_t, 4> staged{};
    std::size_t staged_count = 0;
    for (const auto& transfer : transfers) {
        if (transfer.request == asicen::Request::I2cBufferFill) {
            const std::size_t offset = transfer.value & 0xffU;
            if (offset > staged.size()) continue;
            if (offset < staged.size()) staged[offset] = static_cast<std::uint8_t>(transfer.value >> 8U);
            if (offset + 1U < staged.size()) staged[offset + 1U] = static_cast<std::uint8_t>(transfer.index & 0xffU);
            if (offset + 2U < staged.size()) staged[offset + 2U] = static_cast<std::uint8_t>(transfer.index >> 8U);
            staged_count = offset + std::min<std::size_t>(3, staged.size() - offset);
        } else if (transfer.request == asicen::Request::I2cBufferSend) {
            if (staged_count == staged.size() && staged[0] == 0xfe && staged[1] == 0xc6)
                writes.push_back({staged[2], staged[3]});
            staged.fill(0);
            staged_count = 0;
        }
    }
    return writes;
}

void run_case(std::uint8_t mode, std::uint8_t gain_sample,
              std::uint8_t initial_d, std::vector<std::uint8_t> tail_reads,
              std::vector<std::array<std::uint8_t, 2>> expected_writes,
              const char* label) {
    std::vector<std::uint8_t> reads{gain_sample, mode, initial_d, 0x55};
    reads.insert(reads.end(), tail_reads.begin(), tail_reads.end());
    GainTransport transport(std::move(reads));
    const auto plan = asicen::plan_fc0012_gain_once(1, 0);
    asicen::FrontendRunReport report{};
    const auto result = asicen::run_frontend_plan(plan, &transport, &report);
    check(result == asicen::FrontendRunResult::Completed, label);
    check(decoded_tuner_writes(transport.transfers) == expected_writes,
          "conditional tuner write order/value");
}

}  // namespace

int main() {
    check(asicen::plan_fc0012_gain_once(2).empty(), "invalid local rejected");
    check(asicen::plan_fc0012_gain_once(1, 2).empty(), "invalid source rejected");

    // Source 1 is the early success/no-I/O branch at TunerControl.o:0x13bb.
    GainTransport no_op({});
    asicen::FrontendRunReport no_op_report{};
    check(asicen::run_frontend_plan(asicen::plan_fc0012_gain_once(1, 1),
                                    &no_op, &no_op_report) ==
              asicen::FrontendRunResult::Completed && no_op.transfers.empty(),
          "source 1 is a no-I/O success");

    // Thresholds recovered from TunerControl.o .data:0x168..0x17c;
    // synthetic coverage exercises each decision branch and equality edge.
    run_case(0x02, 251, 0x00, {}, {{0x12, 0}, {0x10, 0}},
             "mode2 equality at level6 is stable");
    run_case(0x02, 251, 0x10, {}, {{0x12, 0}},
             "mode2 with reg0d bit4 set skips initial reg10 reset");
    run_case(0x02, 252, 0xa2, {0xa2, 9, 0xa2},
             {{0x12, 0}, {0x10, 0}, {0x0d, 0xa2}, {0x10, 0},
              {0x0d, 0xb2}, {0x10, 6}, {0x13, 0x0a}},
             "mode2 above level6 performs feedback sequence");

    run_case(0x0a, 240, 0x00, {}, {{0x12, 0}, {0x10, 0}},
             "mode0a equality at level5 is stable");
    run_case(0x0a, 239, 0x00, {0x00, 0, 0x00},
             {{0x12, 0}, {0x10, 0}, {0x0d, 0}, {0x10, 0},
              {0x0d, 0x10}, {0x10, 0xfd}, {0x13, 0x02}},
             "mode0a below level5 adjusts gain");
    run_case(0x0a, 253, 0x00, {0x00},
             {{0x12, 0}, {0x10, 0}, {0x0d, 0}, {0x13, 0x14}},
             "mode0a above level4 advances mode");

    run_case(0x14, 245, 0x00, {}, {{0x12, 0}, {0x10, 0}},
             "mode14 equality at level3 is stable");
    run_case(0x14, 244, 0x00, {0x00, 0, 0x00},
             {{0x12, 0}, {0x10, 0}, {0x0d, 0}, {0x10, 0},
              {0x0d, 0x10}, {0x10, 0xfd}, {0x13, 0x0a}},
             "mode14 below level3 adjusts gain");
    run_case(0x14, 252, 0x00, {0x00},
             {{0x12, 0}, {0x10, 0}, {0x0d, 0}, {0x13, 0x10}},
             "mode14 above level2 advances mode");

    run_case(0x10, 249, 0x00, {0x00},
             {{0x12, 0}, {0x10, 0}, {0x0d, 0}, {0x13, 0x14}},
             "mode10 below level1 advances mode");
    run_case(0x10, 250, 0x00, {}, {{0x12, 0}, {0x10, 0}},
             "mode10 equality at level1 is stable");
    run_case(0x03, 249, 0x00, {0x00},
             {{0x12, 0}, {0x10, 0}, {0x0d, 0}, {0x13, 0x10}},
             "other mode takes fallback branch");

    GainTransport read_order({252, 0x02, 0xa2, 0x00, 0xa2, 9, 0xa2});
    check(asicen::run_frontend_plan(asicen::plan_fc0012_gain_once(1),
                                    &read_order) == asicen::FrontendRunResult::Completed,
          "source-derived gain branch completes");
    check(read_order.read_regs == std::vector<std::uint8_t>({0x12, 0x13, 0x0d,
                                                              0x10, 0x0d, 0x10, 0x0d}),
          "gain branch read-register order");

    const auto stop_before_more_writes = [&](std::size_t failure_at,
                                             std::size_t short_at,
                                             std::size_t bad_status_at,
                                             std::size_t cancel_after,
                                             asicen::FrontendRunResult expected,
                                             const char* name) {
        GainTransport transport({251, 0x02, 0x00, 0x00, 0x00, 9, 0x00});
        transport.fail_at = failure_at;
        transport.short_at = short_at;
        transport.bad_status_at = bad_status_at;
        transport.cancel_after = cancel_after;
        const auto result = asicen::run_frontend_plan(
            asicen::plan_fc0012_gain_once(1), &transport);
        check(result == expected, name);
        check(decoded_tuner_writes(transport.transfers) ==
                  std::vector<std::array<std::uint8_t, 2>>{{0x12, 0}},
              "gain operation stops before later writes on failure/cancel");
    };
    // The initial tuner-register write is three transfers; transfer 3 starts
    // the first read. Any transport/status/cancel failure must stop there.
    stop_before_more_writes(3, static_cast<std::size_t>(-1),
                            static_cast<std::size_t>(-1),
                            static_cast<std::size_t>(-1),
                            asicen::FrontendRunResult::FailedTransfer,
                            "gain first read failure stops");
    stop_before_more_writes(static_cast<std::size_t>(-1), 3,
                            static_cast<std::size_t>(-1),
                            static_cast<std::size_t>(-1),
                            asicen::FrontendRunResult::ShortTransfer,
                            "gain short read setup stops");
    stop_before_more_writes(static_cast<std::size_t>(-1),
                            static_cast<std::size_t>(-1), 3,
                            static_cast<std::size_t>(-1),
                            asicen::FrontendRunResult::FailedTransfer,
                            "gain bad status stops");
    stop_before_more_writes(static_cast<std::size_t>(-1),
                            static_cast<std::size_t>(-1),
                            static_cast<std::size_t>(-1), 3,
                            asicen::FrontendRunResult::Cancelled,
                            "gain cancellation stops before next operation");

    return failures == 0 ? 0 : 1;
}
