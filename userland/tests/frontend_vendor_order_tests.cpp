#include "asicen/frontend_sequence.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <utility>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const char* label) {
    if (!ok) { std::cerr << "FAIL: " << label << '\n'; ++failures; }
}

// Decode the emitted wire requests, rather than trusting planner labels or
// counting FrontendOps. No USB device or vendor code is involved.
class WireTrace final : public asicen::FrontendTransport {
public:
    int control(const asicen::ControlTransfer& t, unsigned char* data) override {
        if (control_count++ == fail_at) return -1;
        std::fill_n(data, t.length, 0);
        if (t.length) data[0] = 1;
        const auto req = static_cast<std::uint8_t>(t.request);
        if (req == 0x0d) {
            const unsigned offset = t.value & 0xff;
            const unsigned char bytes[] = {
                static_cast<unsigned char>(t.value >> 8),
                static_cast<unsigned char>(t.index),
                static_cast<unsigned char>(t.index >> 8)};
            for (unsigned i = 0; i + 1 < t.length && i < 3; ++i)
                if (offset + i < staging.size()) staging[offset + i] = bytes[i];
        } else if (req == 0x0e && t.value == 0x0030 && t.length == 5) {
            check(staging[0] == 0xfe && staging[1] == 0xc6,
                  "FC0012 write bridge prefix");
            tuner_writes.emplace_back(staging[2], staging[3]);
            events.push_back(0x10000U | (unsigned(staging[2]) << 8) | staging[3]);
        } else if (req == 0x03 && t.value == 0xfe30 && t.length == 3) {
            tuner_reg = static_cast<unsigned char>(t.index >> 8);
        } else if (req == 0x19 && t.value == 0x0030 && t.length == 2) {
            if (tuner_reg == 0x0e) {
                ++feedback_reads;
                data[1] = read_index < tuner_0e.size() ? tuner_0e[read_index++] : 0x40;
            } else {
                data[1] = 0;
            }
        } else if (req == 0x02 && t.value == 0x1c30) {
            check(t.index == 0 && t.length == 2, "retry 1c mode0 read");
            ++demod_1c_reads;
            events.push_back(0x50000U);
            data[1] = demod_1c_reads == 1 ? 0x81 : 0x55;
        } else if (req == 0x03 && t.value == 0x1c30) {
            demod_1c_writes.push_back(static_cast<unsigned char>(t.index));
            events.push_back(0x60000U | (t.index & 0xffU));
        } else if (req == 0x02 && t.value == 0x1e30) {
            events.push_back(0x30000U);
        }
        return t.length;
    }
    void delay_ms(unsigned ms) override {
        events.push_back(0x20000U | ms);
        if (ms == cancel_on_delay) cancel = true;
        if (ms == expire_on_delay) expire = true;
    }
    bool cancelled() const override { return cancel; }
    bool expired() const override { return expire; }
    std::vector<unsigned char> staging = std::vector<unsigned char>(32);
    std::vector<unsigned char> tuner_0e;
    std::vector<std::pair<unsigned char, unsigned char>> tuner_writes;
    std::vector<unsigned char> demod_1c_writes;
    std::vector<unsigned> events;
    unsigned demod_1c_reads = 0;
    unsigned feedback_reads = 0;
    std::size_t control_count = 0;
    std::size_t fail_at = static_cast<std::size_t>(-1);
    unsigned cancel_on_delay = 0;
    unsigned expire_on_delay = 0;
    bool cancel = false;
    bool expire = false;
    std::size_t read_index = 0;
    unsigned char tuner_reg = 0;
};

std::vector<unsigned char> values(const WireTrace& trace, unsigned reg) {
    std::vector<unsigned char> out;
    for (const auto& pair : trace.tuner_writes)
        if (pair.first == reg) out.push_back(pair.second);
    return out;
}

void check_calibration(unsigned frequency, unsigned char feedback, bool adjust) {
    WireTrace trace;
    trace.tuner_0e = {feedback};
    const auto result = asicen::run_frontend_plan(
        asicen::plan_fc0012_tune(frequency), &trace);
    check(result == asicen::FrontendRunResult::Completed, "calibration completes");
    // TunerControl.o Adpater_SetFreqISDBT .text 1d91..1e51: one
    // 80,00,delay(1),00 train, then read feedback. Optional adjustment at
    // 1fbd/2076 rejoins 1fca..2039: reg6,80,00,delay(1) in BOTH branches.
    const std::vector<unsigned char> expected = adjust
        ? std::vector<unsigned char>{0x80, 0, 0, 0x80, 0}
        : std::vector<unsigned char>{0x80, 0, 0};
    check(values(trace, 0x0e) == expected, "exact source VCO pulse sequence");
    check(trace.feedback_reads == 1, "one calibration feedback read");
    const auto first = std::find(trace.events.begin(), trace.events.end(), 0x10e80U);
    const std::vector<unsigned> initial{0x10e80U, 0x10e00U, 0x20001U, 0x10e00U};
    check(first != trace.events.end() && trace.events.end() - first >= 4 &&
              std::equal(initial.begin(), initial.end(), first),
          "initial VCO delay is between clear and settle writes");
    const auto pll = asicen::compute_fc0012_pll(frequency);
    const auto reg6 = values(trace, 0x06);
    const auto expected6 = static_cast<unsigned char>(adjust
        ? (pll.vco_select ? (pll.reg6 & 0xf7U) : (pll.reg6 | 0x08U))
        : pll.reg6);
    check(!reg6.empty() && reg6.back() == expected6, "source reg6 correction value");
    const auto read = std::find(trace.events.begin(), trace.events.end(), 0x30000U);
    check(read != trace.events.end(), "demod AGC read follows calibration");
    if (adjust && read != trace.events.end() && read != trace.events.begin())
        check(*(read - 1) == 0x20001U, "both adjustment branches settle before AGC");
}
}

int main() {
    check_calibration(557143, 0x20, false); // high VCO, no adjustment
    check_calibration(557143, 0x3c, false); // exact upper threshold
    check_calibration(557143, 0x3d, true);  // high VCO: clear reg6 bit3
    check_calibration(473143, 0x02, false);// exact lower threshold
    check_calibration(473143, 0x01, true); // low VCO: set reg6 bit3

    WireTrace retry;
    retry.tuner_0e = {0x20, 0x00, 0x20, 0x40};
    check(asicen::run_frontend_plan(
              asicen::plan_terrestrial_tune_full(557142, 6), &retry) ==
              asicen::FrontendRunResult::Completed, "retry then success");
    // TC_SetFrequency .text 24b7 reads once. 24db OR30, 250e AND ef
    // operate on the SAME saved byte. A second read changes this behavior.
    check(retry.demod_1c_reads == 1, "retry reset reads 1c only once");
    check(retry.demod_1c_writes == std::vector<unsigned char>({0xb1, 0xa1}),
          "retry reset reuses original 1c snapshot");
    check(retry.feedback_reads == 4, "two calibration plus two outer feedback reads");
    const auto reset = std::find(retry.events.begin(), retry.events.end(), 0x50000U);
    const std::vector<unsigned> reset_events{
        0x50000U, 0x600b1U, 0x2000aU, 0x600a1U, 0x2000aU};
    check(reset != retry.events.end() && retry.events.end() - reset >= 5 &&
              std::equal(reset_events.begin(), reset_events.end(), reset),
          "source retry read/write/delay ordering");

    // Every emitted transfer is a stop-on-failure boundary, including inside
    // calibration and the saved-value reset helper. Existing tests remain intact.
    for (std::size_t i = 0; i < retry.control_count; ++i) {
        WireTrace failed;
        failed.tuner_0e = {0x20, 0x00, 0x20, 0x40};
        failed.fail_at = i;
        check(asicen::run_frontend_plan(
                  asicen::plan_terrestrial_tune_full(557142, 6), &failed) ==
                  asicen::FrontendRunResult::FailedTransfer, "stop on each wire failure");
        check(failed.control_count == i + 1, "no transfer after a failure");
    }
    for (bool expire : {false, true}) {
        WireTrace interrupted;
        interrupted.tuner_0e = {0x20, 0x00};
        if (expire) interrupted.expire_on_delay = 10;
        else interrupted.cancel_on_delay = 10;
        check(asicen::run_frontend_plan(
                  asicen::plan_terrestrial_tune_full(557142, 6), &interrupted) ==
                  (expire ? asicen::FrontendRunResult::DeadlineExceeded
                          : asicen::FrontendRunResult::Cancelled),
              "retry delay honors deadline/cancellation before next write");
        check(interrupted.demod_1c_writes == std::vector<unsigned char>({0xb1}),
              "no retry clear after interruption");
    }
    return failures == 0 ? 0 : 1;
}
