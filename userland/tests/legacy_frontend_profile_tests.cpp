// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/frontend_sequence.h"
#include "asicen/satellite_tune.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <utility>
#include <vector>

namespace {
using namespace asicen;
using Profile = LegacyFrontendProfile;
#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #condition);           \
            return false;                                                                          \
        }                                                                                          \
    } while (false)

bool check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
    return condition;
}

using Pair = std::pair<std::uint8_t, std::uint8_t>;

class Trace final : public FrontendTransport {
  public:
    int control(const ControlTransfer& t, unsigned char* data) override
    {
        const int current = static_cast<int>(transfers.size());
        transfers.push_back(t);
        if (t.request == Request::Gpio) {
            const auto mask = static_cast<std::uint8_t>(t.value >> 8U);
            const auto value = static_cast<std::uint8_t>(t.value);
            gpio_state = static_cast<std::uint8_t>((gpio_state & ~mask) | (value & mask));
        }
        std::fill(data, data + t.length, 0);
        if (t.length != 0)
            data[0] = 1;
        if (t.request == Request::I2cBufferFill) {
            const std::size_t at = t.value & 0xffU;
            const std::array<std::uint8_t, 3> bytes{{static_cast<std::uint8_t>(t.value >> 8U),
                                                     static_cast<std::uint8_t>(t.index),
                                                     static_cast<std::uint8_t>(t.index >> 8U)}};
            for (std::size_t i = 0; i + 1 < t.length; ++i)
                staging[at + i] = bytes[i];
        }
        if (t.request == Request::I2cBufferSend && t.length == 5 && staging[0] == 0xfe &&
            staging[1] == 0xc6)
            tuner_writes.emplace_back(staging[2], staging[3]);
        if (t.request == Request::I2cWrite && t.value == 0xfe30 && t.length == 3)
            tuner_reg = static_cast<std::uint8_t>(t.index >> 8U);
        if (t.request == Request::I2cReadNoWait && t.length > 1)
            data[1] = tuner_reg == 0x0e ? vco_read : 0;
        if (t.request == Request::I2cRead && t.length > 1) {
            if (t.value == 0x1c30)
                data[1] = demod1c_reads++ == 0 ? 0x81 : 0x02;
            if (t.value == 0xb030)
                data[1] = 0x09;
            if (t.value == 0xce32 || t.value == 0x8f32) {
                std::fill(data + 1, data + t.length, 0xff);
                data[1] = 0x12;
                data[2] = 0x34;
            }
        }
        if (current == fail_at)
            return -1;
        if (current == short_at)
            return static_cast<int>(t.length) - 1;
        if (current == status_at && t.length != 0)
            data[0] = 0;
        return t.length;
    }
    void delay_ms(unsigned ms) override
    {
        delays.push_back(ms);
    }
    bool cancelled() const override
    {
        return cancel || (cancel_after >= 0 && static_cast<int>(transfers.size()) >= cancel_after);
    }
    bool expired() const override
    {
        return expire;
    }
    std::vector<ControlTransfer> transfers;
    std::vector<unsigned> delays;
    std::vector<Pair> tuner_writes;
    std::array<std::uint8_t, 256> staging{};
    std::uint8_t tuner_reg = 0, vco_read = 0x20, gpio_state = 0xff;
    int demod1c_reads = 0, fail_at = -1, short_at = -1, status_at = -1;
    int cancel_after = -1;
    bool cancel = false, expire = false;
};

bool same(const ControlTransfer& a, const ControlTransfer& b)
{
    return a.request == b.request && a.value == b.value && a.index == b.index &&
           a.length == b.length && a.direction == b.direction && a.tuner_num == b.tuner_num;
}
std::vector<Pair> demod_writes(const Trace& trace, std::uint8_t slave)
{
    std::vector<Pair> out;
    for (const auto& t : trace.transfers)
        if (t.request == Request::I2cWrite && (t.value & 0xffU) == slave && t.length == 2)
            out.emplace_back(static_cast<std::uint8_t>(t.value >> 8U),
                             static_cast<std::uint8_t>(t.index));
    return out;
}
std::size_t count_tuner(const Trace& trace, std::uint8_t reg)
{
    return static_cast<std::size_t>(std::count_if(trace.tuner_writes.begin(),
                                                  trace.tuner_writes.end(),
                                                  [reg](const Pair& p) { return p.first == reg; }));
}
bool check_power(Profile profile, bool on,
                 const std::vector<std::pair<Request, std::uint16_t>>& expected,
                 const std::vector<unsigned>& delays)
{
    const auto plan = plan_legacy_frontend_power(profile, on);
    Trace trace;
    CHECK(check(run_frontend_plan(plan, &trace) == FrontendRunResult::Completed,
                "power plan executes on mock"));
    CHECK(check(trace.transfers.size() == expected.size(), "exact power transfer count"));
    for (std::size_t i = 0; i < std::min(trace.transfers.size(), expected.size()); ++i)
        CHECK(check(trace.transfers[i].request == expected[i].first &&
                        trace.transfers[i].value == expected[i].second,
                    "exact power order/mask"));
    CHECK(check(trace.delays == delays, "exact power delays"));
    for (std::size_t fail = 0; fail < trace.transfers.size(); ++fail) {
        Trace broken;
        broken.fail_at = static_cast<int>(fail);
        CHECK(check(run_frontend_plan(plan, &broken) == FrontendRunResult::FailedTransfer &&
                        broken.transfers.size() == fail + 1,
                    "power stops on each failed transfer"));
    }
    return true;
}
bool powered_init(Profile profile)
{
    auto power = plan_legacy_frontend_startup(profile);
    Trace startup_trace;
    CHECK(check(run_frontend_plan(power, &startup_trace) == FrontendRunResult::Completed,
                "full legacy cold startup completes"));
    const std::vector<std::pair<Request, std::uint16_t>> expected_startup{
        {Request::Gpio, 0x4000},    {Request::Gpio, 0x0800}, {Request::Gpio, 0x1010},
        {Request::Gpio, 0x1000},    {Request::Gpio, 0x1010}, {Request::Gpio, 0x0404},
        {Request::Gpio, 0x0400},    {Request::Gpio, 0x0404}, {Request::I2cRead, 0x00a8},
        {Request::I2cRead, 0xb0a8}, {Request::Gpio, 0xfb0f}, {Request::Gpio, 0x4040}};
    CHECK(check(startup_trace.transfers.size() == expected_startup.size(),
                "complete cold prefix plus tail count"));
    for (std::size_t i = 0; i < expected_startup.size(); ++i)
        CHECK(check(startup_trace.transfers[i].request == expected_startup[i].first &&
                        startup_trace.transfers[i].value == expected_startup[i].second,
                    "exact DTV_Start cold prefix/probe/tail order"));
    CHECK(check(startup_trace.delays == std::vector<unsigned>{50, 50, 10, 10, 10, 10, 10, 10},
                "exact cold prefix delays"));
    CHECK(check(startup_trace.transfers[8].length == 2 && startup_trace.transfers[9].length == 17 &&
                    startup_trace.transfers[8].index == 0 && startup_trace.transfers[9].index == 0,
                "cold A8 probes length and I2C mode"));
    const auto startup_off = plan_legacy_frontend_startup_off(profile);
    Trace off_trace;
    CHECK(check(run_frontend_plan(startup_off, &off_trace) == FrontendRunResult::Completed,
                "source startup-off stage completes"));
    if (profile == Profile::S3u) {
        CHECK(check(off_trace.transfers.size() == 2 && off_trace.transfers[0].value == 0x0808 &&
                        off_trace.transfers[1].value == 0x0808 &&
                        off_trace.delays == std::vector<unsigned>{50, 50},
                    "S3U startup invokes off twice"));
    } else {
        CHECK(check(off_trace.transfers.size() == 7 &&
                        off_trace.transfers[2].request == Request::GpioExSet &&
                        off_trace.transfers[2].value == 0x0202 &&
                        off_trace.transfers[5].request == Request::GpioExSet &&
                        off_trace.transfers[5].value == 0x0101 &&
                        off_trace.delays == std::vector<unsigned>{200},
                    "S3U2 startup releases both Ex lines before power-on"));
    }
    power.insert(power.end(), startup_off.begin(), startup_off.end());
    const std::size_t prelude_at = startup_trace.transfers.size() + off_trace.transfers.size();
    const auto prelude = plan_legacy_frontend_init_prelude(profile);
    CHECK(check(prelude.size() == 2 && prelude[0].kind == FrontendOpKind::Control &&
                    prelude[0].transfer.request == Request::Gpio &&
                    prelude[0].transfer.value == 0x0800 &&
                    prelude[1].kind == FrontendOpKind::Delay && prelude[1].delay_ms == 50,
                "DTV_Init prelude is exact GPIO08 clear then50ms"));
    power.insert(power.end(), prelude.begin(), prelude.end());
    const auto tc_power = plan_legacy_frontend_power(profile, true);
    power.insert(power.end(), tc_power.begin(), tc_power.end());
    Trace trace;
    bool guarded = false, initialized = false;
    const auto result = execute_powered_init_sequence(
        [&] { return run_frontend_plan(power, &trace) == FrontendRunResult::Completed; },
        [&] {
            guarded = true;
            CHECK(check((trace.gpio_state & 0x08U) == 0,
                        "GPIO08 is powered low before controller guard"));
            CHECK(
                check(trace.transfers.size() >= 4 && trace.transfers[10].value == 0xfb0f &&
                          trace.transfers[11].value == 0x4040 &&
                          trace.transfers[prelude_at].value == 0x0800 &&
                          trace.delays[startup_trace.delays.size() + off_trace.delays.size()] == 50,
                      "prelude follows startup-off and precedes isolated TC power"));
            for (std::size_t i = 0; i < off_trace.transfers.size(); ++i)
                CHECK(check(same(trace.transfers[startup_trace.transfers.size() + i],
                                 off_trace.transfers[i]),
                            "source startup-off transfers retained in full composition"));
            return (trace.gpio_state & 0x08U) == 0;
        },
        [&] {
            CHECK(check(guarded, "shared init follows powered controller guard"));
            initialized = true;
            return run_frontend_plan(plan_legacy_frontend_init(profile), &trace) ==
                   FrontendRunResult::Completed;
        });
    CHECK(check(result == PoweredInitResult::completed && guarded && initialized,
                "legacy full powered init composition completes"));
    Trace failed;
    failed.fail_at = static_cast<int>(prelude_at);
    guarded = false;
    initialized = false;
    const auto stopped = execute_powered_init_sequence(
        [&] { return run_frontend_plan(power, &failed) == FrontendRunResult::Completed; },
        [&] {
            guarded = true;
            return true;
        },
        [&] {
            initialized = true;
            return true;
        });
    CHECK(check(stopped == PoweredInitResult::power_failed && !guarded && !initialized &&
                    failed.transfers.size() == prelude_at + 1,
                "prelude failure prevents controller/init access"));
    return true;
}
bool init(Profile profile)
{
    const std::vector<Pair> terrestrial{{0x47, 0},    {0x75, 2},    {0xb0, 0xa0}, {0xb2, 0x3d},
                                        {0xb3, 0x25}, {0xb4, 0x8b}, {0xb5, 0x4b}, {0xb6, 0x3f},
                                        {0xb7, 0xff}, {0xb8, 0xff}, {0x22, 0x8f}, {0x5f, 0x80},
                                        {0xef, 1}};
    const auto plan = plan_legacy_frontend_init(profile);
    Trace trace;
    CHECK(check(run_frontend_plan(plan, &trace) == FrontendRunResult::Completed,
                "legacy init executes"));
    auto expected_t = terrestrial;
    if (profile == Profile::S3u2)
        expected_t.emplace_back(0x0f, 0x34);
    CHECK(check(demod_writes(trace, 0x30) == expected_t, "exact legacy T init pairs"));
    const auto sat = demod_writes(trace, 0x32);
    CHECK(check(sat.size() == 42, "exact satellite init count"));
    for (std::size_t i = 0; i < sat.size(); ++i)
        CHECK(check(sat[i] == Pair{satellite_demod_init_reg(i), satellite_demod_init_value(i)},
                    "satellite init equals source facts"));
    std::vector<Pair> expected_rf;
    for (std::size_t i = 0; i < fc0012_init_count(); ++i)
        expected_rf.emplace_back(fc0012_init_reg(i), fc0012_init_value(i));
    if (profile == Profile::S3u2)
        for (int pass = 0; pass != 2; ++pass)
            for (std::uint8_t value : {0, 0x10, 0})
                expected_rf.emplace_back(0x10, value);
    CHECK(check(trace.tuner_writes == expected_rf, "RF init and exact extra S3U2 pulses"));
    CHECK(check(plan.size() == (profile == Profile::S3u ? 118U : 137U),
                "exact init operation count"));
    CHECK(check(plan.front().transfer.value == (profile == Profile::S3u ? 0x0132 : 0x4730),
                "model-specific first init source"));
    if (profile == Profile::S3u2)
        CHECK(check(plan[13 + 63 + 9].transfer.value == 0x0132,
                    "S3U2 source1 follows first RF pulse"));
    for (const auto& t : trace.transfers)
        CHECK(check(t.request != Request::Gpio && t.request != Request::GpioExSet &&
                        (t.value & 0xffU) != 0x34 && (t.value & 0xffU) != 0x36,
                    "init uses index0 and no hidden GPIO"));
    return true;
}
bool tune(Profile profile)
{
    const auto plan = plan_legacy_terrestrial_tune(profile, 557142, 6);
    Trace trace;
    CHECK(check(run_frontend_plan(plan, &trace) == FrontendRunResult::Completed,
                "legacy T completes"));
    const auto n = trace.transfers.size();
    CHECK(check(trace.transfers[0].value == 0x2530 && trace.transfers[0].index == 0 &&
                    trace.transfers[1].value == 0x2330 && trace.transfers[1].index == 0x4d,
                "legacy T prefix"));
    CHECK(check(trace.transfers[n - 3].value == 0x0f30 &&
                    trace.transfers[n - 3].index == (profile == Profile::S3u ? 0x14 : 0x34) &&
                    trace.transfers[n - 2].value == 0x0130 &&
                    trace.transfers[n - 2].index == 0x40 &&
                    trace.transfers[n - 1].value == 0x2330 && trace.transfers[n - 1].index == 0x4c,
                "legacy T source-specific tail"));
    CHECK(check(count_tuner(trace, 0x0e) == 3, "exactly one unconditional VCO train"));
    CHECK(check(count_tuner(trace, 0x13) == 0 && trace.demod1c_reads == 0,
                "legacy tune has no W3U3 pre-gain or retry"));
    unsigned agc_reads = 0;
    for (const auto& t : trace.transfers) {
        if (t.request == Request::I2cRead && t.value == 0x1e30)
            ++agc_reads;
        CHECK(check(t.request != Request::Gpio && t.request != Request::GpioExSet,
                    "legacy T has no W3U3 LNA GPIO"));
    }
    CHECK(check(agc_reads == (profile == Profile::S3u ? 0U : 1U), "model-specific AGC tail"));
    const auto lock = plan_legacy_terrestrial_lock(profile, 557142);
    CHECK(check(lock.size() == 1 && lock[0].transfer.value == 0xb030 && lock[0].transfer.index == 1,
                "legacy T lock source"));
    for (std::size_t fail = 0; fail < n; ++fail) {
        Trace bad;
        bad.fail_at = static_cast<int>(fail);
        CHECK(check(run_frontend_plan(plan, &bad) == FrontendRunResult::FailedTransfer &&
                        bad.transfers.size() == fail + 1,
                    "T fail closed at each transfer"));
    }
    Trace cancelled;
    cancelled.cancel = true;
    CHECK(check(run_frontend_plan(plan, &cancelled) == FrontendRunResult::Cancelled &&
                    cancelled.transfers.empty(),
                "legacy T cancellation before IO"));
    Trace expire;
    expire.expire = true;
    CHECK(check(run_frontend_plan(plan, &expire) == FrontendRunResult::DeadlineExceeded &&
                    expire.transfers.empty(),
                "legacy T deadline before IO"));
    Trace mid;
    mid.cancel_after = 4;
    CHECK(check(run_frontend_plan(plan, &mid) == FrontendRunResult::Cancelled &&
                    mid.transfers.size() == 4,
                "legacy T mid-sequence cancellation"));
    CHECK(check(plan_legacy_terrestrial_tune(profile, 0, 6).empty() &&
                    plan_legacy_terrestrial_tune(profile, 557142, 0).empty() &&
                    plan_legacy_terrestrial_tune(profile, 557142, 7).empty() &&
                    plan_legacy_terrestrial_tune(profile, 1000000, 6).empty() &&
                    plan_legacy_terrestrial_lock(profile, 0).empty() &&
                    plan_legacy_terrestrial_lock(profile, 1000000).empty(),
                "invalid T inputs"));
    return true;
}
bool gain(Profile profile)
{
    const auto plan = plan_legacy_default_gain(profile, true);
    CHECK(check(!plan.empty() && plan_legacy_default_gain(profile, false).empty(),
                "legacy gain requires explicit default state"));
    Trace trace;
    CHECK(check(run_frontend_plan(plan, &trace) == FrontendRunResult::Completed,
                "legacy gain completes once"));
    if (profile == Profile::S3u2) {
        CHECK(check(trace.tuner_writes == std::vector<Pair>{{0x13, 0x0f}} &&
                        trace.transfers.size() == 3 && trace.delays.empty(),
                    "S3U2 default cached-state branch is exactly13=0f"));
    } else {
        CHECK(check(plan.size() == 1 && plan[0].local == 0 && plan[0].source == 0 &&
                        plan[0].kind == FrontendOpKind::Fc0012GainOnce,
                    "S3U feedback uses physical index0/source0"));
        CHECK(check(!trace.tuner_writes.empty() && trace.tuner_writes.front() == Pair{0x12, 0},
                    "S3U feedback begins with source register12 reset"));
    }
    for (std::size_t failure = 0; failure < trace.transfers.size(); ++failure) {
        Trace failed;
        failed.fail_at = static_cast<int>(failure);
        CHECK(check(run_frontend_plan(plan, &failed) == FrontendRunResult::FailedTransfer &&
                        failed.transfers.size() == failure + 1,
                    "gain stops on each failed transfer"));
    }
    Trace cancelled;
    cancelled.cancel = true;
    CHECK(check(run_frontend_plan(plan, &cancelled) == FrontendRunResult::Cancelled &&
                    cancelled.transfers.empty(),
                "gain cancellation before I/O"));
    return true;
}
bool satellite(Profile profile)
{
    for (std::uint32_t row = 0; row < 24; ++row) {
        const auto rf = row < 12 ? 11727480U + row * 38360U : 12291000U + (row - 12U) * 40000U;
        const auto plan = plan_legacy_satellite_tune(profile, rf);
        const auto reference = plan_w3u3_satellite_tune(rf);
        CHECK(check(plan.size() == reference.size(), "satellite common operation count"));
        for (std::size_t i = 0; i < std::min(plan.size(), reference.size()); ++i) {
            if (plan[i].kind == FrontendOpKind::Delay) {
                CHECK(check(plan[i].delay_ms == reference[i].delay_ms, "satellite settle"));
            } else if (plan[i].transfer.value == 0x0f30) {
                CHECK(check(plan[i].transfer.index == (profile == Profile::S3u ? 0x3c : 0x34),
                            "satellite model-specific source selector"));
            } else
                CHECK(check(same(plan[i].transfer, reference[i].transfer),
                            "satellite adapter and TSID source equivalence"));
        }
    }
    Trace io;
    CHECK(check(run_legacy_satellite_tune(profile, &io, 11727480) ==
                    SatelliteOperationResult::Completed,
                "legacy S run"));
    CHECK(check(read_legacy_satellite_lock(profile, &io).locked, "legacy S lock"));
    CHECK(check(poll_legacy_satellite_lock(profile, &io, 1, 1).locked, "legacy S poll"));
    const auto list = read_legacy_satellite_tsids(profile, &io);
    CHECK(check(list.result == SatelliteOperationResult::Completed && list.tsids[0] == 0x1234,
                "legacy TSID byte order"));
    CHECK(check(select_legacy_satellite_tsid(profile, &io, 0, list.tsids).selected_tsid == 0x1234,
                "legacy TSID write/readback"));
    CHECK(check(wait_legacy_satellite_slot_ready(profile, &io, 0, 1, 1).slot == 0 &&
                    wait_legacy_satellite_tsid_ready(profile, &io, 0x1234, 1, 1).slot == 0 &&
                    wait_legacy_satellite_any_ready(profile, &io, 1, 1).slot == 0,
                "legacy TSID ready wrappers"));
    for (const auto& t : io.transfers)
        if (t.request == Request::I2cRead)
            CHECK(check((t.value & 0xffU) == 0x32, "satellite reads use index0 demod32"));
    CHECK(check(plan_legacy_satellite_tune(profile, 1).empty() &&
                    plan_legacy_satellite_tune(profile, 12291000, 0x1234).empty(),
                "legacy satellite rejects unknown RF/CS TSID"));
    Trace cancelled;
    cancelled.cancel = true;
    CHECK(check(run_legacy_satellite_tune(profile, &cancelled, 11727480) ==
                        SatelliteOperationResult::Cancelled &&
                    cancelled.transfers.empty(),
                "legacy satellite cancellation"));
    return true;
}
} // namespace

bool test_all()
{
    using namespace asicen;
    const auto gpio = Request::Gpio, ex = Request::GpioExSet;
    CHECK(check_power(Profile::S3u, true,
                      {{gpio, 0x0800},
                       {gpio, 0x4444},
                       {gpio, 0x4400},
                       {gpio, 0x4444},
                       {gpio, 0x2000},
                       {gpio, 0x2020}},
                      {50, 50, 50, 50, 100, 100}));
    CHECK(check_power(Profile::S3u, false, {{gpio, 0x0808}}, {50}));
    std::vector<unsigned> on_delays(16, 10);
    on_delays.push_back(100);
    CHECK(check_power(Profile::S3u2, true,
                      {{gpio, 0x0505},
                       {gpio, 0x0400},
                       {gpio, 0x0505},
                       {gpio, 0x2000},
                       {gpio, 0x2020},
                       {gpio, 0x2000},
                       {gpio, 0x1010},
                       {gpio, 0x1000},
                       {gpio, 0x1010},
                       {ex, 0x0100},
                       {gpio, 0x4040},
                       {gpio, 0x4000},
                       {gpio, 0x4040},
                       {gpio, 0x8000},
                       {gpio, 0x8080},
                       {gpio, 0x8000},
                       {ex, 0x0200},
                       {Request::I2cRead, 0x00a8}},
                      on_delays));
    CHECK(check_power(Profile::S3u2, false,
                      {{gpio, 0x4000},
                       {gpio, 0x8080},
                       {ex, 0x0202},
                       {gpio, 0x0400},
                       {gpio, 0x2020},
                       {ex, 0x0101},
                       {gpio, 0x0808}},
                      {200}));
    for (const auto profile : {Profile::S3u, Profile::S3u2}) {
        const auto startup = plan_legacy_frontend_startup(profile);
        CHECK(check(startup.size() == 20 && startup[18].transfer.value == 0xfb0f &&
                        startup[19].transfer.value == 0x4040,
                    "legacy ordinary full cold startup"));
        CHECK(check(plan_legacy_frontend_startup(profile, true).size() == 1,
                    "legacy16/52 startup omits GPIO40 assertion"));
        CHECK(powered_init(profile));
        CHECK(init(profile));
        CHECK(tune(profile));
        CHECK(gain(profile));
        CHECK(satellite(profile));
    }
    for (const auto center : {93143U, 93144U, 255144U, 261142U, 261143U, 767142U, 767143U}) {
        const auto plan = plan_legacy_fc0012_tune(Profile::S3u, center);
        const bool band = center >= 93144 && center <= 767142;
        CHECK(check(!plan.empty(), "band fixture valid PLL"));
        CHECK(check((plan[0].kind == FrontendOpKind::Control) == band, "band write boundary"));
        if (band)
            CHECK(check(plan[0].transfer.value == 0x1432 &&
                            plan[0].transfer.index == (center <= 261142 ? 0 : 0x20),
                        "S3U band write range and overlap priority"));
    }
    // Regression: high-VCO correction shares the same final1ms as low correction.
    for (const bool high : {false, true}) {
        FrontendOp op;
        op.kind = FrontendOpKind::Fc0012VcoCalibrate;
        op.local = 1;
        op.vco_select = high;
        op.reg6 = 0x88;
        Trace trace;
        trace.vco_read = high ? 0x3f : 0;
        CHECK(check(run_frontend_plan({op}, &trace) == FrontendRunResult::Completed,
                    "VCO correction completed"));
        CHECK(check(count_tuner(trace, 0x0e) == 5 && trace.delays == std::vector<unsigned>{1, 1},
                    "VCO single initial pulse train plus corrected fallback delay"));
    }
    // Regression: W3U3 retry reads1c once then writes saved|30 and saved&ef.
    Trace retry;
    CHECK(check(run_frontend_plan(plan_terrestrial_tune_full(557142, 6), &retry) ==
                    FrontendRunResult::Completed,
                "W3U3 retry regression completes"));
    CHECK(check(retry.demod1c_reads == 3, "W3U3 retry only one1c sample per retry"));
    std::vector<std::uint8_t> writes;
    for (const auto& t : retry.transfers)
        if (t.request == Request::I2cWrite && t.value == 0x1c30)
            writes.push_back(static_cast<std::uint8_t>(t.index));
    CHECK(check(writes == std::vector<std::uint8_t>{0xb1, 0xa1, 0x32, 0x22, 0x32, 0x22},
                "W3U3 retry keeps original sample for second write"));
    const auto invalid = static_cast<Profile>(255);
    CHECK(check(plan_legacy_frontend_startup(invalid).empty() &&
                    plan_legacy_frontend_power(invalid, true).empty() &&
                    plan_legacy_frontend_init_prelude(invalid).empty() &&
                    plan_legacy_frontend_startup_off(invalid).empty() &&
                    plan_legacy_frontend_init(invalid).empty() &&
                    plan_legacy_default_gain(invalid, true).empty() &&
                    plan_legacy_fc0012_tune(invalid, 557143).empty() &&
                    plan_legacy_terrestrial_tune(invalid, 557142, 6).empty() &&
                    plan_legacy_terrestrial_lock(invalid, 557142).empty() &&
                    plan_legacy_satellite_tune(invalid, 11727480).empty(),
                "invalid profile rejected"));
    Trace invalid_io;
    CHECK(check(read_legacy_satellite_lock(invalid, &invalid_io).result ==
                        SatelliteOperationResult::InvalidArgument &&
                    invalid_io.transfers.empty(),
                "invalid profile cannot become satellite alias"));
    // A cancellation arising from an RMW read must prevent its following write.
    FrontendOp rmw;
    rmw.kind = FrontendOpKind::I2cMask;
    rmw.transfer = make_i2c_read(0x30, 0x1e, 1, 1);
    Trace cancelled_rmw;
    cancelled_rmw.cancel_after = 1;
    CHECK(check(run_frontend_plan({rmw}, &cancelled_rmw) == FrontendRunResult::Cancelled &&
                    cancelled_rmw.transfers.size() == 1,
                "RMW cancellation before write"));
    return true;
}

int main()
{
    return test_all() ? 0 : 1;
}
