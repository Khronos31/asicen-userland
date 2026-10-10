// SPDX-License-Identifier: GPL-2.0-only
#include "asicen/v2_frontend.h"
#include "asicen/write_protocol.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <map>
#include <tuple>
#include <vector>

namespace {
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

using Key = std::tuple<std::uint8_t, bool, std::uint16_t>;
class RegisterTransport final : public asicen::FrontendTransport {
  public:
    std::vector<asicen::ControlTransfer> calls;
    std::vector<unsigned> delays;
    std::map<Key, std::uint32_t> registers;
    std::array<std::uint8_t, 64> staged{};
    std::uint16_t selected_reg = 0;
    bool selected_tuner = false;
    bool calibration_ready = true, locked = true, cancel = false, expire = false;
    int fail_at = -1, short_at = -1, bad_status_at = -1;
    unsigned por_ready_after = 1, channel_ready_after = 1, por_reads = 0, channel_reads = 0;
    RegisterTransport()
    {
        for (auto slave : {0x20, 0x24, 0x28, 0x2c})
            registers[{slave, true, 0x3fc}] = 0x12000;
        for (auto slave : {0x22, 0x26, 0x2a, 0x2e})
            registers[{slave, true, 0x21}] = 0x40;
    }
    int control(const asicen::ControlTransfer& c, unsigned char* data) override
    {
        const int index = static_cast<int>(calls.size());
        calls.push_back(c);
        std::fill(data, data + c.length, 0);
        if (c.length)
            data[0] = 1;
        if (index == fail_at)
            return -1;
        if (index == short_at)
            return c.length - 1;
        if (index == bad_status_at) {
            data[0] = 0;
            return c.length;
        }
        if (c.request == asicen::Request::I2cBufferFill) {
            const auto offset = c.value & 0xffU;
            const std::uint8_t bytes[] = {static_cast<std::uint8_t>(c.value >> 8),
                                          static_cast<std::uint8_t>(c.index),
                                          static_cast<std::uint8_t>(c.index >> 8)};
            for (unsigned i = 0; i < c.length - 1U; ++i)
                staged.at(offset + i) = bytes[i];
        } else if (c.request == asicen::Request::I2cBufferSend) {
            const auto slave = static_cast<std::uint8_t>(c.value);
            const auto size = c.length - 1U;
            if (staged[0] == 0xfe && size >= 2) {
                selected_tuner = true;
                if (staged[1] == 0xce && size >= 4) {
                    selected_reg = static_cast<std::uint16_t>((staged[2] << 8U) | staged[3]);
                    if (size > 4) {
                        std::uint32_t v = 0;
                        for (unsigned i = 4; i < size; ++i)
                            v |= std::uint32_t(staged[i]) << (8 * (i - 4));
                        registers[{slave, true, selected_reg}] = v;
                    }
                } else if (staged[1] == 0xa8 && size >= 3) {
                    selected_reg = staged[2];
                    if (size == 4)
                        registers[{slave, true, selected_reg}] = staged[3];
                }
            } else {
                selected_tuner = false;
                selected_reg = staged[0];
                if (size == 2)
                    registers[{slave, false, selected_reg}] = staged[1];
            }
        } else if (c.request == asicen::Request::I2cReadNoWait) {
            const auto slave = static_cast<std::uint8_t>(c.value);
            auto value = registers[{slave, selected_tuner, selected_reg}];
            if (selected_tuner && (slave & 2U) && (selected_reg == 0x11 || selected_reg == 0x15)) {
                const bool ready = selected_reg == 0x11 ? (++por_reads >= por_ready_after)
                                                        : (++channel_reads >= channel_ready_after);
                value = (calibration_ready && ready) ? (value | 0x10U) : (value & ~0x10U);
            }
            if (!selected_tuner) {
                if (selected_reg == 0xb0)
                    value = locked ? 0xa8 : 0;
                if (selected_reg == 0x80)
                    value = locked ? 0 : 8;
                if (selected_reg == 0xc3)
                    value = locked ? 0 : 0x10;
            }
            for (unsigned i = 1; i < c.length; ++i)
                data[i] = static_cast<std::uint8_t>(value >> (8 * (i - 1)));
        }
        return c.length;
    }
    void delay_ms(unsigned ms) override
    {
        delays.push_back(ms);
    }
    bool cancelled() const override
    {
        return cancel;
    }
    bool expired() const override
    {
        return expire;
    }
};
} // namespace
bool test_all()
{
    using namespace asicen;
    CHECK(check(v2_demod_slave({0}) == 0x20 && v2_demod_slave({1}) == 0x22 &&
                    v2_demod_slave({2}) == 0x24 && v2_demod_slave({3}) == 0x26,
                "V2 demod addresses"));
    CHECK(check(v2_demod_slave({7}) == 0x2e && v2_demod_slave({8}) == 0, "V2 source bounds"));
    CHECK(check(v2_internal_source_from_api(0) == 1 && v2_internal_source_from_api(1) == 0 &&
                    v2_internal_source_from_api(2) == 3,
                "V2 API source swap"));
    std::array<std::uint8_t, 58> info{};
    info[0] = 1;
    info[3] = 6;
    info[4] = 0xb;
    info[5] = 6;
    for (unsigned field = 0; field < 4; ++field) {
        info[57] = static_cast<std::uint8_t>(field << 6U);
        const unsigned roles[] = {0, 2, 1, 3};
        const auto r = v2_source_route(info.data(), info.size(), 0);
        CHECK(check(r.valid && r.device_role == roles[field] &&
                        r.rf_master_role == (roles[field] & 2U) &&
                        r.target.internal_source == (2 * roles[field] + 1),
                    "wire support_feature role mapping"));
    }
    CHECK(check(!v2_source_route(info.data(), 57, 0).valid, "short customer-info rejected"));
    info[0] = 0;
    CHECK(check(!v2_source_route(info.data(), 58, 0).valid, "invalid customer-info rejected"));
    info[0] = 1;
    info[5] = 5;
    CHECK(check(!v2_source_route(info.data(), 58, 0).valid, "wrong model customer-info rejected"));
    CHECK(check(v2_tune_frequency_hz({0}, 557142) == 557143000U, "V2 terrestrial center"));
    CHECK(check(v2_tune_frequency_hz({0}, 165000) == 167143000U, "V2 165 MHz correction"));
    CHECK(check(v2_tune_frequency_hz({0}, 197143) == 195143000U, "V2 197 MHz correction"));
    CHECK(check(v2_tune_frequency_hz({0}, 471143) == 473143000U, "V2 471 MHz correction"));
    CHECK(check(v2_tune_frequency_hz({1}, 11727480) == 1049480000U, "V2 satellite RF to IF Hz"));
    CHECK(check(v2_tune_frequency_hz({1}, 1049480) == 0 &&
                    v2_tune_frequency_hz({0}, 0xffffffffU) == 0,
                "frequency units and overflow fail closed"));
    // Literal scalar fixtures derived from official 27 MHz code path at
    // 1c730/1d7e0/1d968, with reg21.bit6=1 and reg1b.bit0=0.
    struct Fixture {
        std::uint32_t khz;
        std::uint8_t div, integer;
        std::uint16_t fraction;
    };
    const Fixture fixtures[] = {{1049480, 8, 27, 0x7a80}, {1087840, 7, 13, 0x042c},
                                {1202920, 7, 27, 0xef1f}, {1318000, 6, 18, 0x71c6},
                                {1608400, 5, 20, 0xed09}, {2053000, 4, 24, 0x12f6}};
    for (auto f : fixtures) {
        const auto p = compute_v2_satellite_pll(f.khz);
        CHECK(check(p.valid && p.lo_divider == f.div && p.integer == f.integer &&
                        p.fraction == f.fraction && p.reference_ratio == 0 && p.predivider == 0,
                    "official V2 PLL scalar fixture"));
    }
    const auto fractional_boundary = compute_v2_satellite_pll(1720004, true, false);
    CHECK(check(fractional_boundary.valid && fractional_boundary.lo_divider == 4 &&
                    fractional_boundary.reference_ratio == 2 &&
                    fractional_boundary.predivider == 1 && fractional_boundary.integer == 0x3f &&
                    fractional_boundary.fraction == 0x1c8e,
                "official floor-half plus floor-quarter PLL scaling"));
    const auto half_mode = compute_v2_satellite_pll(1433065, false, false);
    CHECK(check(half_mode.valid && half_mode.lo_divider == 5 && half_mode.reference_ratio == 2 &&
                    half_mode.predivider == 1 && half_mode.integer == 0x46 &&
                    half_mode.fraction == 0x84b5,
                "official half-mode fractional fixture"));
    CHECK(check(!compute_v2_satellite_pll(0).valid && !compute_v2_satellite_pll(0xffffffffU).valid,
                "PLL invalid bounds"));
    CHECK(check(compute_v2_satellite_pll(1075000).lo_divider == 8 &&
                    compute_v2_satellite_pll(1075001).lo_divider == 7,
                "PLL band boundary"));
    // Literal control-transfer fixtures: NMI FE CE 01 64 00 08 00 00.
    const auto nw = v2_tuner_write_plan({0}, 0x164, 0x800, 4);
    CHECK(check(nw.size() == 4 && nw[0].request == Request::I2cBufferFill &&
                    nw[0].value == 0xfe00 && nw[0].index == 0x01ce && nw[0].length == 4,
                "NMI staging first chunk"));
    CHECK(check(nw[1].value == 0x6403 && nw[1].index == 0x0800 && nw[2].value == 6 &&
                    nw[2].index == 0,
                "NMI LE32 payload"));
    CHECK(check(nw.back().request == Request::I2cBufferSend && nw.back().value == 0x0020 &&
                    nw.back().length == 9,
                "NMI mode2 send"));
    const auto nr = v2_tuner_read_plan({0}, 0x3fc, 4);
    CHECK(check(nr.size() == 6 && nr[0].value == 0xfe00 && nr[0].index == 0x03ce &&
                    nr[1].value == 0xfc03,
                "NMI chip ID address"));
    CHECK(check(nr[nr.size() - 2].value == 0x0120 && nr.back().request == Request::I2cReadNoWait &&
                    nr.back().value == 0x20 && nr.back().length == 5,
                "NMI repeated-start/no-wait read"));
    const auto sw = v2_tuner_write_plan({1}, 0x1e, 0x1b, 1);
    CHECK(check(sw.size() == 3 && sw[0].value == 0xfe00 && sw[0].index == 0x1ea8 &&
                    sw[1].value == 0x1b03 && sw[2].value == 0x22,
                "TDA byte bridge wire"));
    CHECK(check(v2_tuner_write_plan({1}, 0x1e, 0x100, 1).empty() &&
                    v2_tuner_read_plan({1}, 0x100, 1).empty(),
                "TDA width bounds"));
    CHECK(check(plan_v2_demod_init({0}).size() == 28 && plan_v2_demod_init({1}).size() == 18,
                "model-specific demod reset plans"));
    CHECK(check(plan_v2_select_tsid({0}, 0x4010).empty() &&
                    plan_v2_select_tsid({1}, 0x4010).size() == 4,
                "TSID satellite-only"));
    CHECK(check(plan_v2_tsids_read({1}).size() == 48, "eight big-endian TSIDs"));
    const auto power = plan_v2_shared_power_on();
    bool resets_shared = false, lnb = false;
    for (auto op : power)
        if (op.kind == FrontendOpKind::Control && op.transfer.request == Request::Gpio) {
            const auto mask = op.transfer.value >> 8U;
            const auto value = op.transfer.value & 0xffU;
            CHECK(check(!op.require_status, "GPIO response is pin state, not ACK"));
            resets_shared |= (mask & 0x40U) != 0;
            lnb |= (mask & 0x20U) != 0 && (value & 0x20U) == 0;
        }
    CHECK(check(resets_shared && !lnb, "power plan includes shared reset and no LNB enable"));
    bool lock = false;
    RegisterTransport t;
    V2FrontendReport report;
    CHECK(
        check(read_v2_frontend_lock(&t, {0}, &lock, &report) == V2FrontendResult::Completed && lock,
              "terrestrial lock predicate"));
    t.locked = false;
    CHECK(check(read_v2_frontend_lock(&t, {0}, &lock) == V2FrontendResult::Completed && !lock,
                "terrestrial unlock predicate"));
    t.locked = true;
    CHECK(check(read_v2_frontend_lock(&t, {1}, &lock) == V2FrontendResult::Completed && lock,
                "satellite lock predicate"));
    t.registers[{0x22, false, 0xeb}] = 0;
    t.registers[{0x22, false, 0xec}] = 0x30;
    t.registers[{0x22, false, 0xed}] = 0xd5;
    CHECK(check(read_v2_frontend_lock(&t, {1}, &lock) == V2FrontendResult::Completed && !lock,
                "satellite BER floor"));
    RegisterTransport sat;
    CHECK(check(initialize_v2_frontend(&sat, {1}, &report) == V2FrontendResult::Completed,
                "full TDA RF initialization"));
    RegisterTransport tune;
    CHECK(check(tune_v2_frontend(&tune, {1}, 11727480, &report) == V2FrontendResult::Completed &&
                    report.locked,
                "full TDA tune and lock"));
    CHECK(check(tune.registers[{0x22, true, 0x1e}] == 0x1b &&
                    tune.registers[{0x22, true, 0x1f}] == 0x7a &&
                    tune.registers[{0x22, true, 0x20}] == 0x80,
                "TDA PLL output big endian registers"));
    CHECK(check(tune.calls.front().value == 0x0a00 && tune.calls.front().index == 0,
                "satellite demod prepare precedes tuner access"));
    CHECK(check(std::count(sat.delays.begin(), sat.delays.end(), 250U) == 0,
                "initial default tune does not reacquire demod"));
    for (unsigned index = 0; index < 4; ++index) {
        RegisterTransport gains;
        const auto slave = static_cast<std::uint8_t>(0x22 + 4 * index);
        gains.registers[{slave, true, 6}] = 0x48;
        gains.registers[{slave, true, 7}] = 0xff;
        CHECK(check(tune_v2_frontend(&gains, {static_cast<std::uint8_t>(1 + 2 * index)},
                                     11727480) == V2FrontendResult::Completed,
                    "all satellite source profiles tune"));
        const unsigned g[] = {0xaa, 0xbb, 0xbb, 0xbf}, a[] = {0x30, 0x30, 0x60, 0x30};
        CHECK(check(gains.registers[{slave, true, 6}] == g[index] &&
                        gains.registers[{slave, true, 7}] == 0xae &&
                        (gains.registers[{slave, true, 0xc}] & 0xf0U) == a[index],
                    "official per-source gain fixtures"));
    }
    RegisterTransport late_por;
    late_por.por_ready_after = 3;
    CHECK(check(initialize_v2_frontend(&late_por, {1}) == V2FrontendResult::Completed &&
                    late_por.por_reads == 3,
                "POR succeeds on third bounded poll"));
    RegisterTransport late_channel;
    late_channel.channel_ready_after = 2;
    CHECK(check(tune_v2_frontend(&late_channel, {1}, 11727480) == V2FrontendResult::Completed &&
                    late_channel.channel_reads == 2,
                "channel succeeds on second bounded poll"));
    RegisterTransport sid;
    std::array<std::uint16_t, 8> ids{};
    for (unsigned i = 0; i < 8; ++i) {
        sid.registers[{0x22, false, static_cast<std::uint16_t>(0xce + 2 * i)}] = 0x40;
        sid.registers[{0x22, false, static_cast<std::uint16_t>(0xcf + 2 * i)}] = 0x10 + i;
    }
    CHECK(check(read_v2_frontend_tsids(&sid, {1}, &ids) == V2FrontendResult::Completed &&
                    ids[0] == 0x4010 && ids[7] == 0x4017,
                "TSID executable big-endian read"));
    CHECK(check(select_v2_frontend_tsid(&sid, {1}, 0x4012) == V2FrontendResult::Completed &&
                    sid.registers[{0x22, false, 0x8f}] == 0x40 &&
                    sid.registers[{0x22, false, 0x90}] == 0x12,
                "TSID executable selector writes"));
    RegisterTransport partial_sid;
    partial_sid.fail_at = 4;
    ids.fill(0xffff);
    CHECK(
        check(read_v2_frontend_tsids(&partial_sid, {1}, &ids) == V2FrontendResult::FailedTransfer &&
                  std::all_of(ids.begin(), ids.end(), [](auto x) { return x == 0; }),
              "partial TSID result not exposed"));
    for (auto chip : {0x12000U, 0x13000U, 0x13100U, 0x813000U}) {
        RegisterTransport nmi;
        nmi.registers[{0x20, true, 0x3fc}] = chip;
        CHECK(check(initialize_v2_frontend(&nmi, {0}, &report) == V2FrontendResult::Completed &&
                        report.chip_id == chip,
                    "four-family NMI wire initialization"));
        CHECK(check(tune_v2_frontend(&nmi, {0}, 557142, &report) == V2FrontendResult::Completed &&
                        report.locked,
                    "four-family NMI full frontend tune"));
    }
    RegisterTransport unready;
    unready.calibration_ready = false;
    CHECK(check(initialize_v2_frontend(&unready, {1}) == V2FrontendResult::NotLocked,
                "calibration failure is not success"));
    RegisterTransport no_lock;
    no_lock.locked = false;
    CHECK(check(tune_v2_frontend(&no_lock, {1}, 11727480) == V2FrontendResult::NotLocked,
                "finite RF lock polling"));
    CHECK(check(std::count(no_lock.delays.begin(), no_lock.delays.end(), 50U) == 22,
                "22 bounded lock polls"));
    RegisterTransport cancel;
    cancel.cancel = true;
    CHECK(check(initialize_v2_frontend(&cancel, {1}) == V2FrontendResult::Cancelled &&
                    cancel.calls.empty(),
                "cancellation before IO"));
    RegisterTransport expired;
    expired.expire = true;
    CHECK(check(tune_v2_frontend(&expired, {1}, 11727480) == V2FrontendResult::DeadlineExceeded &&
                    expired.calls.empty(),
                "deadline before IO"));
    RegisterTransport short_io;
    short_io.short_at = 0;
    CHECK(check(initialize_v2_frontend(&short_io, {1}) == V2FrontendResult::ShortTransfer &&
                    short_io.calls.size() == 1,
                "short transfer stops sequence"));
    RegisterTransport bad_ack;
    bad_ack.bad_status_at = 0;
    CHECK(check(initialize_v2_frontend(&bad_ack, {1}) == V2FrontendResult::FailedTransfer &&
                    bad_ack.calls.size() == 1,
                "bad ACK stops sequence"));
    // Fail every transfer position in a complete TDA tune and ensure no
    // later transfer occurs, including read-modify-write and staged reads.
    for (std::size_t i = 0; i < tune.calls.size(); ++i) {
        RegisterTransport broken;
        broken.fail_at = static_cast<int>(i);
        CHECK(check(tune_v2_frontend(&broken, {1}, 11727480) == V2FrontendResult::FailedTransfer &&
                        broken.calls.size() == i + 1,
                    "TDA failure boundary is atomic"));
    }
    std::cout << "V2 frontend tests passed\n";
    return true;
}

int main()
{
    return test_all() ? 0 : 1;
}
