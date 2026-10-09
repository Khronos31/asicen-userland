// SPDX-License-Identifier: GPL-2.0-only
// NMI/TDA RF algorithms adapted from knight-rider/ptx, commit
// ad3dc2619787a9a38ae3c5a17137f47d9631e8e1 retrieved 2026-10-08, Copyright (C) Budi Rachmanto,
// AreMa Inc. <info@are.ma>.
// https://github.com/knight-rider/ptx/blob/ad3dc2619787a9a38ae3c5a17137f47d9631e8e1/drivers/media/tuners/nm131.c
// SHA256 fd35d5a07754627d8bea839c21cc041bcd389cbaf04f0be398faf6240a264adf
// https://github.com/knight-rider/ptx/blob/ad3dc2619787a9a38ae3c5a17137f47d9631e8e1/drivers/media/tuners/tda2014x.c
// SHA256 283d92324eab103d6071e7a1937ac7f1a8ce2ebac759833eb21b7a936d25a3af
// Both sources declare MODULE_LICENSE("GPL"). That string is metadata, not a
// GPL version or a separate license document; none was found for these two
// drivers. This derived file is distributed as GPL-2.0-only with the product.
// Preserve the attribution above.
//
// V2-specific facts independently checked against official x64 BDA SYS
// SHA256 5c7174d62eef7d704f44904ac1336261135a1edfccd2776e5e1c45ce7088f0f2.
// File offsets equal RVAs; image VA = RVA + 0x10000:
// demod init 165b4; prepare/acquire 169dc/16b70; demod read 17464;
// NMI chip guard 1b650, RF tables 28eb8/28ef8/28f10/28d84;
// NMI bridge 1c448/1c4f4; TDA bridge 1f038/1f09c;
// TDA init 1cd34, PLL 1c730/1d7e0/1d968; source map 1f5f0;
// RF unit/center conversion 1f69c/1f334; lock 1f490; GPIO power 1fca4.
// No vendor executable code, firmware, card payload or key material included.

#include "asicen/v2_frontend.h"
#include "asicen/v2_nmi.h"
#include "asicen/write_protocol.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <utility>

namespace asicen {
bool v2_target_valid(V2FrontendTarget t) {
    return t.internal_source < 8;
}
bool v2_target_is_satellite(V2FrontendTarget t) {
    return v2_target_valid(t) && (t.internal_source & 1U) != 0;
}
std::uint8_t v2_demod_slave(V2FrontendTarget t) {
    return v2_target_valid(t) ? static_cast<std::uint8_t>(0x20 + 2 * t.internal_source) : 0;
}
std::uint8_t v2_internal_source_from_api(std::uint8_t source) {
    return source < 8 ? static_cast<std::uint8_t>(source ^ 1U) : 0xff;
}
V2SourceRoute v2_source_route(const std::uint8_t *data, std::size_t size, std::uint8_t local) {
    V2SourceRoute r{};
    if (!data || size != kCustomerInfoSize || local > 1 || data[0] != 1)
        return r;
    // Customer-info stores VID/PID little endian at bytes3..6.
    if (data[3] != 0x06 || data[4] != 0x0b || data[5] != 0x06 || data[6] != 0)
        return r;
    const std::uint8_t roles[] = {0, 2, 1, 3};
    r.device_role = roles[data[57] >> 6U];
    r.rf_master_role = static_cast<std::uint8_t>(r.device_role & 2U);
    r.target.internal_source =
        v2_internal_source_from_api(static_cast<std::uint8_t>(2 * r.device_role + local));
    r.valid = true;
    return r;
}
std::uint32_t v2_tune_frequency_hz(V2FrontendTarget t, std::uint32_t f) {
    if (!v2_target_valid(t))
        return 0;
    if (v2_target_is_satellite(t)) {
        // Bound the Japanese RF band before subtracting LO or multiplying.
        if (f < 11628000U || f > 12828000U)
            return 0;
        return (f - 10678000U) * 1000U;
    }
    if (f < 90000U || f > 770000U)
        return 0;
    if (f >= 165000U && f <= 167143U)
        f = 167143U;
    else if (f >= 195143U && f <= 197143U)
        f = 195143U;
    else if (f >= 471143U && f <= 473143U)
        f = 473143U;
    return (f / 1000U) * 1000000U + 143000U;
}
V2Pll compute_v2_satellite_pll(std::uint32_t f, bool bit6, bool bit0) {
    V2Pll out{};
    if (f < 950000U || f > 2150000U)
        return out;
    const unsigned div = f <= 1075000U   ? 8U
                         : f <= 1228000U ? 7U
                         : f <= 1433000U ? 6U
                         : f <= 1720000U ? 5U
                                         : 4U;
    // Official 27 MHz path (1d65c/1d6dc): exact wide product and division.
    std::uint64_t k = static_cast<std::uint64_t>(div) * f * 1000U / 27U;
    const std::uint64_t n10 = (k + 50000U) / 100000U;
    const unsigned low = bit0 ? (bit6 ? 1290U : 1305U) : (bit6 ? 1310U : 1345U);
    const unsigned high = bit0 ? (bit6 ? 2530U : 2525U) : (bit6 ? 2510U : 2485U);
    unsigned r = 1, pre = 2;
    std::uint64_t n = n10 / 2U;
    if (n > high) {
        pre = 4;
        n = n10 / 4U;
        if (n > high) {
            r = 3;
            n = 3U * n10 / 4U;
        }
    } else if (n < low) {
        r = 2;
        n = n10;
        if (n > high) {
            r = 3;
            pre = 4;
            n = 3U * n10 / 4U;
        } else if (n < low) {
            r = 3;
            n = 3U * n10 / 2U;
        }
    }
    if (n < low || n > high)
        return out;
    // The binary rounds the scaled divisor to nearest ten (not the GPL
    // reference's truncate-to-ten shortcut).
    const auto scaled = (r == 3 && pre == 4) ? k / 2U + k / 4U : k * r / pre;
    k = ((scaled + 5U) / 10U) * 10U;
    std::uint64_t integer = 0, frac = 0;
    if (bit6) {
        integer = k / 1000000U;
        frac = k % 1000000U;
    } else {
        const auto half = k / 2U;
        auto base = half / 1000000U;
        frac = ((half % 1000000U + 5U) / 10U) * 10U;
        unsigned odd = 0;
        if (frac > 750000U) {
            frac -= 500000U;
            odd = 1;
        } else if (frac < 250000U) {
            if (base == 0)
                return out;
            --base;
            frac += 500000U;
            odd = 1;
        }
        integer = base * 2U + odd;
    }
    if (integer < 128U || integer > 383U)
        return out;
    std::uint64_t scale = 1000000U;
    for (unsigned i = 0; i < 16; ++i) {
        frac *= 2U;
        if (frac > 0x0fffffffU && i != 15U) {
            frac /= 10U;
            scale /= 10U;
        }
    }
    frac /= scale;
    if (frac > 0xffffU)
        return out;
    out.valid = true;
    out.lo_divider = static_cast<std::uint8_t>(div);
    out.reference_ratio = static_cast<std::uint8_t>(r - 1U);
    out.predivider = static_cast<std::uint8_t>(pre == 2 ? 0 : 1);
    out.integer = static_cast<std::uint8_t>(integer - 128U);
    out.fraction = static_cast<std::uint16_t>(frac);
    return out;
}
namespace {
using Bytes = std::vector<std::uint8_t>;
std::vector<ControlTransfer> staged(V2FrontendTarget t, const Bytes &bytes, bool stop = true) {
    if (!v2_target_valid(t) || bytes.empty())
        return {};
    return build_i2c_write_sequence(v2_demod_slave(t), 0, bytes.data(), bytes.size(), stop ? 2 : 3);
}
void add_control(FrontendPlan &plan, const ControlTransfer &c, const char *label) {
    FrontendOp op{};
    op.transfer = c;
    op.require_status = (c.request != Request::Gpio && c.request != Request::GpioExSet);
    op.label = label;
    plan.push_back(op);
}
void add_delay(FrontendPlan &plan, unsigned ms) {
    FrontendOp op{};
    op.kind = FrontendOpKind::Delay;
    op.delay_ms = ms;
    op.label = "V2 delay";
    plan.push_back(op);
}
void append_demod(FrontendPlan &plan, V2FrontendTarget t, std::uint8_t reg, std::uint8_t val) {
    for (auto c : staged(t, {reg, val}))
        add_control(plan, c, "V2 demod write");
}
} // namespace
std::vector<ControlTransfer> v2_tuner_write_plan(V2FrontendTarget t, std::uint16_t reg,
                                                 std::uint32_t value, std::uint8_t width) {
    if (!v2_target_valid(t))
        return {};
    if (v2_target_is_satellite(t)) {
        if (reg > 0xff || width != 1 || value > 0xff)
            return {};
        return staged(
            t, {0xfe, 0xa8, static_cast<std::uint8_t>(reg), static_cast<std::uint8_t>(value)});
    }
    if (width != 1 && width != 4)
        return {};
    if (width == 1 && value > 0xff)
        return {};
    Bytes b{0xfe, 0xce, static_cast<std::uint8_t>(reg >> 8), static_cast<std::uint8_t>(reg)};
    for (unsigned i = 0; i < width; ++i)
        b.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    return staged(t, b);
}
std::vector<ControlTransfer> v2_tuner_read_plan(V2FrontendTarget t, std::uint16_t reg,
                                                std::uint8_t width) {
    if (!v2_target_valid(t))
        return {};
    Bytes arm, trigger;
    if (v2_target_is_satellite(t)) {
        if (reg > 0xff || width != 1)
            return {};
        arm = {0xfe, 0xa8, static_cast<std::uint8_t>(reg)};
        trigger = {0xfe, 0xa9};
    } else {
        if (width != 1 && width != 4)
            return {};
        arm = {0xfe, 0xce, static_cast<std::uint8_t>(reg >> 8), static_cast<std::uint8_t>(reg)};
        trigger = {0xfe, 0xcf};
    }
    auto out = staged(t, arm);
    auto next = staged(t, trigger, false);
    out.insert(out.end(), next.begin(), next.end());
    out.push_back(make_i2c_read_no_wait(v2_demod_slave(t), width));
    return out;
}
FrontendPlan plan_v2_demod_init(V2FrontendTarget t) {
    FrontendPlan out;
    if (!v2_target_valid(t))
        return out;
    if (v2_target_is_satellite(t)) {
        for (auto r : {0x13, 0x15, 0x17, 0x1c, 0x1d, 0x1f})
            append_demod(out, t, r, 0);
        append_demod(out, t, 7, 0x31);
        append_demod(out, t, 8, 0x77);
        append_demod(out, t, 4, 2);
    } else {
        const std::pair<std::uint8_t, std::uint8_t> regs[] = {
            {0xb0, 0xa0}, {0xb2, 0x3d}, {0xb3, 0x25}, {0xb4, 0x8b}, {0xb5, 0x4b},
            {0xb6, 0x3f}, {0xb7, 0xff}, {0xb8, 0xc0}, {3, 0},       {0x1d, 0},
            {0x1f, 0},    {0xe, 0x77},  {0xf, 0x13},  {0x75, 2}};
        for (auto rv : regs)
            append_demod(out, t, rv.first, rv.second);
    }
    return out;
}
FrontendPlan plan_v2_select_tsid(V2FrontendTarget t, std::uint16_t tsid) {
    FrontendPlan p;
    if (!v2_target_is_satellite(t))
        return p;
    append_demod(p, t, 0x8f, static_cast<std::uint8_t>(tsid >> 8));
    append_demod(p, t, 0x90, static_cast<std::uint8_t>(tsid));
    return p;
}
FrontendPlan plan_v2_tsids_read(V2FrontendTarget t) {
    FrontendPlan p;
    if (!v2_target_is_satellite(t))
        return p;
    for (unsigned reg = 0xce; reg < 0xde; ++reg) {
        for (auto c : staged(t, {static_cast<std::uint8_t>(reg)}, false))
            add_control(p, c, "V2 TSID address");
        add_control(p, make_i2c_read_no_wait(v2_demod_slave(t), 1), "V2 TSID byte");
    }
    return p;
}
FrontendPlan plan_v2_shared_power_on() {
    FrontendPlan p;
    auto gpio = [&](std::uint8_t v, std::uint8_t m, unsigned ms) {
        add_control(p, make_gpio_set(v, m), "V2 shared GPIO");
        add_delay(p, ms);
    };
    gpio(8, 8, 50);
    gpio(0, 8, 50);
    gpio(4, 4, 10);
    gpio(0, 5, 10);
    gpio(4, 4, 10);
    gpio(0x10, 0x10, 10);
    gpio(0, 0x10, 10);
    gpio(0x10, 0x10, 10);
    add_control(p, make_i2c_read(0xa8, 0, 1, 0), "V2 power controller read");
    add_delay(p, 100);
    gpio(0x40, 0x40, 10);
    gpio(0, 0x40, 10);
    add_delay(p, 100);
    return p;
}
FrontendPlan plan_v2_shared_power_off() {
    // TC power(local/API0,power0), official1fdbd..1fe1c. Do not restore
    // unrelated snapshot bits or infer GPIO20's electrical function.
    FrontendPlan p;
    add_control(p, make_gpio_set(0x40, 0x40), "V2 shared power off");
    add_delay(p, 10);
    add_control(p, make_gpio_set(8, 8), "V2 standby off");
    add_delay(p, 100);
    return p;
}
FrontendPlan plan_v2_revision11_startup_prefix() {
    FrontendPlan p;
    auto gpio = [&](std::uint8_t v, std::uint8_t m, unsigned ms) {
        add_control(p, make_gpio_set(v, m), "V2 startup GPIO");
        add_delay(p, ms);
    };
    gpio(0, 0x40, 50);
    gpio(0, 8, 50);
    gpio(0x10, 0x10, 10);
    gpio(0, 0x10, 10);
    gpio(0x10, 0x10, 0);
    gpio(4, 4, 10);
    gpio(0, 4, 10);
    gpio(4, 4, 10);
    // Caller now reads a8:b0 one byte, waits10ms, then privately reads16.
    return p;
}
FrontendPlan plan_v2_revision11_startup_tail() {
    FrontendPlan p;
    add_control(p, make_gpio_set(0xa7, 0xfb), "V2 startup default GPIO");
    add_control(p, make_gpio_set(0x40, 0x40), "V2 shared standby");
    add_control(p, make_gpio_set(0x40, 0x40), "V2 shared standby repeat");
    add_delay(p, 10);
    add_control(p, make_gpio_set(8, 8), "V2 standby");
    add_delay(p, 100);
    return p;
}
namespace {
class Engine {
  public:
    FrontendTransport *transport;
    V2FrontendTarget target;
    V2FrontendReport *report;
    V2FrontendResult status = V2FrontendResult::Completed;
    std::uint32_t nmi_chip_id = 0;
    std::chrono::steady_clock::time_point deadline;
    Engine(FrontendTransport *t, V2FrontendTarget s, V2FrontendReport *r, unsigned ms)
        : transport(t), target(s), report(r),
          deadline(std::chrono::steady_clock::now() + std::chrono::milliseconds(ms)) {}
    bool ok() {
        if (status != V2FrontendResult::Completed)
            return false;
        if (transport->cancelled())
            status = V2FrontendResult::Cancelled;
        else if (transport->expired() || std::chrono::steady_clock::now() >= deadline)
            status = V2FrontendResult::DeadlineExceeded;
        return status == V2FrontendResult::Completed;
    }
    bool delay(unsigned ms) {
        if (!ok())
            return false;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - std::chrono::steady_clock::now())
                              .count();
        if (left < static_cast<long long>(ms)) {
            status = V2FrontendResult::DeadlineExceeded;
            return false;
        }
        transport->delay_ms(ms);
        return ok();
    }
    bool control(ControlTransfer c, Bytes *read = nullptr) {
        if (!ok())
            return false;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - std::chrono::steady_clock::now())
                              .count();
        c.timeout_ms = static_cast<std::uint16_t>(
            std::max<long long>(1, std::min<long long>(c.timeout_ms, left)));
        Bytes b(c.length);
        const int rc = transport->control(c, b.data());
        if (rc < 0) {
            status = V2FrontendResult::FailedTransfer;
            return false;
        }
        if (rc != c.length) {
            status = V2FrontendResult::ShortTransfer;
            return false;
        }
        if (b.empty() || b[0] != 1) {
            status = V2FrontendResult::FailedTransfer;
            return false;
        }
        if (report)
            ++report->transfers_completed;
        if (read)
            read->assign(b.begin() + 1, b.end());
        return ok();
    }
    bool sequence(const std::vector<ControlTransfer> &plan, Bytes *read = nullptr) {
        if (plan.empty()) {
            status = V2FrontendResult::InvalidArgument;
            return false;
        }
        for (std::size_t i = 0; i < plan.size(); ++i)
            if (!control(plan[i], i + 1 == plan.size() ? read : nullptr))
                return false;
        return true;
    }
    bool dw(std::uint8_t reg, std::uint8_t value) {
        return sequence(staged(target, {reg, value}));
    }
    bool dr(std::uint8_t reg, std::uint8_t &value) {
        auto p = staged(target, {reg}, false);
        p.push_back(make_i2c_read_no_wait(v2_demod_slave(target), 1));
        Bytes b;
        if (!sequence(p, &b))
            return false;
        value = b[0];
        return true;
    }
    bool wr(std::uint16_t reg, std::uint32_t value, std::uint8_t width = 1) {
        return sequence(v2_tuner_write_plan(target, reg, value, width));
    }
    bool rd(std::uint16_t reg, std::uint32_t &value, std::uint8_t width = 1) {
        Bytes b;
        if (!sequence(v2_tuner_read_plan(target, reg, width), &b))
            return false;
        value = 0;
        for (unsigned i = 0; i < width; ++i)
            value |= std::uint32_t(b[i]) << (8 * i);
        return true;
    }
    bool mask(std::uint16_t reg, std::uint8_t andmask, std::uint8_t ormask) {
        std::uint32_t v = 0;
        return rd(reg, v) && wr(reg, (v & andmask) | ormask);
    }
    bool tr8(std::uint8_t reg, std::uint8_t start, std::uint8_t bits, std::uint8_t *value) {
        std::uint32_t v = 0;
        if (!rd(reg, v))
            return false;
        *value = static_cast<std::uint8_t>((v >> start) & ((1U << bits) - 1));
        return true;
    }
    bool tw16(std::uint8_t reg, std::uint8_t start, std::uint8_t bits, std::uint8_t bytes, bool rmw,
              std::uint16_t value) {
        if (!bytes)
            bytes = 1;
        if (bytes > 2 || !bits || bits > 16 || start + bits > 16) {
            status = V2FrontendResult::InvalidArgument;
            return false;
        }
        const std::uint32_t bitmask = ((1U << bits) - 1U) << start;
        const std::uint32_t val = (std::uint32_t(value) << start) & bitmask;
        for (unsigned i = 0; i < bytes; ++i) {
            const unsigned shift = 8 * (bytes - 1 - i);
            std::uint32_t v = (val >> shift) & 0xff;
            // Official1eb9c reads only for an explicit read-modify-write.
            if (rmw) {
                std::uint32_t old = 0;
                if (!rd(reg + i, old))
                    return false;
                v |= old & ~(bitmask >> shift) & 0xff;
            }
            if (!wr(reg + i, v))
                return false;
        }
        return true;
    }
    bool demod_init() {
        for (const auto &op : plan_v2_demod_init(target)) {
            if (!control(op.transfer))
                return false;
        }
        return ok();
    }
    bool nmi_init();
    bool nmi_tune(std::uint32_t hz);
    bool tda_init();
    bool tda_tune(std::uint32_t khz, bool prepare = true);
    bool lock(bool &locked);
};

class NmiAdapter final : public V2NmiIo {
  public:
    Engine &e;
    explicit NmiAdapter(Engine &engine) : e(engine) {}
    bool read(std::uint16_t reg, std::uint32_t *value, std::uint8_t width) override {
        return value && e.rd(reg, *value, width);
    }
    bool write(std::uint16_t reg, std::uint32_t value, std::uint8_t width) override {
        return e.wr(reg, value, width);
    }
    bool demod_write(std::uint8_t reg, std::uint8_t value) override {
        return e.dw(reg, value);
    }
    bool delay_ms(unsigned ms) override {
        return e.delay(ms);
    }
    bool healthy() const override {
        return e.status == V2FrontendResult::Completed;
    }
};
bool nmi_result(Engine &e, V2NmiResult r) {
    if (r == V2NmiResult::Completed)
        return e.ok();
    if (e.status != V2FrontendResult::Completed)
        return false;
    if (r == V2NmiResult::UnsupportedChip)
        e.status = V2FrontendResult::UnsupportedChip;
    else if (r == V2NmiResult::InvalidArgument)
        e.status = V2FrontendResult::InvalidArgument;
    else if (r == V2NmiResult::CalibrationTimeout)
        e.status = V2FrontendResult::NotLocked;
    else
        e.status = V2FrontendResult::FailedTransfer;
    return false;
}
bool Engine::nmi_init() {
    NmiAdapter io(*this);
    const auto r = initialize_v2_nmi(io, &nmi_chip_id);
    if (report && nmi_chip_id) {
        report->chip_id = nmi_chip_id;
        report->chip_id_valid = true;
    }
    return nmi_result(*this, r);
}
bool Engine::nmi_tune(std::uint32_t hz) {
    NmiAdapter io(*this);
    return nmi_result(*this, tune_v2_nmi(io, hz, nmi_chip_id));
}
bool Engine::tda_init() {
    using u8 = std::uint8_t;
    u8 val = 0;
    void *c = nullptr;
    auto tda2014x_r8 = [&](void *, u8 r, u8 b, u8 n, u8 *v) { return tr8(r, b, n, v); };
    auto tda2014x_w16 = [&](void *, u8 r, u8 b, u8 n, u8 bytes, bool rmw, u8, std::uint16_t v) {
        return tw16(r, b, n, bytes, rmw, v);
    };
    return (/* SetPowerMode */
            tda2014x_r8(c, 2, 0, 8, &val) && tda2014x_w16(c, 2, 0, 8, 0, 0, 6, val | 0x81) &&
            tda2014x_r8(c, 6, 0, 8, &val) &&
            tda2014x_w16(c, 6, 0, 8, 0, 0, 6, (val | 0x39) & 0x7F) &&
            tda2014x_r8(c, 7, 0, 8, &val) && tda2014x_w16(c, 7, 0, 8, 0, 0, 6, val | 0xAE) &&
            tda2014x_r8(c, 0xF, 0, 8, &val) && tda2014x_w16(c, 0xF, 0, 8, 0, 0, 6, val | 0x80) &&
            tda2014x_r8(c, 0x18, 0, 8, &val) && tda2014x_w16(c, 0x18, 0, 8, 0, 0, 6, val & 0x7F) &&
            tda2014x_r8(c, 0x1A, 0, 8, &val) && tda2014x_w16(c, 0x1A, 0, 8, 0, 0, 6, val | 0xC0) &&
            tda2014x_w16(c, 0x22, 0, 8, 0, 0, 6, 0xFF) && tda2014x_r8(c, 0x23, 0, 8, &val) &&
            tda2014x_w16(c, 0x23, 0, 8, 0, 0, 6, val & 0xFE) && tda2014x_r8(c, 0x25, 0, 8, &val) &&
            tda2014x_w16(c, 0x25, 0, 8, 0, 0, 6, val | 8) && tda2014x_r8(c, 0x27, 0, 8, &val) &&
            tda2014x_w16(c, 0x27, 0, 8, 0, 0, 6, (val | 0xC0) & 0xDF) &&
            tda2014x_r8(c, 0x24, 0, 8, &val) &&
            tda2014x_w16(c, 0x24, 0, 8, 0, 0, 6, (val | 4) & 0xCF) &&
            tda2014x_r8(c, 0xD, 0, 8, &val) && tda2014x_w16(c, 0xD, 0, 8, 0, 0, 6, val & 0xDF) &&
            tda2014x_r8(c, 9, 0, 8, &val) &&
            tda2014x_w16(c, 9, 0, 8, 0, 0, 6, (val | 0xB0) & 0xB1) &&
            tda2014x_r8(c, 0xA, 0, 8, &val) &&
            tda2014x_w16(c, 0xA, 0, 8, 0, 0, 6, (val | 0x6F) & 0x7F) &&
            tda2014x_r8(c, 0xB, 0, 8, &val) &&
            tda2014x_w16(c, 0xB, 0, 8, 0, 0, 6, (val | 0x7A) & 0x7B) &&
            tda2014x_w16(c, 0xC, 0, 8, 0, 0, 6, 0) && tda2014x_w16(c, 0x19, 0, 8, 0, 0, 6, 0xFA) &&
            tda2014x_r8(c, 0x1B, 0, 8, &val) && tda2014x_w16(c, 0x1B, 0, 8, 0, 0, 6, val & 0x7F) &&
            tda2014x_r8(c, 0x21, 0, 8, &val) && tda2014x_w16(c, 0x21, 0, 8, 0, 0, 6, val | 0x40) &&
            tda2014x_r8(c, 0x10, 0, 8, &val) &&
            tda2014x_w16(c, 0x10, 0, 8, 0, 0, 6, (val | 0x90) & 0xBF) &&
            tda2014x_r8(c, 0x14, 0, 8, &val) &&
            tda2014x_w16(c, 0x14, 0, 8, 0, 0, 6, (val | 0x20) & 0xEF) &&

            /* ProgramPllPor */
            tda2014x_w16(c, 0x1A, 6, 1, 0, 1, 6, 1) && tda2014x_w16(c, 0x18, 0, 1, 0, 1, 6, 1) &&
            tda2014x_w16(c, 0x18, 7, 1, 0, 1, 6, 1) && tda2014x_w16(c, 0x1B, 7, 1, 0, 1, 6, 1) &&
            tda2014x_w16(c, 0x18, 0, 1, 0, 1, 6, 0) &&

            /* ProgramVcoPor */
            tda2014x_r8(c, 0xF, 0, 8, &val) &&
            (val = (val & 0x1F) | 0x80, tda2014x_w16(c, 0xF, 0, 8, 0, 0, 6, val)) &&
            tda2014x_r8(c, 0x13, 0, 8, &val) &&
            (val = (val & 0xFFFFFFCF) | 0x20, tda2014x_w16(c, 0x13, 0, 8, 0, 0, 6, val)) &&
            tda2014x_r8(c, 0x12, 0, 8, &val) &&
            (val |= 0xC0, tda2014x_w16(c, 0x12, 0, 8, 0, 0, 6, val)) &&
            tda2014x_w16(c, 0x10, 5, 1, 0, 1, 6, 1) && tda2014x_w16(c, 0x10, 5, 1, 0, 1, 6, 1) &&
            tda2014x_w16(c, 0xF, 5, 1, 0, 1, 6, 1) && tda2014x_r8(c, 0x11, 4, 1, &val) &&
            (val || tda2014x_r8(c, 0x11, 4, 1, &val)) &&
            (val || tda2014x_r8(c, 0x11, 4, 1, &val)) && val && tda2014x_r8(c, 0x10, 0, 4, &val) &&
            tda2014x_w16(c, 0xF, 0, 4, 0, 1, 6, val) && tda2014x_w16(c, 0xF, 6, 1, 0, 1, 6, 1) &&
            tda2014x_w16(c, 0xF, 5, 1, 0, 1, 6, 0) && tda2014x_r8(c, 0x12, 0, 8, &val) &&
            (val &= 0x7F, tda2014x_w16(c, 0x12, 0, 8, 0, 0, 6, val)) &&
            tda2014x_w16(c, 0xD, 5, 2, 0, 1, 6, 1) &&

            /* EnableLoopThrough */
            tda2014x_r8(c, 6, 0, 8, &val) && tda2014x_w16(c, 6, 0, 8, 0, 0, 6, (val & 0xF7) | 8)) &&
           tda_tune(1318000U, false);
}
bool Engine::tda_tune(std::uint32_t khz, bool prepare) {
    if (prepare && (!dw(0x0a, 0) || !dw(0x10, 0xb0) || !dw(0x11, 2) || !dw(3, 1)))
        return false;
    std::uint8_t mux = 0, bit6 = 0, bit0 = 0, val = 0;
    if (!tr8(0x25, 3, 1, &mux) || !tr8(0x21, 6, 1, &bit6) || !tr8(0x1b, 0, 1, &bit0))
        return false;
    const auto pll = compute_v2_satellite_pll(khz, bit6 != 0, bit0 != 0);
    if (!pll.valid) {
        status = V2FrontendResult::InvalidArgument;
        return false;
    }
    const auto div = pll.lo_divider;
    const unsigned lo22 = div == 8 ? 0x1e : 0xff;
    const unsigned lo23 =
        (div == 7 || div == 5 ? 0x80 : 0) | (div == 7 || div == 6 ? 0x20 : 0) | (div == 8 ? 4 : 8);
    if (!tw16(0x22, 0, 8, 0, false, lo22) || !tr8(0x23, 0, 8, &val) ||
        !tw16(0x23, 0, 8, 0, false, (val & 0x53U) | lo23) || !tw16(0x25, 3, 1, 0, true, mux))
        return false;
    if (!tw16(3, 6, 2, 0, true, pll.reference_ratio) ||
        !tw16(0x1a, 5, 1, 0, true, pll.predivider) || !tw16(0x1e, 0, 8, 0, false, pll.integer) ||
        !tw16(0x1f, 0, 16, 2, false, pll.fraction))
        return false;
    // ProgramVcoChannelChange, checked and bounded (official 1e6a4).
    if (!tr8(0x12, 0, 8, &val) || !tw16(0x12, 0, 8, 0, false, (val & 0x7fU) | 0x40U) ||
        !tw16(0x13, 0, 2, 0, true, 2) || !tw16(0x13, 7, 1, 0, true, 0) ||
        !tw16(0x13, 5, 1, 0, true, 1) || !tw16(0x13, 5, 1, 0, true, 1) ||
        !tw16(0x13, 7, 1, 0, true, 0) || !tw16(0x13, 4, 1, 0, true, 1))
        return false;
    bool ready = false;
    for (unsigned i = 0; i < 2; ++i) {
        if (!tr8(0x15, 4, 1, &val))
            return false;
        if (val) {
            ready = true;
            break;
        }
    }
    if (!ready) {
        status = V2FrontendResult::NotLocked;
        return false;
    }
    if (!tw16(0x13, 4, 1, 0, true, 0) || !tr8(0x12, 0, 8, &val) ||
        !tw16(0x12, 0, 8, 0, false, val & 0x7fU))
        return false;
    // 28,860 ksym/s, rolloff selector2 and 5,000 kHz offset produce the
    // official 36 MHz bandwidth; 1cacc writes 0a[3:0]=a,0b[7:1]=7c.
    if (!tw16(0xa, 0, 4, 0, true, 0xa) || !tw16(0xb, 1, 7, 0, true, 0x7c))
        return false;
    // Official index-specific defaults (1c550 -> 1cc78 -> 1db8c/1dd18).
    const unsigned chip_index = target.internal_source >> 1U;
    constexpr std::uint8_t gain[] = {0xa2, 0xb3, 0xb3, 0xb7};
    constexpr std::uint8_t amp[] = {3, 3, 6, 3};
    std::uint8_t reg7 = 0;
    if (!tr8(7, 0, 8, &reg7) || !tw16(6, 6, 1, 0, true, 0) ||
        !tw16(7, 0, 8, 0, false, (reg7 & 0xacU) | 2U) || !tr8(6, 0, 8, &val) ||
        !tw16(6, 0, 8, 0, false, (val & 0x48U) | gain[chip_index]) || !tr8(9, 0, 8, &val) ||
        !tw16(9, 0, 8, 0, false, (val & 3U) | 0xb0U) || !tw16(0xa, 5, 3, 0, true, 3) ||
        !tw16(0xc, 4, 4, 0, true, amp[chip_index]))
        return false;
    if (!prepare)
        return ok();
    if (!delay(250))
        return false;
    return dw(0xa, 0xff) && dw(0x10, 0xb2) && dw(0x11, 0) && dw(3, 1);
}
bool Engine::lock(bool &locked) {
    locked = false;
    std::uint8_t v = 0;
    if (v2_target_is_satellite(target)) {
        if (!dr(0xc3, v))
            return false;
        if (v & 0x10U)
            return true;
        // The official lock predicate applies a BER-quality floor too.
        // A zero counter is good quality. Transfer failures are never lock.
        if (!dr(0xc3, v))
            return false;
        if (v & 0x10U)
            return true;
        std::uint8_t hi = 0, mid = 0, lo = 0;
        if (!dr(0xeb, hi) || !dr(0xec, mid) || !dr(0xed, lo))
            return false;
        const std::uint32_t errors = (std::uint32_t(hi) << 16) | (std::uint32_t(mid) << 8) | lo;
        locked = errors <= 12500U;
    } else {
        if (!dr(0x80, v))
            return false;
        if (v & 8U)
            return true;
        if (!dr(0xb0, v))
            return false;
        locked = (v & 0xfU) >= 8U;
    }
    if (report)
        report->locked = locked;
    return true;
}
V2FrontendResult finish(Engine &e, bool success) {
    if (!success && e.status == V2FrontendResult::Completed)
        return V2FrontendResult::NotLocked;
    return e.status;
}
} // namespace
V2FrontendResult initialize_v2_frontend(FrontendTransport *transport, V2FrontendTarget target,
                                        V2FrontendReport *report, unsigned budget_ms) {
    if (report)
        *report = {};
    if (!transport || !v2_target_valid(target) || budget_ms == 0)
        return V2FrontendResult::InvalidArgument;
    Engine e(transport, target, report, budget_ms);
    const bool ok =
        e.demod_init() && (v2_target_is_satellite(target) ? e.tda_init() : e.nmi_init());
    return finish(e, ok);
}
V2FrontendResult tune_v2_frontend(FrontendTransport *transport, V2FrontendTarget target,
                                  std::uint32_t rf_khz, V2FrontendReport *report,
                                  unsigned budget_ms) {
    if (report)
        *report = {};
    const auto hz = v2_tune_frequency_hz(target, rf_khz);
    if (!transport || !hz || !budget_ms)
        return V2FrontendResult::InvalidArgument;
    Engine e(transport, target, report, budget_ms);
    // A fresh ID read is required before family-specific arithmetic even if
    // this operation is called without initialize_v2_frontend first.
    if (!v2_target_is_satellite(target)) {
        std::uint32_t chip = 0;
        if (!e.rd(0x3fc, chip, 4))
            return e.status;
        if (report) {
            report->chip_id = chip;
            report->chip_id_valid = true;
        }
        e.nmi_chip_id = chip;
    }
    const bool tuned = v2_target_is_satellite(target) ? e.tda_tune(hz / 1000U) : e.nmi_tune(hz);
    if (!tuned)
        return finish(e, false);
    for (unsigned i = 0; i < 22; ++i) {
        if (!e.delay(50))
            return e.status;
        bool locked = false;
        if (!e.lock(locked))
            return e.status;
        if (locked)
            return V2FrontendResult::Completed;
    }
    return V2FrontendResult::NotLocked;
}
V2FrontendResult read_v2_frontend_lock(FrontendTransport *transport, V2FrontendTarget target,
                                       bool *locked, V2FrontendReport *report, unsigned budget_ms) {
    if (report)
        *report = {};
    if (locked)
        *locked = false;
    if (!transport || !locked || !v2_target_valid(target) || !budget_ms)
        return V2FrontendResult::InvalidArgument;
    Engine e(transport, target, report, budget_ms);
    return finish(e, e.lock(*locked));
}
V2FrontendResult read_v2_frontend_tsids(FrontendTransport *transport, V2FrontendTarget target,
                                        std::array<std::uint16_t, 8> *tsids,
                                        V2FrontendReport *report, unsigned budget_ms) {
    if (report)
        *report = {};
    if (tsids)
        *tsids = {};
    if (!transport || !tsids || !v2_target_is_satellite(target) || !budget_ms)
        return V2FrontendResult::InvalidArgument;
    Engine e(transport, target, report, budget_ms);
    std::array<std::uint16_t, 8> result{};
    for (unsigned i = 0; i < 8; ++i) {
        std::uint8_t hi = 0, lo = 0;
        if (!e.dr(static_cast<std::uint8_t>(0xce + 2 * i), hi) ||
            !e.dr(static_cast<std::uint8_t>(0xcf + 2 * i), lo))
            return e.status;
        result[i] = static_cast<std::uint16_t>((std::uint16_t(hi) << 8) | lo);
    }
    *tsids = result;
    return V2FrontendResult::Completed;
}
V2FrontendResult select_v2_frontend_tsid(FrontendTransport *transport, V2FrontendTarget target,
                                         std::uint16_t tsid, V2FrontendReport *report,
                                         unsigned budget_ms) {
    if (report)
        *report = {};
    if (!transport || !v2_target_is_satellite(target) || !budget_ms)
        return V2FrontendResult::InvalidArgument;
    Engine e(transport, target, report, budget_ms);
    return finish(e, e.dw(0x8f, static_cast<std::uint8_t>(tsid >> 8)) &&
                         e.dw(0x90, static_cast<std::uint8_t>(tsid)));
}
} // namespace asicen
