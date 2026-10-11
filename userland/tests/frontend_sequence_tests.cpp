#include "asicen/frontend_sequence.h"

#include <cstdint>
#include <cstdio>
#include <iostream>
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

class FakeTransport final : public asicen::FrontendTransport {
  public:
    int control(const asicen::ControlTransfer& transfer, unsigned char* data) override
    {
        const int index = static_cast<int>(transfers.size());
        transfers.push_back(transfer);
        for (std::uint16_t i = 0; i < transfer.length; ++i) {
            data[i] = 0;
        }
        if (transfer.length > 0) {
            data[0] = 1;
        }
        if (transfer.length > 1) {
            data[1] = read_data;
        }
        if (index == fail_at) {
            return -1;
        }
        if (index == short_at) {
            return static_cast<int>(transfer.length) - 1;
        }
        if (index == status_at && transfer.length > 0) {
            data[0] = 0;
        }
        return transfer.length;
    }

    void delay_ms(unsigned ms) override
    {
        delays.push_back(ms);
    }

    std::vector<asicen::ControlTransfer> transfers;
    std::vector<unsigned> delays;
    std::uint8_t read_data = 0;
    int fail_at = -1;
    int short_at = -1;
    int status_at = -1;
};

asicen::FrontendPlan single(asicen::FrontendOp op)
{
    asicen::FrontendPlan plan;
    plan.push_back(op);
    return plan;
}

bool has_gpio(const asicen::FrontendPlan& plan)
{
    for (const asicen::FrontendOp& op : plan) {
        if (op.kind == asicen::FrontendOpKind::Control &&
            op.transfer.request == asicen::Request::Gpio) {
            return true;
        }
    }
    return false;
}

} // namespace

bool test_all()
{
    // FC0012 PLL for terrestrial channel T13 (473142 kHz).
    const asicen::Fc0012Pll pll = asicen::compute_fc0012_pll(473142U);
    CHECK(check(pll.valid, "pll valid"));
    CHECK(check(pll.reg1 == 0x06, "pll reg1"));
    CHECK(check(pll.reg2 == 0x13, "pll reg2"));
    CHECK(check(pll.reg3 == 0xdb, "pll reg3"));
    CHECK(check(pll.reg4 == 0x64, "pll reg4"));
    CHECK(check(pll.reg5 == 0x0f, "pll reg5"));
    CHECK(check(pll.reg6 == 0x80, "pll reg6"));
    CHECK(check(!pll.vco_select, "pll vco_select"));
    CHECK(check(!asicen::compute_fc0012_pll(0).valid, "pll rejects zero"));
    CHECK(check(!asicen::compute_fc0012_pll(1002000U).valid, "pll rejects out of band"));

    // Recovered register/value facts.
    CHECK(check(asicen::terrestrial_demod_init_count() == 22, "demod table size"));
    CHECK(check(asicen::terrestrial_demod_init_reg(0) == 0x04 &&
                    asicen::terrestrial_demod_init_value(0) == 0x00,
                "demod table first"));
    CHECK(check(asicen::terrestrial_demod_init_reg(21) == 0xef &&
                    asicen::terrestrial_demod_init_value(21) == 0x01,
                "demod table last"));
    CHECK(check(asicen::fc0012_init_count() == 21, "fc0012 table size"));
    CHECK(check(asicen::fc0012_init_reg(0) == 0x01 && asicen::fc0012_init_value(0) == 0x05,
                "fc0012 table first"));
    CHECK(check(asicen::fc0012_init_reg(17) == 0x12 && asicen::fc0012_init_value(17) == 0x1b,
                "fc0012 vendor gain reg12"));
    CHECK(check(asicen::fc0012_init_reg(18) == 0x13 && asicen::fc0012_init_value(18) == 0x10,
                "fc0012 vendor gain reg13"));
    CHECK(check(asicen::fc0012_init_reg(20) == 0x15 && asicen::fc0012_init_value(20) == 0x04,
                "fc0012 table last"));

    // Safe power-on masks: never clear LNB bit 0x20 (value bit clear + mask set).
    const asicen::FrontendPlan power = asicen::plan_safe_power_on();
    bool saw_lnb_set = false;
    bool clears_lnb = false;
    for (const asicen::FrontendOp& op : power) {
        if (op.kind != asicen::FrontendOpKind::Control ||
            op.transfer.request != asicen::Request::Gpio) {
            continue;
        }
        const std::uint8_t value = static_cast<std::uint8_t>(op.transfer.value & 0xffU);
        const std::uint8_t mask = static_cast<std::uint8_t>((op.transfer.value >> 8U) & 0xffU);
        if ((mask & 0x20U) != 0 && (value & 0x20U) == 0) {
            clears_lnb = true;
        }
        if ((value & 0x20U) != 0 && (mask & 0x20U) != 0) {
            saw_lnb_set = true;
        }
    }
    CHECK(check(saw_lnb_set, "power-on sets LNB bit"));
    CHECK(check(!clears_lnb, "power-on never clears LNB bit"));
    CHECK(check(power.back().kind == asicen::FrontendOpKind::I2cMask &&
                    power.back().transfer.index == 0 && power.back().and_mask == 0xef &&
                    power.back().or_mask == 0x00,
                "power-on ends with mode0 demod 0x1c rmw clear"));

    // Sibling restore targets only bit 0x40.
    const asicen::FrontendPlan sibling = asicen::plan_sibling40_restore();
    CHECK(check(sibling.size() == 1, "sibling restore single op"));
    CHECK(check(sibling[0].transfer.request == asicen::Request::Gpio &&
                    sibling[0].transfer.value == 0x4040,
                "sibling restore gpio 0x4040"));

    // Bounded mode1 demod read.
    const asicen::FrontendPlan read = asicen::plan_demod_read(1, 0xb0, 1);
    CHECK(check(read.size() == 1, "demod read single op"));
    CHECK(check(read[0].transfer.request == asicen::Request::I2cRead &&
                    read[0].transfer.value == 0xb030 && read[0].transfer.index == 0x0001 &&
                    read[0].transfer.length == 2 && read[0].require_status,
                "demod read mode1 length2"));
    CHECK(check(asicen::plan_demod_read(2, 0xb0, 1).empty(), "demod read rejects local"));
    CHECK(check(asicen::plan_demod_read(1, 0xb0, 0).empty(), "demod read rejects zero length"));
    CHECK(check(asicen::plan_demod_read(1, 0xb0, 0x21).empty(), "demod read rejects overlong"));

    // Init contains no GPIO write and ends with demod reg 0x0f = 0x34.
    const asicen::FrontendPlan init = asicen::plan_terrestrial_init();
    CHECK(check(init.size() == 22 + 21 * 3 + 1, "terrestrial init size"));
    CHECK(check(!has_gpio(init), "terrestrial init performs no gpio write"));
    CHECK(check(init.back().transfer.request == asicen::Request::I2cWrite &&
                    init.back().transfer.value == 0x0f30,
                "terrestrial init writes demod 0f"));

    // Tune plan: no LNB/sibling gpio, contains vco calibration and demod 0x1e.
    const asicen::FrontendPlan tune = asicen::plan_fc0012_tune(473142U);
    CHECK(check(!tune.empty(), "tune plan non-empty"));
    CHECK(check(!has_gpio(tune), "tune performs no gpio write"));
    bool saw_vco = false;
    bool saw_demod_1e = false;
    for (const asicen::FrontendOp& op : tune) {
        if (op.kind == asicen::FrontendOpKind::Fc0012VcoCalibrate) {
            saw_vco = true;
        }
        if (op.kind == asicen::FrontendOpKind::I2cMask && op.transfer.value == 0x1e30 &&
            op.transfer.index == 0x0001) {
            saw_demod_1e = true;
        }
    }
    CHECK(check(saw_vco, "tune has vco calibration"));
    CHECK(check(saw_demod_1e, "tune has mode1 demod 0x1e rmw"));
    CHECK(check(asicen::plan_fc0012_tune(0).empty(), "tune rejects zero"));
    CHECK(check(asicen::plan_fc0012_tune(1002000U).empty(), "tune rejects out of band"));

    // Lock read.
    const asicen::FrontendPlan lock = asicen::plan_terrestrial_lock_read(473142U);
    CHECK(check(lock.size() == 1 && lock[0].transfer.value == 0xb030 &&
                    lock[0].transfer.index == 0x0001,
                "terrestrial lock read"));
    CHECK(check(asicen::plan_terrestrial_lock_read(0).empty(), "lock rejects zero"));

    // Execution: order, stop-on-failure, short transfer, status failure.
    {
        FakeTransport fake;
        fake.fail_at = 2;
        const asicen::FrontendRunResult result = asicen::run_frontend_plan(power, &fake);
        CHECK(check(result == asicen::FrontendRunResult::FailedTransfer, "fail result"));
        CHECK(check(fake.transfers.size() == 3, "stops at failing transfer"));
    }
    {
        FakeTransport fake;
        fake.short_at = 1;
        const asicen::FrontendRunResult result = asicen::run_frontend_plan(power, &fake);
        CHECK(check(result == asicen::FrontendRunResult::ShortTransfer, "short result"));
        CHECK(check(fake.transfers.size() == 2, "stops at short transfer"));
    }
    {
        FakeTransport fake;
        asicen::FrontendRunReport report{};
        const auto plan = asicen::plan_demod_read(1, 0xb0, 1);
        const auto result = asicen::run_frontend_plan(plan, &fake, &report);
        CHECK(check(result == asicen::FrontendRunResult::Completed, "read run completes"));
        CHECK(check(report.have_last_read && report.last_read == 0x00, "read report value"));
    }
    {
        FakeTransport fake;
        fake.status_at = 0;
        const auto plan = asicen::plan_demod_read(1, 0xb0, 1);
        const asicen::FrontendRunResult result = asicen::run_frontend_plan(plan, &fake);
        CHECK(check(result == asicen::FrontendRunResult::FailedTransfer, "status failure"));
    }

    // RMW masks are applied to the read value before the write.
    {
        asicen::FrontendPlan set_plan;
        asicen::FrontendPlan clear_plan;
        for (const asicen::FrontendOp& op : power) {
            if (op.kind != asicen::FrontendOpKind::I2cMask) {
                continue;
            }
            if (op.or_mask == 0x30) {
                set_plan = single(op);
            } else {
                clear_plan = single(op);
            }
        }
        FakeTransport fake;
        fake.read_data = 0x11;
        CHECK(check(asicen::run_frontend_plan(set_plan, &fake) ==
                        asicen::FrontendRunResult::Completed,
                    "rmw set completes"));
        CHECK(check(fake.transfers.size() == 2, "rmw set transfers"));
        CHECK(check((fake.transfers[1].index & 0xffU) == (0x11U | 0x30U), "rmw set value"));

        FakeTransport clear;
        clear.read_data = 0x11;
        CHECK(check(asicen::run_frontend_plan(clear_plan, &clear) ==
                        asicen::FrontendRunResult::Completed,
                    "rmw clear completes"));
        CHECK(check((clear.transfers[1].index & 0xffU) == (0x11U & 0xefU), "rmw clear value"));
    }

    // Target parsing.
    std::uint8_t bus = 0;
    std::uint8_t address = 0;
    CHECK(check(asicen::parse_usb_location("1:29", &bus, &address) && bus == 1 && address == 29,
                "parse location"));
    CHECK(check(!asicen::parse_usb_location("0x1:0x1d", &bus, &address),
                "reject hexadecimal USB location under the decimal selector contract"));
    CHECK(check(!asicen::parse_usb_location("1:", &bus, &address), "reject trailing colon"));
    CHECK(check(!asicen::parse_usb_location(":1", &bus, &address), "reject leading colon"));
    CHECK(check(!asicen::parse_usb_location("1:2:3", &bus, &address), "reject double colon"));
    CHECK(check(!asicen::parse_usb_location("300:1", &bus, &address), "reject bus overflow"));
    CHECK(check(!asicen::parse_usb_location("abc", &bus, &address), "reject non numeric"));
    CHECK(check(asicen::parse_port_path("1-2.1"), "parse port path"));
    CHECK(check(asicen::parse_port_path("1-2"), "parse port path single"));
    CHECK(check(asicen::parse_port_path("1-2.1.3"), "parse port path deep"));
    CHECK(check(!asicen::parse_port_path("1-"), "reject empty port"));
    CHECK(check(!asicen::parse_port_path("-2"), "reject missing bus"));
    CHECK(check(!asicen::parse_port_path("1"), "reject missing dash"));
    CHECK(check(!asicen::parse_port_path("abc"), "reject port text"));

    // Empty plan and null transport boundaries.
    {
        FakeTransport fake;
        const asicen::FrontendPlan empty;
        CHECK(check(asicen::run_frontend_plan(empty, &fake) == asicen::FrontendRunResult::Completed,
                    "empty plan"));
        CHECK(check(asicen::run_frontend_plan(empty, nullptr) ==
                        asicen::FrontendRunResult::InvalidArgument,
                    "null transport"));
    }

    return true;
}

int main()
{
    return test_all() ? 0 : 1;
}
