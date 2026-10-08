// SPDX-License-Identifier: GPL-2.0-or-later
//
// Recovered terrestrial frontend planning for the PX-W3U3.
//
// FC0012 algorithm source (recorded before incorporation): Linux tag v6.6,
//   drivers/media/tuners/fc0012.c,
//   Copyright (C) 2012 Hans-Frieder Vogt <hfvogt@gmx.net>,
//   SPDX-License-Identifier: GPL-2.0-or-later.
//
// The vendor register/value facts below were recovered read-only from
//   out/linux64/ReleaseToCustomer_64bit_130109_2/libPlexLib_W3U3.a
// member TunerControl.o using ar/objdump:
//   - safe power-on           TC_PowerTunerDemod        .text 0x28e0
//   - demod read/write        DemodRegRead/Write        .text 0x0320/0x0f80
//   - demod init tables       InitDemod                 .text 0x1040,
//       values  .rodata 0x200 (22 bytes) addresses .rodata 0x220 (22 bytes)
//   - FC0012 init table       InitRFDevice              .text 0x1970
//   - FC0012 RSSI + PLL       FC0012_RSSI_Calibration   .text 0x1ae0,
//                             Adpater_SetFreqISDBT      .text 0x1c50
//   - terrestrial lock        TC_IsLocked               .text 0x0880
// No vendor object code, crypto material or large table is copied: the
// demodulator init table is a 22-entry register/value fact, and the FC0012
// algorithm is reimplemented from the mainline source above.

#include "asicen/frontend_sequence.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>

#include "asicen/frontend_facts.h"
#include "asicen/write_protocol.h"

namespace asicen {
namespace {

constexpr std::uint32_t kFc0012BandProductLimit = 0x36523fU;
constexpr std::uint32_t kFc0012VcoSelectMinKhz = 3060000U;
constexpr std::uint16_t kFc0012XtalKhz2 = 18000U;
constexpr std::uint32_t kIsdbTDemodAgcBoundaryKhz = 260999U;
constexpr std::uint32_t kTerrestrialLockMaxKhz = 1000000U;

std::uint8_t demod_slave_for_source(std::uint8_t source) {
    return source == 0 ? W3u3FrontendFacts::kTerrestrialDemodI2c
                       : W3u3FrontendFacts::kSatelliteDemodI2c;
}

// Tuner-through-demod bridge read (TunerRegRead, source 0/1).
std::vector<ControlTransfer> build_tuner_read(std::uint8_t local,
                                              std::uint8_t source,
                                              std::uint8_t reg,
                                              std::uint16_t length) {
    std::vector<ControlTransfer> result;
    if (local > 1 || source > 1 || length == 0) {
        return result;
    }
    const std::uint8_t slave = demod_slave_for_source(source);
    const std::uint8_t bridge_arm[2] = {0xc6, reg};
    ControlTransfer arm{};
    if (!make_i2c_write_chunk(slave, 0xfe, bridge_arm, 2, false, &arm)) {
        return {};
    }
    const std::uint8_t bridge_read = 0xc7;
    ControlTransfer trigger{};
    if (!make_i2c_write_chunk(slave, 0xfe, &bridge_read, 1, true, &trigger)) {
        return {};
    }
    result.push_back(arm);
    result.push_back(trigger);
    result.push_back(make_i2c_read_no_wait(slave, length));
    return result;
}

// Tuner-through-demod bridge write (TunerRegWrite, source 0 only here).
std::vector<ControlTransfer> build_tuner_write(std::uint8_t local,
                                               std::uint8_t source,
                                               std::uint8_t reg,
                                               std::uint8_t value) {
    if (local > 1 || source > 1) {
        return {};
    }
    const std::uint8_t slave = demod_slave_for_source(source);
    const std::uint8_t payload[4] = {0xfe, 0xc6, reg, value};
    return build_i2c_write_sequence(slave, 0x00, payload, sizeof(payload), 2);
}

void append_control(FrontendPlan* plan, const ControlTransfer& transfer,
                    bool require_status, const char* label) {
    FrontendOp op{};
    op.kind = FrontendOpKind::Control;
    op.transfer = transfer;
    op.require_status = require_status;
    op.label = label;
    plan->push_back(op);
}

void append_sequence(FrontendPlan* plan, const std::vector<ControlTransfer>& sequence,
                     bool require_status, const char* label) {
    for (const ControlTransfer& transfer : sequence) {
        append_control(plan, transfer, require_status, label);
    }
}

void append_delay(FrontendPlan* plan, unsigned delay_ms) {
    FrontendOp op{};
    op.kind = FrontendOpKind::Delay;
    op.delay_ms = delay_ms;
    op.label = "delay";
    plan->push_back(op);
}

void append_gpio(FrontendPlan* plan, std::uint8_t value, std::uint8_t mask,
                 const char* label) {
    append_control(plan, make_gpio_set(value, mask), false, label);
}

void append_i2c_mask(FrontendPlan* plan, std::uint8_t slave, std::uint8_t reg,
                     std::uint8_t read_mode, std::uint8_t and_mask,
                     std::uint8_t or_mask, const char* label) {
    FrontendOp op{};
    op.kind = FrontendOpKind::I2cMask;
    op.transfer = make_i2c_read(slave, reg, 1, read_mode);
    op.require_status = true;
    op.and_mask = and_mask;
    op.or_mask = or_mask;
    op.label = label;
    plan->push_back(op);
}

void append_tuner_mask(FrontendPlan* plan, std::uint8_t local, std::uint8_t source,
                       std::uint8_t reg, std::uint8_t and_mask, std::uint8_t or_mask,
                       const char* label) {
    FrontendOp op{};
    op.kind = FrontendOpKind::TunerMask;
    op.require_status = true;
    op.local = local;
    op.source = source;
    op.reg = reg;
    op.and_mask = and_mask;
    op.or_mask = or_mask;
    op.label = label;
    plan->push_back(op);
}

void append_tuner_write(FrontendPlan* plan, std::uint8_t local, std::uint8_t source,
                        std::uint8_t reg, std::uint8_t value, const char* label) {
    append_sequence(plan, build_tuner_write(local, source, reg, value), true, label);
}

FrontendRunResult run_sequence(FrontendTransport* transport,
                               const std::vector<ControlTransfer>& sequence,
                               std::uint8_t* last_value,
                               FrontendRunReport* report) {
    if (sequence.empty()) {
        return FrontendRunResult::InvalidArgument;
    }
    for (const ControlTransfer& transfer : sequence) {
        if (transport->cancelled()) {
            return FrontendRunResult::Cancelled;
        }
        if (transport->expired()) {
            return FrontendRunResult::DeadlineExceeded;
        }
        std::vector<unsigned char> buffer(transfer.length == 0 ? 1U : transfer.length);
        const int rc = transport->control(transfer, buffer.data());
        if (rc < 0) {
            return FrontendRunResult::FailedTransfer;
        }
        if (static_cast<std::uint16_t>(rc) != transfer.length) {
            return FrontendRunResult::ShortTransfer;
        }
        if (transfer.length == 0 || buffer[0] != 1) {
            return FrontendRunResult::FailedTransfer;
        }
        if (last_value != nullptr && transfer.length >= 2) {
            *last_value = buffer[1];
        }
        if (report != nullptr && transfer.length >= 2) {
            report->last_read = buffer[1];
            report->have_last_read = true;
        }
    }
    return FrontendRunResult::Completed;
}

FrontendRunResult run_control(FrontendTransport* transport, const FrontendOp& op,
                              FrontendRunReport* report) {
    if (transport->cancelled()) {
        return FrontendRunResult::Cancelled;
    }
    if (transport->expired()) {
        return FrontendRunResult::DeadlineExceeded;
    }
    std::vector<unsigned char> buffer(op.transfer.length == 0 ? 1U : op.transfer.length);
    const int rc = transport->control(op.transfer, buffer.data());
    if (rc < 0) {
        return FrontendRunResult::FailedTransfer;
    }
    if (static_cast<std::uint16_t>(rc) != op.transfer.length) {
        return FrontendRunResult::ShortTransfer;
    }
    if (op.require_status) {
        if (op.transfer.length == 0 || buffer[0] != 1) {
            return FrontendRunResult::FailedTransfer;
        }
        if (report != nullptr && op.transfer.length >= 2) {
            report->last_read = buffer[1];
            report->have_last_read = true;
        }
    }
    return FrontendRunResult::Completed;
}

FrontendRunResult run_i2c_mask(FrontendTransport* transport, const FrontendOp& op) {
    if (op.transfer.length < 2) {
        return FrontendRunResult::InvalidArgument;
    }
    if (transport->cancelled()) {
        return FrontendRunResult::Cancelled;
    }
    if (transport->expired()) {
        return FrontendRunResult::DeadlineExceeded;
    }
    std::vector<unsigned char> buffer(op.transfer.length);
    int rc = transport->control(op.transfer, buffer.data());
    if (rc < 0) {
        return FrontendRunResult::FailedTransfer;
    }
    if (static_cast<std::uint16_t>(rc) != op.transfer.length || buffer[0] != 1) {
        return FrontendRunResult::FailedTransfer;
    }
    std::uint8_t value = static_cast<std::uint8_t>((buffer[1] & op.and_mask) | op.or_mask);
    const std::uint8_t slave = static_cast<std::uint8_t>(op.transfer.value & 0xffU);
    const std::uint8_t reg = static_cast<std::uint8_t>((op.transfer.value >> 8U) & 0xffU);
    ControlTransfer write{};
    if (!make_i2c_write_chunk(slave, reg, &value, 1, false, &write)) {
        return FrontendRunResult::InvalidArgument;
    }
    std::vector<unsigned char> write_buffer(write.length);
    rc = transport->control(write, write_buffer.data());
    if (rc < 0) {
        return FrontendRunResult::FailedTransfer;
    }
    if (static_cast<std::uint16_t>(rc) != write.length || write_buffer[0] != 1) {
        return FrontendRunResult::FailedTransfer;
    }
    return FrontendRunResult::Completed;
}

FrontendRunResult run_tuner_mask(FrontendTransport* transport, const FrontendOp& op,
                                 FrontendRunReport* report) {
    const std::vector<ControlTransfer> read =
        build_tuner_read(op.local, op.source, op.reg, 1);
    std::uint8_t value = 0;
    FrontendRunResult result = run_sequence(transport, read, &value, report);
    if (result != FrontendRunResult::Completed) {
        return result;
    }
    value = static_cast<std::uint8_t>((value & op.and_mask) | op.or_mask);
    const std::vector<ControlTransfer> write =
        build_tuner_write(op.local, op.source, op.reg, value);
    return run_sequence(transport, write, nullptr, report);
}

FrontendRunResult run_vco_calibration(FrontendTransport* transport, const FrontendOp& op,
                                      FrontendRunReport* report) {
    const auto write = [&](std::uint8_t reg, std::uint8_t value) {
        return run_sequence(transport, build_tuner_write(op.local, 0, reg, value),
                            nullptr, report);
    };

    FrontendRunResult result = write(0x0e, 0x80);
    if (result != FrontendRunResult::Completed) {
        return result;
    }
    result = write(0x0e, 0x00);
    if (result != FrontendRunResult::Completed) {
        return result;
    }
    transport->delay_ms(1);
    result = write(0x0e, 0x00);
    if (result != FrontendRunResult::Completed) {
        return result;
    }

    std::uint8_t tmp = 0;
    result = run_sequence(transport, build_tuner_read(op.local, 0, 0x0e, 1), &tmp, report);
    if (result != FrontendRunResult::Completed) {
        return result;
    }
    tmp = static_cast<std::uint8_t>(tmp & 0x3fU);

    std::uint8_t reg6 = op.reg6;
    if (op.vco_select) {
        if (tmp > 0x3cU) {
            reg6 = static_cast<std::uint8_t>(reg6 & static_cast<std::uint8_t>(~0x08U));
            result = write(0x06, reg6);
            if (result != FrontendRunResult::Completed) {
                return result;
            }
            result = write(0x0e, 0x80);
            if (result != FrontendRunResult::Completed) {
                return result;
            }
            result = write(0x0e, 0x00);
            if (result != FrontendRunResult::Completed) {
                return result;
            }
        }
    } else if (tmp < 0x02U) {
        reg6 = static_cast<std::uint8_t>(reg6 | 0x08U);
        result = write(0x06, reg6);
        if (result != FrontendRunResult::Completed) {
            return result;
        }
        result = write(0x0e, 0x80);
        if (result != FrontendRunResult::Completed) {
            return result;
        }
        result = write(0x0e, 0x00);
        if (result != FrontendRunResult::Completed) {
            return result;
        }
        transport->delay_ms(1);
    }
    return FrontendRunResult::Completed;
}

struct RegisterValue {
    std::uint8_t reg;
    std::uint8_t value;
};

// InitDemod terrestrial table, TunerControl.o .rodata 0x220 (addresses) and
// 0x200 (values), 22 entries.
constexpr RegisterValue kTerrestrialDemodInit[] = {
    {0x04, 0x00}, {0x11, 0x1a}, {0x12, 0x04}, {0x13, 0x33}, {0x14, 0x20},
    {0x31, 0x00}, {0x32, 0x00}, {0x38, 0x00}, {0x39, 0xaa}, {0x47, 0x00},
    {0x75, 0x02}, {0xb0, 0xa0}, {0xb2, 0x3d}, {0xb3, 0x25}, {0xb4, 0x8b},
    {0xb5, 0x4b}, {0xb6, 0x3f}, {0xb7, 0xff}, {0xb8, 0xff}, {0x22, 0x8f},
    {0x5f, 0x80}, {0xef, 0x01},
};

// InitDemod SIG_SOURCE=1 table from the TunerControl.o object whose SHA-256 is
// 26956331982fce11b4f1b9abbfe5439fcd46f82dca1b2a1499c75f429d83e84f.
// Registers are .rodata 0x280..0x2a9; values are .rodata 0x240..0x269
// (exactly 42 bytes). Only public register facts are represented; no vendor
// object code or unrelated tables are copied.
constexpr RegisterValue kSatelliteDemodInit[] = {
    {0x01, 0x90}, {0x03, 0x00}, {0x04, 0x02}, {0x06, 0x00}, {0x07, 0x41},
    {0x08, 0x00}, {0x09, 0x00}, {0x0a, 0xff}, {0x0c, 0x59}, {0x0d, 0xf2},
    {0x0e, 0xf0}, {0x0f, 0x50}, {0x10, 0xb2}, {0x11, 0x00}, {0x12, 0x30},
    {0x13, 0x80}, {0x14, 0x00}, {0x15, 0x00}, {0x17, 0x00}, {0x1a, 0x00},
    {0x1b, 0x00}, {0x1c, 0x00}, {0x1d, 0x00}, {0x1e, 0x00}, {0x1f, 0x00},
    {0x20, 0x00}, {0x38, 0x40}, {0x39, 0x10}, {0x3b, 0x90}, {0x51, 0xb0},
    {0x52, 0x89}, {0x53, 0xb3}, {0x5a, 0x2d}, {0x5b, 0xd3}, {0x85, 0x69},
    {0x87, 0x04}, {0x8d, 0x00}, {0x8e, 0x00}, {0xa3, 0x11}, {0xa4, 0x00},
    {0xa5, 0x40}, {0xa6, 0x04},
};

// InitRFDevice FC0012 table, TunerControl.o .text 0x1970, 21 entries. Matches
// mainline fc0012_init() except vendor gain reg 0x12/0x13.
constexpr RegisterValue kFc0012Init[] = {
    {0x01, 0x05}, {0x02, 0x10}, {0x03, 0x00}, {0x04, 0x00}, {0x05, 0x0f},
    {0x06, 0x00}, {0x07, 0x00}, {0x08, 0xff}, {0x09, 0x6e}, {0x0a, 0xb8},
    {0x0b, 0x82}, {0x0c, 0xf8}, {0x0d, 0x02}, {0x0e, 0x00}, {0x0f, 0x00},
    {0x10, 0x00}, {0x11, 0x00}, {0x12, 0x1b}, {0x13, 0x10}, {0x14, 0x00},
    {0x15, 0x04},
};

bool parse_u8(const std::string& text, std::uint8_t* out) {
    if (out == nullptr || text.empty()) {
        return false;
    }
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(text.c_str(), &end, 0);
    if (end == nullptr || *end != '\0' || parsed > 255) {
        return false;
    }
    *out = static_cast<std::uint8_t>(parsed);
    return true;
}

}  // namespace

std::size_t terrestrial_demod_init_count() {
    return sizeof(kTerrestrialDemodInit) / sizeof(kTerrestrialDemodInit[0]);
}

std::uint8_t terrestrial_demod_init_reg(std::size_t index) {
    return index < terrestrial_demod_init_count() ? kTerrestrialDemodInit[index].reg : 0;
}

std::uint8_t terrestrial_demod_init_value(std::size_t index) {
    return index < terrestrial_demod_init_count() ? kTerrestrialDemodInit[index].value : 0;
}

std::size_t satellite_demod_init_count() {
    return sizeof(kSatelliteDemodInit) / sizeof(kSatelliteDemodInit[0]);
}

std::uint8_t satellite_demod_init_reg(std::size_t index) {
    return index < satellite_demod_init_count() ? kSatelliteDemodInit[index].reg : 0;
}

std::uint8_t satellite_demod_init_value(std::size_t index) {
    return index < satellite_demod_init_count() ? kSatelliteDemodInit[index].value : 0;
}

std::size_t fc0012_init_count() {
    return sizeof(kFc0012Init) / sizeof(kFc0012Init[0]);
}

std::uint8_t fc0012_init_reg(std::size_t index) {
    return index < fc0012_init_count() ? kFc0012Init[index].reg : 0;
}

std::uint8_t fc0012_init_value(std::size_t index) {
    return index < fc0012_init_count() ? kFc0012Init[index].value : 0;
}

Fc0012Pll compute_fc0012_pll(std::uint32_t freq_khz) {
    Fc0012Pll pll{};
    if (freq_khz == 0) {
        return pll;
    }

    struct Band {
        std::uint32_t multi;
        std::uint8_t reg5;
        std::uint8_t reg6;
    };
    static const Band kBands[] = {
        {96, 0x82, 0x00}, {64, 0x82, 0x02}, {48, 0x42, 0x00}, {32, 0x42, 0x02},
        {24, 0x22, 0x00}, {16, 0x22, 0x02}, {12, 0x12, 0x00}, {8, 0x12, 0x02},
        {6, 0x0a, 0x00}, {4, 0x0a, 0x02},
    };

    std::uint32_t multi = 0;
    std::uint8_t reg5 = 0;
    std::uint8_t reg6 = 0;
    for (const Band& band : kBands) {
        const std::uint64_t product =
            static_cast<std::uint64_t>(freq_khz) * band.multi;
        // Largest multi with multi*freq <= 0x36523f (3559999).
        if (product <= kFc0012BandProductLimit) {
            multi = band.multi;
            reg5 = band.reg5;
            reg6 = band.reg6;
            break;
        }
    }
    if (multi == 0) {
        return pll;
    }

    const std::uint64_t f_vco = static_cast<std::uint64_t>(freq_khz) * multi;
    bool vco_select = false;
    if (f_vco >= kFc0012VcoSelectMinKhz) {
        reg6 = static_cast<std::uint8_t>(reg6 | 0x08U);
        vco_select = true;
    }

    std::uint16_t xdiv = static_cast<std::uint16_t>(f_vco / kFc0012XtalKhz2);
    if ((f_vco - static_cast<std::uint64_t>(xdiv) * kFc0012XtalKhz2) >=
        (kFc0012XtalKhz2 / 2U)) {
        xdiv = static_cast<std::uint16_t>(xdiv + 1U);
    }

    const std::uint8_t pm = static_cast<std::uint8_t>(xdiv / 8U);
    const std::uint8_t am = static_cast<std::uint8_t>(xdiv - 8U * pm);
    std::uint8_t reg1 = 0;
    std::uint8_t reg2 = 0;
    if (am < 2U) {
        reg1 = static_cast<std::uint8_t>(am + 8U);
        reg2 = static_cast<std::uint8_t>(pm - 1U);
    } else {
        reg1 = am;
        reg2 = pm;
    }

    const std::uint64_t remainder = f_vco - (f_vco / kFc0012XtalKhz2) * kFc0012XtalKhz2;
    std::uint16_t xin =
        static_cast<std::uint16_t>((remainder << 15U) / kFc0012XtalKhz2);
    if (xin >= 16384U) {
        xin = static_cast<std::uint16_t>(xin + 32768U);
    }
    const std::uint8_t reg3 = static_cast<std::uint8_t>(xin >> 8U);
    const std::uint8_t reg4 = static_cast<std::uint8_t>(xin & 0xffU);

    // Vendor FC0012 tune sets reg6 bit7 (6 MHz ISDB-T bandwidth) and reg5 bit0-2.
    reg6 = static_cast<std::uint8_t>(reg6 | 0x80U);
    reg5 = static_cast<std::uint8_t>(reg5 | 0x07U);

    pll.reg1 = reg1;
    pll.reg2 = reg2;
    pll.reg3 = reg3;
    pll.reg4 = reg4;
    pll.reg5 = reg5;
    pll.reg6 = reg6;
    pll.vco_select = vco_select;
    pll.valid = true;
    return pll;
}

std::uint32_t terrestrial_tune_center_khz(std::uint32_t freq_khz) {
    if (freq_khz == 0) {
        return 0;
    }
    // TunerControl.o .text 0x23bc-0x23f5: rdx = (freq>>3)/125*1000, i.e.
    // floor(freq/1000)*1000, then vendor-specific special ranges and a +143 kHz
    // offset before Adpater_SetFreqISDBT(rdx + 0x8f).
    const std::uint64_t truncated =
        static_cast<std::uint64_t>(freq_khz / 1000U) * 1000U;
    if (truncated >= 165000U && truncated <= 167000U) {
        return 0x28ce7U;  // 167143
    }
    if (truncated >= 195000U && truncated <= 197000U) {
        return 0x2fa47U;  // 195143
    }
    if (truncated >= 471000U && truncated <= 473000U) {
        return 0x73837U;  // 473143
    }
    return static_cast<std::uint32_t>(truncated + 0x8fU);
}

FrontendPlan plan_startup_subset() {
    FrontendPlan plan;
    // DTV_Start GPIO value 0x27 mask 0xfb with the sibling bit 0x40 excluded:
    // mask 0xbb. Sets bits 0/1/5, clears 3/4/7, leaves 0x40 untouched.
    append_gpio(&plan, 0x27, 0xbb, "startup subset gpio 27/bb");
    return plan;
}

FrontendPlan plan_safe_power_on() {
    FrontendPlan plan;
    append_gpio(&plan, 0x05, 0x05, "gpio set 0x05");
    append_delay(&plan, 10);
    append_gpio(&plan, 0x00, 0x04, "gpio clear 0x04");
    append_delay(&plan, 10);
    append_gpio(&plan, 0x05, 0x05, "gpio set 0x05");
    append_delay(&plan, 10);
    append_gpio(&plan, 0x10, 0x10, "gpio set 0x10");
    append_delay(&plan, 10);
    append_gpio(&plan, 0x00, 0x10, "gpio clear 0x10");
    append_delay(&plan, 10);
    append_gpio(&plan, 0x10, 0x10, "gpio set 0x10");
    append_delay(&plan, 10);
    // Sets LNB bit 0x20 and never clears it.
    append_gpio(&plan, 0x20, 0x20, "gpio set lnb 0x20");
    append_delay(&plan, 10);
    // Result of this tuner probe read is discarded by the vendor path.
    append_control(&plan, make_i2c_read(0xa8, 0x00, 1, 0), false, "probe i2c 0xa8");
    append_delay(&plan, 100);
    append_i2c_mask(&plan, 0x30, 0x1c, 0, 0xff, 0x30, "demod 1c set 0x30");
    append_delay(&plan, 10);
    append_i2c_mask(&plan, 0x30, 0x1c, 0, 0xef, 0x00, "demod 1c clear 0x10");
    return plan;
}

FrontendPlan plan_sibling40_restore() {
    FrontendPlan plan;
    append_gpio(&plan, 0x40, 0x40, "gpio set sibling 0x40");
    return plan;
}

FrontendPlan plan_demod_read(std::uint8_t local, std::uint8_t reg,
                             std::uint16_t length) {
    FrontendPlan plan;
    if (local > 1 || length == 0 || length > 0x20) {
        return plan;
    }
    const std::uint8_t slave = w3u3_demod_i2c_for_local_lane(local);
    if (slave == 0) {
        return plan;
    }
    append_control(&plan, make_i2c_read(slave, reg, length, 1), true, "demod read mode1");
    return plan;
}

FrontendPlan plan_demod_init_terrestrial() {
    FrontendPlan plan;
    for (std::size_t i = 0; i < terrestrial_demod_init_count(); ++i) {
        const std::uint8_t reg = terrestrial_demod_init_reg(i);
        const std::uint8_t value = terrestrial_demod_init_value(i);
        const std::uint8_t payload[1] = {value};
        append_sequence(&plan,
                        build_i2c_write_sequence(0x30, reg, payload, 1, 0), true,
                        "demod init");
    }
    return plan;
}

FrontendPlan plan_demod_init_satellite() {
    FrontendPlan plan;
    for (std::size_t i = 0; i < satellite_demod_init_count(); ++i) {
        const std::uint8_t reg = satellite_demod_init_reg(i);
        const std::uint8_t payload[1] = {satellite_demod_init_value(i)};
        append_sequence(&plan,
                        build_i2c_write_sequence(0x32, reg, payload, 1, 0), true,
                        "satellite demod init");
    }
    return plan;
}

FrontendPlan plan_fc0012_init() {
    FrontendPlan plan;
    for (std::size_t i = 0; i < fc0012_init_count(); ++i) {
        append_tuner_write(&plan, 1, 0, fc0012_init_reg(i), fc0012_init_value(i),
                           "fc0012 init");
    }
    return plan;
}

FrontendPlan plan_terrestrial_init() {
    FrontendPlan plan = plan_demod_init_terrestrial();
    const FrontendPlan rf = plan_fc0012_init();
    plan.insert(plan.end(), rf.begin(), rf.end());
    const std::uint8_t payload[1] = {0x34};
    append_sequence(&plan, build_i2c_write_sequence(0x30, 0x0f, payload, 1, 0), true,
                    "demod 0f = 0x34");
    return plan;
}

FrontendPlan plan_terrestrial_init_with_satellite_demod() {
    FrontendPlan plan = plan_terrestrial_init();
    const std::size_t insert_at = plan_demod_init_terrestrial().size() +
                                  plan_fc0012_init().size();
    const FrontendPlan satellite = plan_demod_init_satellite();
    if (plan.empty() || insert_at > plan.size() || satellite.empty()) return {};
    plan.insert(plan.begin() + static_cast<std::ptrdiff_t>(insert_at),
                satellite.begin(), satellite.end());
    return plan;
}

FrontendPlan plan_fc0012_tune(std::uint32_t freq_khz) {
    FrontendPlan plan;
    const Fc0012Pll pll = compute_fc0012_pll(freq_khz);
    if (!pll.valid) {
        return plan;
    }

    // FC0012_RSSI_Calibration (source 0).
    append_tuner_mask(&plan, 1, 0, 0x09, 0xff, 0x10, "fc0012 rssi reg9 +0x10");
    append_tuner_mask(&plan, 1, 0, 0x06, 0xff, 0x01, "fc0012 rssi reg6 +0x01");
    append_delay(&plan, 1);
    append_tuner_mask(&plan, 1, 0, 0x09, 0xef, 0x00, "fc0012 rssi reg9 -0x10");
    append_tuner_mask(&plan, 1, 0, 0x06, 0xef, 0x00, "fc0012 rssi reg6 -0x10");

    append_tuner_write(&plan, 1, 0, 0x01, pll.reg1, "fc0012 reg1");
    append_tuner_write(&plan, 1, 0, 0x02, pll.reg2, "fc0012 reg2");
    append_tuner_write(&plan, 1, 0, 0x03, pll.reg3, "fc0012 reg3");
    append_tuner_write(&plan, 1, 0, 0x04, pll.reg4, "fc0012 reg4");
    append_tuner_write(&plan, 1, 0, 0x05, pll.reg5, "fc0012 reg5");
    append_tuner_write(&plan, 1, 0, 0x06, pll.reg6, "fc0012 reg6");

    append_tuner_write(&plan, 1, 0, 0x0e, 0x80, "fc0012 vco arm");
    append_tuner_write(&plan, 1, 0, 0x0e, 0x00, "fc0012 vco clear");
    append_delay(&plan, 1);
    append_tuner_write(&plan, 1, 0, 0x0e, 0x00, "fc0012 vco settle");

    FrontendOp vco{};
    vco.kind = FrontendOpKind::Fc0012VcoCalibrate;
    vco.local = 1;
    vco.reg6 = pll.reg6;
    vco.vco_select = pll.vco_select;
    vco.label = "fc0012 vco calibrate";
    plan.push_back(vco);

    // Vendor-specific demod AGC tweak on reg 0x1e (mode1 read, mode0 write).
    if (freq_khz <= kIsdbTDemodAgcBoundaryKhz) {
        append_i2c_mask(&plan, 0x30, 0x1e, 1, 0xcf, 0x20, "demod 1e agc low");
    } else {
        append_i2c_mask(&plan, 0x30, 0x1e, 1, 0xff, 0x30, "demod 1e agc high");
    }
    return plan;
}

FrontendPlan plan_terrestrial_tune_full(std::uint32_t freq_khz,
                                        std::uint8_t bandwidth_mhz) {
    FrontendPlan plan;
    if (freq_khz == 0 || bandwidth_mhz == 0) {
        return plan;
    }
    const std::uint32_t center = terrestrial_tune_center_khz(freq_khz);
    if (center == 0 || !compute_fc0012_pll(center).valid) {
        return plan;
    }
    const std::uint8_t p25[1] = {0x00};
    append_sequence(&plan, build_i2c_write_sequence(0x30, 0x25, p25, 1, 0), true,
                    "tc_setfreq demod 0x25 = 0x00");
    const std::uint8_t p23[1] = {0x4d};
    append_sequence(&plan, build_i2c_write_sequence(0x30, 0x23, p23, 1, 0), true,
                    "tc_setfreq demod 0x23 = 0x4d");
    FrontendOp tune{};
    tune.kind = FrontendOpKind::TerrestrialTune;
    tune.local = 1;
    tune.frequency_khz = freq_khz;
    tune.bandwidth_mhz = bandwidth_mhz;
    tune.label = "terrestrial tune (TC_SetFrequency loop + tail)";
    plan.push_back(tune);
    return plan;
}

FrontendPlan plan_terrestrial_lock_read(std::uint32_t freq_khz) {
    FrontendPlan plan;
    // Terrestrial-only: reject zero and out-of-range (satellite) frequencies so
    // a >1 MHz request cannot silently read the satellite lock register.
    if (freq_khz == 0 || freq_khz > kTerrestrialLockMaxKhz) {
        return plan;
    }
    append_control(&plan, make_i2c_read(0x30, 0xb0, 1, 1), true, "terrestrial lock 0xb0");
    return plan;
}

FrontendPlan plan_stream_setup(std::uint8_t local) {
    return plan_stream_setup(local, 1);
}

FrontendPlan plan_stream_setup(std::uint8_t local, std::uint8_t reset_state) {
    FrontendPlan plan;
    if (local > 1 || reset_state > 1) {
        return plan;
    }
    FrontendOp reset{};
    reset.kind = FrontendOpKind::FilterReset;
    reset.local = local;
    reset.block_rmw = true;  // third USB_FilterReset argument
    reset.flag = reset_state;  // fourth argument, reset state
    reset.label = "usb filter reset";
    plan.push_back(reset);

    const std::uint8_t boundary[2] = {0x1f, 0xff};
    ControlTransfer write{};
    if (make_cf_write(local, 0x41, boundary, 2, &write)) {
        append_control(&plan, write, false, "pid boundary 0x41");
    }
    if (make_cf_write(local, 0x43, boundary, 2, &write)) {
        append_control(&plan, write, false, "pid boundary 0x43");
    }
    return plan;
}

namespace {
FrontendRunResult run_plan_ops(const FrontendPlan& plan, FrontendTransport* transport,
                               FrontendRunReport* report);
}

// TC_SetFrequency terrestrial loop and tail (TunerControl.o .text 0x2340-0x26f0
// with DTV_SetTunerFreq defaults ptr[8]=ptr[0x10]=ptr[0x18]=0). The
// deterministic demod 0x25=0x00 / demod 0x23=0x4d prefix is emitted by
// plan_terrestrial_tune_full before this op.
FrontendRunResult run_terrestrial_tune(FrontendTransport* transport, const FrontendOp& op,
                                       FrontendRunReport* report) {
    const std::uint8_t local = 1;
    const std::uint8_t source = 0;
    const std::uint32_t center = terrestrial_tune_center_khz(op.frequency_khz);
    if (center == 0) {
        return FrontendRunResult::InvalidArgument;
    }

    const auto single_control = [&](const ControlTransfer& transfer, bool require_status) {
        FrontendOp single{};
        single.kind = FrontendOpKind::Control;
        single.transfer = transfer;
        single.require_status = require_status;
        return run_control(transport, single, report);
    };
    const auto demod_write = [&](std::uint8_t reg, std::uint8_t value) {
        const std::uint8_t payload[1] = {value};
        return run_sequence(transport,
                            build_i2c_write_sequence(0x30, reg, payload, 1, 0), nullptr,
                            report);
    };
    const auto tuner_write = [&](std::uint8_t reg, std::uint8_t value) {
        return run_sequence(transport, build_tuner_write(local, source, reg, value),
                            nullptr, report);
    };
    const auto demod_1c_rmw = [&](std::uint8_t and_mask, std::uint8_t or_mask) {
        FrontendOp rmw{};
        rmw.kind = FrontendOpKind::I2cMask;
        rmw.transfer = make_i2c_read(0x30, 0x1c, 1, 0);
        rmw.and_mask = and_mask;
        rmw.or_mask = or_mask;
        return run_i2c_mask(transport, rmw);
    };

    for (int count = 0;; ++count) {
        FrontendRunResult result = tuner_write(0x13, 0x02);  // table[ptr[0x18]=0]
        if (result != FrontendRunResult::Completed) {
            return result;
        }
        result = single_control(make_gpio_set(0x00, 0x01), false);  // LNA off
        if (result != FrontendRunResult::Completed) {
            return result;
        }
        const FrontendPlan adapter = plan_fc0012_tune(center);
        if (adapter.empty()) {
            return FrontendRunResult::InvalidArgument;
        }
        result = run_plan_ops(adapter, transport, report);
        if (result != FrontendRunResult::Completed) {
            return result;
        }
        std::uint8_t reg0e = 0;
        result = run_sequence(transport, build_tuner_read(local, source, 0x0e, 1),
                              &reg0e, report);
        if (result != FrontendRunResult::Completed) {
            return result;
        }
        if ((reg0e & 0x40U) != 0U) {
            break;
        }
        if (count == 3) {
            break;
        }
        result = demod_1c_rmw(0xff, 0x30);
        if (result != FrontendRunResult::Completed) {
            return result;
        }
        transport->delay_ms(10);
        result = demod_1c_rmw(0xef, 0x00);
        if (result != FrontendRunResult::Completed) {
            return result;
        }
        transport->delay_ms(10);
        result = run_plan_ops(plan_fc0012_init(), transport, report);  // InitRFDevice
        if (result != FrontendRunResult::Completed) {
            return result;
        }
    }

    FrontendRunResult result = demod_write(0x0f, 0x34);
    if (result != FrontendRunResult::Completed) {
        return result;
    }
    result = demod_write(0x01, 0x40);  // ReAcqDemod (terrestrial, source 0)
    if (result != FrontendRunResult::Completed) {
        return result;
    }
    return demod_write(0x23, 0x4c);
}

// USB_FilterReset(block_rmw=1): read the 0x45-byte CF block starting at subcmd 0, toggle
// byte 0x40 bit2 by `flag`, FUSBDTV_Cmd_Reset_Channel(local, flag), write the
// block back with all-zero 3-byte chunks skipped (as the vendor USB_CF_Write
// does). The block contents come from the device, not a copied vendor table.
FrontendRunResult run_filter_reset(
    FrontendTransport* transport, const FrontendOp& op,
    std::array<std::uint8_t, 0x45>* block_before = nullptr,
    std::array<std::uint8_t, 0x45>* block_after = nullptr) {
    const std::uint8_t local = op.local;
    if (local > 1) {
        return FrontendRunResult::InvalidArgument;
    }
    const auto run_one = [&](const ControlTransfer& transfer) {
        if (transport->cancelled()) {
            return FrontendRunResult::Cancelled;
        }
        if (transport->expired()) {
            return FrontendRunResult::DeadlineExceeded;
        }
        std::vector<unsigned char> buffer(transfer.length == 0 ? 1U : transfer.length);
        const int rc = transport->control(transfer, buffer.data());
        if (rc < 0) {
            return FrontendRunResult::FailedTransfer;
        }
        if (static_cast<std::uint16_t>(rc) != transfer.length) {
            return FrontendRunResult::ShortTransfer;
        }
        return FrontendRunResult::Completed;
    };

    std::array<std::uint8_t, 0x45> block{};
    std::size_t offset = 0;
    if (op.block_rmw) {
        while (offset < block.size()) {
            const std::size_t chunk = std::min<std::size_t>(0x20, block.size() - offset);
            const ControlTransfer read = make_cf_read(
                local, static_cast<std::uint8_t>(offset), static_cast<std::uint16_t>(chunk));
            if (transport->cancelled()) {
                return FrontendRunResult::Cancelled;
            }
            if (transport->expired()) {
                return FrontendRunResult::DeadlineExceeded;
            }
            std::vector<unsigned char> buffer(read.length);
            const int rc = transport->control(read, buffer.data());
            if (rc < 0) {
                return FrontendRunResult::FailedTransfer;
            }
            if (static_cast<std::uint16_t>(rc) != read.length) {
                return FrontendRunResult::ShortTransfer;
            }
            std::copy_n(buffer.begin() + 1, chunk, block.begin() + offset);
            offset += chunk;
        }

        if (block_before != nullptr) *block_before = block;

        if (op.flag == 0) {
            block[0x40] = static_cast<std::uint8_t>(block[0x40] & 0xfbU);
        } else {
            block[0x40] = static_cast<std::uint8_t>(block[0x40] | 0x04U);
        }
        if (block_after != nullptr) *block_after = block;
    }

    FrontendRunResult result = run_one(make_reset_channel(local, op.flag));
    if (result != FrontendRunResult::Completed) {
        return result;
    }

    offset = 0;
    while (op.block_rmw && offset < block.size()) {
        const std::size_t chunk = std::min<std::size_t>(3, block.size() - offset);
        bool all_zero = true;
        for (std::size_t i = 0; i < chunk; ++i) {
            if (block[offset + i] != 0) {
                all_zero = false;
            }
        }
        if (!all_zero) {
            ControlTransfer write{};
            if (!make_cf_write(local, static_cast<std::uint8_t>(offset),
                               block.data() + offset, chunk, &write)) {
                return FrontendRunResult::InvalidArgument;
            }
            result = run_one(write);
            if (result != FrontendRunResult::Completed) {
                return result;
            }
        }
        offset += chunk;
    }
    return FrontendRunResult::Completed;
}

namespace {

FrontendRunResult run_plan_ops(const FrontendPlan& plan, FrontendTransport* transport,
                               FrontendRunReport* report) {
    for (const FrontendOp& op : plan) {
        if (transport->cancelled()) {
            return FrontendRunResult::Cancelled;
        }
        if (transport->expired()) {
            return FrontendRunResult::DeadlineExceeded;
        }
        FrontendRunResult result = FrontendRunResult::Completed;
        switch (op.kind) {
            case FrontendOpKind::Control:
                result = run_control(transport, op, report);
                break;
            case FrontendOpKind::Delay:
                transport->delay_ms(op.delay_ms);
                break;
            case FrontendOpKind::I2cMask:
                result = run_i2c_mask(transport, op);
                break;
            case FrontendOpKind::TunerMask:
                result = run_tuner_mask(transport, op, report);
                break;
            case FrontendOpKind::Fc0012VcoCalibrate:
                result = run_vco_calibration(transport, op, report);
                break;
            case FrontendOpKind::TerrestrialTune:
                result = run_terrestrial_tune(transport, op, report);
                break;
            case FrontendOpKind::FilterReset:
                result = run_filter_reset(transport, op);
                break;
        }
        if (result != FrontendRunResult::Completed) {
            return result;
        }
        if (report != nullptr) {
            ++report->ops_completed;
        }
    }
    return FrontendRunResult::Completed;
}

}  // namespace

FrontendRunResult run_frontend_plan(const FrontendPlan& plan,
                                    FrontendTransport* transport,
                                    FrontendRunReport* report) {
    if (transport == nullptr) {
        return FrontendRunResult::InvalidArgument;
    }
    FrontendRunReport local_report{};
    return run_plan_ops(plan, transport, report != nullptr ? report : &local_report);
}

FrontendRunResult run_filter_reset_operation(
    FrontendTransport* transport, std::uint8_t local, std::uint8_t reset_state,
    std::array<std::uint8_t, 0x45>* block_before,
    std::array<std::uint8_t, 0x45>* block_after) {
    if (transport == nullptr || local > 1 || reset_state > 1)
        return FrontendRunResult::InvalidArgument;
    FrontendOp op{};
    op.kind = FrontendOpKind::FilterReset;
    op.local = local;
    op.block_rmw = true;
    op.flag = reset_state;
    return run_filter_reset(transport, op, block_before, block_after);
}

bool parse_usb_location(const std::string& text, std::uint8_t* bus,
                        std::uint8_t* address) {
    if (bus == nullptr || address == nullptr) {
        return false;
    }
    const std::size_t colon = text.find(':');
    if (colon == std::string::npos || text.find(':', colon + 1) != std::string::npos) {
        return false;
    }
    return parse_u8(text.substr(0, colon), bus) &&
           parse_u8(text.substr(colon + 1), address);
}

bool parse_port_path(const std::string& text) {
    if (text.empty()) {
        return false;
    }
    std::size_t index = 0;
    std::size_t run = 0;
    while (index < text.size() &&
           std::isdigit(static_cast<unsigned char>(text[index])) != 0) {
        ++index;
        ++run;
    }
    if (run == 0 || index >= text.size() || text[index] != '-') {
        return false;
    }
    ++index;
    while (true) {
        run = 0;
        while (index < text.size() &&
               std::isdigit(static_cast<unsigned char>(text[index])) != 0) {
            ++index;
            ++run;
        }
        if (run == 0) {
            return false;
        }
        if (index == text.size()) {
            return true;
        }
        if (text[index] != '.') {
            return false;
        }
        ++index;
    }
}

}  // namespace asicen
