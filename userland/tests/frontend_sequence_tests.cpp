#include "asicen/frontend_sequence.h"

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

class FakeTransport final : public asicen::FrontendTransport {
public:
    int control(const asicen::ControlTransfer& transfer, unsigned char* data) override {
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

    void delay_ms(unsigned ms) override {
        delays.push_back(ms);
    }

    std::vector<asicen::ControlTransfer> transfers;
    std::vector<unsigned> delays;
    std::uint8_t read_data = 0;
    int fail_at = -1;
    int short_at = -1;
    int status_at = -1;
};

asicen::FrontendPlan single(asicen::FrontendOp op) {
    asicen::FrontendPlan plan;
    plan.push_back(op);
    return plan;
}

bool has_gpio(const asicen::FrontendPlan& plan) {
    for (const asicen::FrontendOp& op : plan) {
        if (op.kind == asicen::FrontendOpKind::Control &&
            op.transfer.request == asicen::Request::Gpio) {
            return true;
        }
    }
    return false;
}

}  // namespace

int main() {
    // FC0012 PLL for terrestrial channel T13 (473142 kHz).
    const asicen::Fc0012Pll pll = asicen::compute_fc0012_pll(473142U);
    check(pll.valid, "pll valid");
    check(pll.reg1 == 0x06, "pll reg1");
    check(pll.reg2 == 0x13, "pll reg2");
    check(pll.reg3 == 0xdb, "pll reg3");
    check(pll.reg4 == 0x64, "pll reg4");
    check(pll.reg5 == 0x0f, "pll reg5");
    check(pll.reg6 == 0x80, "pll reg6");
    check(!pll.vco_select, "pll vco_select");
    check(!asicen::compute_fc0012_pll(0).valid, "pll rejects zero");
    check(!asicen::compute_fc0012_pll(1002000U).valid, "pll rejects out of band");

    // Recovered register/value facts.
    check(asicen::terrestrial_demod_init_count() == 22, "demod table size");
    check(asicen::terrestrial_demod_init_reg(0) == 0x04 &&
          asicen::terrestrial_demod_init_value(0) == 0x00,
          "demod table first");
    check(asicen::terrestrial_demod_init_reg(21) == 0xef &&
          asicen::terrestrial_demod_init_value(21) == 0x01,
          "demod table last");
    check(asicen::fc0012_init_count() == 21, "fc0012 table size");
    check(asicen::fc0012_init_reg(0) == 0x01 && asicen::fc0012_init_value(0) == 0x05,
          "fc0012 table first");
    check(asicen::fc0012_init_reg(17) == 0x12 && asicen::fc0012_init_value(17) == 0x1b,
          "fc0012 vendor gain reg12");
    check(asicen::fc0012_init_reg(18) == 0x13 && asicen::fc0012_init_value(18) == 0x10,
          "fc0012 vendor gain reg13");
    check(asicen::fc0012_init_reg(20) == 0x15 && asicen::fc0012_init_value(20) == 0x04,
          "fc0012 table last");

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
    check(saw_lnb_set, "power-on sets LNB bit");
    check(!clears_lnb, "power-on never clears LNB bit");
    check(power.back().kind == asicen::FrontendOpKind::I2cMask &&
          power.back().transfer.index == 0 &&
          power.back().and_mask == 0xef && power.back().or_mask == 0x00,
          "power-on ends with mode0 demod 0x1c rmw clear");

    // Sibling restore targets only bit 0x40.
    const asicen::FrontendPlan sibling = asicen::plan_sibling40_restore();
    check(sibling.size() == 1, "sibling restore single op");
    check(sibling[0].transfer.request == asicen::Request::Gpio &&
          sibling[0].transfer.value == 0x4040,
          "sibling restore gpio 0x4040");

    // Bounded mode1 demod read.
    const asicen::FrontendPlan read = asicen::plan_demod_read(1, 0xb0, 1);
    check(read.size() == 1, "demod read single op");
    check(read[0].transfer.request == asicen::Request::I2cRead &&
          read[0].transfer.value == 0xb030 && read[0].transfer.index == 0x0001 &&
          read[0].transfer.length == 2 && read[0].require_status,
          "demod read mode1 length2");
    check(asicen::plan_demod_read(2, 0xb0, 1).empty(), "demod read rejects local");
    check(asicen::plan_demod_read(1, 0xb0, 0).empty(), "demod read rejects zero length");
    check(asicen::plan_demod_read(1, 0xb0, 0x21).empty(), "demod read rejects overlong");

    // Init contains no GPIO write and ends with demod reg 0x0f = 0x34.
    const asicen::FrontendPlan init = asicen::plan_terrestrial_init();
    check(init.size() == 22 + 21 * 3 + 1, "terrestrial init size");
    check(!has_gpio(init), "terrestrial init performs no gpio write");
    check(init.back().transfer.request == asicen::Request::I2cWrite &&
          init.back().transfer.value == 0x0f30,
          "terrestrial init writes demod 0f");

    // Tune plan: no LNB/sibling gpio, contains vco calibration and demod 0x1e.
    const asicen::FrontendPlan tune = asicen::plan_fc0012_tune(473142U);
    check(!tune.empty(), "tune plan non-empty");
    check(!has_gpio(tune), "tune performs no gpio write");
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
    check(saw_vco, "tune has vco calibration");
    check(saw_demod_1e, "tune has mode1 demod 0x1e rmw");
    check(asicen::plan_fc0012_tune(0).empty(), "tune rejects zero");
    check(asicen::plan_fc0012_tune(1002000U).empty(), "tune rejects out of band");

    // Lock read.
    const asicen::FrontendPlan lock = asicen::plan_terrestrial_lock_read(473142U);
    check(lock.size() == 1 && lock[0].transfer.value == 0xb030 &&
          lock[0].transfer.index == 0x0001,
          "terrestrial lock read");
    check(asicen::plan_terrestrial_lock_read(0).empty(), "lock rejects zero");

    // Execution: order, stop-on-failure, short transfer, status failure.
    {
        FakeTransport fake;
        fake.fail_at = 2;
        const asicen::FrontendRunResult result =
            asicen::run_frontend_plan(power, &fake);
        check(result == asicen::FrontendRunResult::FailedTransfer, "fail result");
        check(fake.transfers.size() == 3, "stops at failing transfer");
    }
    {
        FakeTransport fake;
        fake.short_at = 1;
        const asicen::FrontendRunResult result =
            asicen::run_frontend_plan(power, &fake);
        check(result == asicen::FrontendRunResult::ShortTransfer, "short result");
        check(fake.transfers.size() == 2, "stops at short transfer");
    }
    {
        FakeTransport fake;
        const asicen::FrontendRunReport report = [&] {
            asicen::FrontendRunReport local{};
            const auto plan = asicen::plan_demod_read(1, 0xb0, 1);
            asicen::FrontendRunResult r = asicen::run_frontend_plan(plan, &fake, &local);
            check(r == asicen::FrontendRunResult::Completed, "read run completes");
            return local;
        }();
        check(report.have_last_read && report.last_read == 0x00, "read report value");
    }
    {
        FakeTransport fake;
        fake.status_at = 0;
        const auto plan = asicen::plan_demod_read(1, 0xb0, 1);
        const asicen::FrontendRunResult result =
            asicen::run_frontend_plan(plan, &fake);
        check(result == asicen::FrontendRunResult::FailedTransfer, "status failure");
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
        check(asicen::run_frontend_plan(set_plan, &fake) ==
                  asicen::FrontendRunResult::Completed,
              "rmw set completes");
        check(fake.transfers.size() == 2, "rmw set transfers");
        check((fake.transfers[1].index & 0xffU) == (0x11U | 0x30U), "rmw set value");

        FakeTransport clear;
        clear.read_data = 0x11;
        check(asicen::run_frontend_plan(clear_plan, &clear) ==
                  asicen::FrontendRunResult::Completed,
              "rmw clear completes");
        check((clear.transfers[1].index & 0xffU) == (0x11U & 0xefU), "rmw clear value");
    }

    // Target parsing.
    std::uint8_t bus = 0;
    std::uint8_t address = 0;
    check(asicen::parse_usb_location("1:29", &bus, &address) && bus == 1 &&
          address == 29,
          "parse location");
    check(asicen::parse_usb_location("0x1:0x1d", &bus, &address) && bus == 1 &&
          address == 29,
          "parse location hex");
    check(!asicen::parse_usb_location("1:", &bus, &address), "reject trailing colon");
    check(!asicen::parse_usb_location(":1", &bus, &address), "reject leading colon");
    check(!asicen::parse_usb_location("1:2:3", &bus, &address), "reject double colon");
    check(!asicen::parse_usb_location("300:1", &bus, &address), "reject bus overflow");
    check(!asicen::parse_usb_location("abc", &bus, &address), "reject non numeric");
    check(asicen::parse_port_path("1-2.1"), "parse port path");
    check(asicen::parse_port_path("1-2"), "parse port path single");
    check(asicen::parse_port_path("1-2.1.3"), "parse port path deep");
    check(!asicen::parse_port_path("1-"), "reject empty port");
    check(!asicen::parse_port_path("-2"), "reject missing bus");
    check(!asicen::parse_port_path("1"), "reject missing dash");
    check(!asicen::parse_port_path("abc"), "reject port text");

    // Empty plan and null transport boundaries.
    {
        FakeTransport fake;
        const asicen::FrontendPlan empty;
        check(asicen::run_frontend_plan(empty, &fake) ==
                  asicen::FrontendRunResult::Completed,
              "empty plan");
        check(asicen::run_frontend_plan(empty, nullptr) ==
                  asicen::FrontendRunResult::InvalidArgument,
              "null transport");
    }

    return failures == 0 ? 0 : 1;
}
