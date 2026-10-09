// SPDX-License-Identifier: GPL-2.0-only
// Common PLL and register initialization adapted from nm131.c:
// Copyright (C) Budi Rachmanto, AreMa Inc. <info@are.ma>.
// nm131.c declares MODULE_LICENSE("GPL") and does not state a GPL version.
// Per-family ISDB-T differences independently transcribed from documented
// register operations in the official V2 driver. See v2-nmi-families.md.
#include "asicen/v2_nmi.h"
#include <algorithm>
#include <array>
#include <limits>

namespace asicen {
V2NmiFamily v2_nmi_family(std::uint32_t id) {
    if ((id & 0xfff00U) == 0x13100U)
        return V2NmiFamily::Nm131;
    if ((id & 0xfff00U) == 0x12000U)
        return V2NmiFamily::Nm120;
    if ((id & 0xffff00U) == 0x13000U)
        return V2NmiFamily::Nm130;
    if ((id & 0xffff00U) == 0x813000U)
        return V2NmiFamily::Extended813000;
    return V2NmiFamily::Unsupported;
}
namespace {
using U = std::uint32_t;
struct BytePair {
    std::uint8_t reg, value;
};
class CheckedIo {
  public:
    explicit CheckedIo(V2NmiIo &io) : io_(io) {}
    bool read(std::uint16_t reg, U &value, std::uint8_t width = 4) {
        if (!io_.healthy() || !io_.read(reg, &value, width) || !io_.healthy())
            return false;
        if (width == 1)
            value &= 0xffU;
        return true;
    }
    bool write(std::uint16_t reg, U value, std::uint8_t width = 4) {
        if (width == 1)
            value &= 0xffU;
        if (reg == 0x36)
            value &= 0x7fU; // initializer config+4 = 0: LDO on
        return io_.healthy() && io_.write(reg, value, width) && io_.healthy();
    }
    bool rf(std::uint16_t reg, U value) {
        return write(reg, value, 1);
    }
    bool modify(std::uint16_t reg, U mask, U bits) {
        U value = 0;
        return read(reg, value) && write(reg, (value & mask) | bits);
    }
    template <std::size_t N> bool pairs(const BytePair (&pairs)[N]) {
        for (auto pair : pairs)
            if (!rf(pair.reg, pair.value))
                return false;
        return true;
    }
    bool demod(std::uint8_t reg, std::uint8_t value) {
        return io_.healthy() && io_.demod_write(reg, value) && io_.healthy();
    }
    bool wait(unsigned ms) {
        return io_.healthy() && io_.delay_ms(ms) && io_.healthy();
    }

  private:
    V2NmiIo &io_;
};

bool nm130_reference(CheckedIo &io, U hz) {
    // 0x196f2..0x197bf: exact-frequency reference-divider exceptions.
    constexpr U double_reference[] = {
        171000000, 506000000, 554000000, 602000000, 650000000,
        698000000, 746000000, 794000000, 842000000,
    };
    U value = 0;
    if (!io.read(0x21, value, 1))
        return false;
    if (hz == 147000000)
        value |= 3;
    else if (std::find(std::begin(double_reference), std::end(double_reference), hz) !=
             std::end(double_reference))
        value = (value & ~1U) | 2;
    else
        value = (value & ~2U) | 1;
    return io.rf(0x21, value);
}

bool tune_rf(CheckedIo &io, U hz, V2NmiFamily family, U &clock_offset) {
    constexpr U bounds[] = {0, 0, 434000000, 237000000, 214000000, 118000000, 79000000, 53000000};
    constexpr std::uint8_t multiplier[] = {0, 1, 2, 3, 4, 6, 9, 12};
    struct Filter {
        U hz;
        std::uint8_t r8, r9;
    };
    constexpr Filter filter[] = {
        {45000000, 167, 58}, {55000000, 151, 57}, {65000000, 100, 54},
        {75000000, 83, 53},  {85000000, 82, 53},  {95000000, 65, 52},
        {105000000, 64, 52}, {115000000, 64, 52}, {125000000, 0, 0},
    };
    U rf = hz, xo = 24000, vco = 0, integer = 0, fraction = 0;
    unsigned band = 7;
    U previous_rf05 = 0x87; // reset-state cache from 0x1b713
    // Vendor permits exactly one +1 kHz retry for a nonzero fraction.
    for (unsigned pass = 0; pass != 2; ++pass) {
        if (family == V2NmiFamily::Nm130 && !nm130_reference(io, rf))
            return false;
        const U rf05 = rf >= 120100000 && rf <= 120400000 ? 5 : 0x85;
        if (rf05 != previous_rf05) {
            if (!io.rf(0x05, rf05))
                return false;
            previous_rf05 = rf05;
        }
        band = 7;
        while (band > 1 && rf > bounds[band])
            --band;
        unsigned fi = 0;
        while (fi < 8 && !(rf > filter[fi].hz && rf <= filter[fi + 1].hz))
            ++fi;
        if (!io.rf(0x08, filter[fi].r8) || !io.rf(0x09, filter[fi].r9))
            return false;
        vco = (rf / 1000U) * 8U * multiplier[band];
        U ref = 0;
        if (!io.read(0x21, ref, 1))
            return false;
        if ((ref & 3U) == 2)
            xo *= 2;
        else if ((ref & 3U) == 3)
            xo >>= 1;
        integer = vco / xo;
        // Deliberately preserve the driver's uint32 multiply/truncation.
        fraction = ((vco % xo) * (0x80000000U / xo) >> 12) & 0x7ffffU;
        if (!fraction || pass)
            break;
        rf += 1000;
    }
    U divider = std::max<U>(16, std::min<U>(31, (vco / 216000U) & 0xffU));
    clock_offset = (vco << 9) / divider - 110592000U;
    if (!io.rf(1, (integer & 0xffffU) >> 1) || !io.rf(2, (integer & 1U) | (fraction * 2U)) ||
        !io.rf(3, fraction >> 7) || !io.rf(4, ((fraction >> 15) & 15U) | (divider << 4)))
        return false;
    U value = 0;
    if (!io.read(0x1d, value, 1) || !io.rf(0x1d, (value & 0x1fU) | (band << 5)))
        return false;
    if (family != V2NmiFamily::Nm120 && !io.rf(0x1b, fraction ? 0x0e : 0x08))
        return false;

    if (family == V2NmiFamily::Nm120) {
        if (!io.rf(0x25, rf < 300000000 ? 0x78 : 0xf4) ||
            !io.rf(0x27, rf < 300000000 ? 0x7f : 0xef) ||
            !io.rf(0x29, rf < 300000000 ? 0x7f : 0x4f) ||
            !io.rf(0x2e, rf < 300000000 ? 0x12 : 0x34) ||
            !io.rf(0x36, rf < 155000000 ? 0x54 : 0x7c))
            return false;
    } else if (family == V2NmiFamily::Nm130) {
        const bool special = rf == 147000000 || rf == 171000000 || rf == 195000000 ||
                             rf == 219000000 || rf == 243000000;
        const bool lower = rf == 115000000 || rf == 123000000;
        if (!io.rf(0x0e, special ? 0x45 : 0x25) || !io.rf(0x25, lower ? 0x43 : 0x56) ||
            !io.rf(0x2e, lower ? 0x78 : 0x56) || !io.rf(0x30, rf < 139000000 ? 0xdf : 1) ||
            !io.rf(0x32, rf < 139000000 ? 0xdf : 1))
            return false;
    } else {
        if (!io.rf(0x0e, 0x45) || !io.rf(0x25, 0xfa))
            return false;
        if (family == V2NmiFamily::Nm131 && !io.rf(0x2e, 0x56))
            return false;
        if (!io.rf(0x26, 0x82))
            return false;
        const U gain = rf >= 786000000 ? 0x3f : rf >= 762000000 ? 0x5f : 0x7f;
        const U range = rf >= 818000000 ? 0xaf : 0xdf;
        if (!io.rf(0x27, gain) || !io.rf(0x29, gain) || !io.rf(0x30, range) ||
            !io.rf(0x32, range) || !io.rf(0x36, rf < 155000000 ? 0x54 : 0x7c) ||
            !io.rf(0x34, 0x78) || !io.rf(0x35, 0x54))
            return false;
    }
    return io.rf(0x37, rf >= 155000000 && rf < 300000000 ? 0x9c : 0x84);
}

bool tune_digital(CheckedIo &io, U hz, V2NmiFamily family, U clock) {
    // Mode-transition writes are reapplied to make each tune independent of
    // vendor global caches. This establishes standard 6 / output mode 2.
    if (family == V2NmiFamily::Nm131 && !io.write(0x1c0, 0x2d8c19c7))
        return false;
    if (!io.write(0x130, 0x200) ||
        !io.write(0x164, family == V2NmiFamily::Nm130 ? 0x300
                         : hz < 300000000             ? 0x600
                                                      : 0x500) ||
        !io.modify(0x234, 0xf1ffffff, 0x08000000) || !io.write(0x20c, 0x400))
        return false;
    U value = 0;
    if (!io.read(0x280, value) || !io.write(0x280, value) || !io.modify(0x238, 0xffffc000, 0) ||
        !io.modify(0x32c, 0xbfffbfff, 0))
        return false;
    const U rf = clock / 6750U + 16384U;
    if (!io.write(0x230, ((126217U << 15) / rf) | 0x80000U) || !io.write(0x250, 0) ||
        !io.write(0x27c, 0x1010) || !io.write(0x1bc, 0) || !io.write(0x1c8, 0x8079) ||
        !io.write(0x1cc, 0x8079) || !io.write(0x1d0, 0x8079) || !io.write(0x110, 0x4c52) ||
        !io.write(0x26c, 2) || !io.write(0x270, 6))
        return false;
    // 0x182d8: divide before doubling; GPL's doubled numerator can differ by 1.
    const U ifword = 2U * (262144000U / ((clock >> 14) + 6750U));
    if (!io.read(0x21c, value) ||
        !io.write(0x21c, (((value & 0xffc00000U) | ifword) & 0xcfbfffffU) | 0x08000000U) ||
        !io.modify(0x234, 0xcfc00000, 0) || !io.modify(0x12c, 0xffff9f7f, 0x1000) ||
        !io.write(0x138, 0x5b18) || !io.write(0x148, 0x1ff082) ||
        !io.modify(0x200, 0xffffc000, 0x8800))
        return false;
    const U sample = ((864U * (clock >> 5) - 0x4e058000U) / 216000U & 0xfe00fff3U) | 3U;
    if (!io.write(0x210, sample) || !io.read(0x104, value))
        return false;
    value = (value & 0x97ffffd1U) | 0x10000011U;
    value = (value & 0xffff87ffU) | (1U << 11);
    value = (value & 0xffff7fffU) | (1U << 15);
    const U c = clock > 3686396 ? 2 : clock > 1843192 ? 1 : 0;
    value = ((value & 0xfff0ffffU) | (c << 16)) & 0xfc0fffffU;
    value |=
        family == V2NmiFamily::Nm120 || family == V2NmiFamily::Nm130 ? 0x00a00000U : 0x02600000U;
    if (!io.write(0x104, value))
        return false;
    value = (value & ~0x10U) | 0x20000020U;
    if (!io.write(0x104, value) || !io.wait(1))
        return false;
    U status = 0;
    if (!io.read(0x328, status))
        return false;
    if (status && (!io.write(0x104, value & ~0x20U) || !io.write(0x104, value | 0x20U)))
        return false;
    return true;
}
} // namespace

// V2 NMI initialization, statically traced from official x64 image.
// RVA 0x1c1b8 selects defaults; 0x1b650 writes the chip-family tables;
// vtable+0x128 -> 0x1a66c sets ltgain, vtable+8 -> 0x1b358 rereads ID.
// Tables at 0x28eb8/0x28ef8 match public GPL nm131.c by Budi Rachmanto.
// The vendor initializer performs no calibration, delay or bounded poll.
V2NmiResult initialize_v2_nmi(V2NmiIo &raw, std::uint32_t *chip_id) {
    if (!chip_id)
        return V2NmiResult::InvalidArgument;
    CheckedIo io(raw);
    std::uint32_t id = 0;
    if (!io.read(0x3fc, id, 4))
        return V2NmiResult::Failed;
    *chip_id = id;
    const auto family = v2_nmi_family(id);
    if (family == V2NmiFamily::Unsupported)
        return V2NmiResult::UnsupportedChip;
    struct RfPair {
        std::uint8_t reg, value;
    };
    static constexpr RfPair common[] = {
        {0x06, 0x48}, {0x07, 0x40}, {0x0a, 0xeb}, {0x0b, 0x11}, {0x0c, 0x10}, {0x0d, 0x88},
        {0x10, 0x04}, {0x11, 0x30}, {0x12, 0x30}, {0x15, 0xaa}, {0x16, 0x03}, {0x17, 0x80},
        {0x18, 0x67}, {0x19, 0xd4}, {0x1a, 0x44}, {0x1c, 0x10}, {0x1d, 0xee}, {0x1e, 0x99},
        {0x21, 0xc5}, {0x22, 0x91}, {0x24, 0x01}, {0x2b, 0x91}, {0x2d, 0x01}, {0x2f, 0x80},
        {0x31, 0x00}, {0x33, 0x00}, {0x38, 0x00}, {0x39, 0x2f}, {0x3a, 0x00}, {0x3b, 0x00}};
    for (const auto &rv : common)
        if (!io.write(rv.reg, rv.value, 1))
            return V2NmiResult::Failed;
    std::uint32_t value = 0;
    // config+4 (LDO bypass) is zero in the official 0x1c1b8 path.
    // The vendor RF writer also enforces bit7=0 on every later RF36 write.
    if (!io.read(0x36, value, 1) || !io.write(0x36, value & 0x7fU, 1) ||
        !io.write(0x164, 0x00000800U, 4) || !io.write(0x1c0, 0x2d8c19c7U, 4))
        return V2NmiResult::Failed;
    static constexpr RfPair nm120[] = {{0x0e, 0x45}, {0x1b, 0x0e}, {0x23, 0xff},
                                       {0x26, 0x82}, {0x28, 0x00}, {0x30, 0xdf},
                                       {0x32, 0xdf}, {0x34, 0x68}, {0x35, 0x18}};
    static constexpr RfPair nm130[] = {{0x26, 0x80}, {0x27, 0x5f}, {0x28, 0x00}, {0x29, 0x5f},
                                       {0x34, 0x68}, {0x35, 0x54}, {0x36, 0x7c}};
    static constexpr RfPair extended[] = {{0x28, 0x00}, {0x2e, 0x56}, {0x34, 0x78}};
    if (family == V2NmiFamily::Nm120) {
        for (const auto &rv : nm120)
            if (!io.write(rv.reg, rv.value, 1))
                return V2NmiResult::Failed;
    } else if (family == V2NmiFamily::Nm130) {
        for (const auto &rv : nm130)
            if (!io.write(rv.reg, rv.value, 1))
                return V2NmiResult::Failed;
    } else if (family == V2NmiFamily::Extended813000) {
        for (const auto &rv : extended)
            if (!io.write(rv.reg, rv.value, 1))
                return V2NmiResult::Failed;
    } else {
        if (!io.write(0x28, 0x00, 1))
            return V2NmiResult::Failed;
    }
    // Original state caches these RF values at state+0x33/34/35.
    if (!io.read(0x00, value, 1) || !io.read(0x34, value, 1) || !io.read(0x35, value, 1))
        return V2NmiResult::Failed;
    // config+0x6c is zero: ltgain(enable=true) selects RF0A=FB.
    if (!io.write(0x0a, 0xfb, 1) || !io.read(0x3fc, value, 4))
        return V2NmiResult::Failed;
    return raw.healthy() ? V2NmiResult::Completed : V2NmiResult::Failed;
}

V2NmiResult tune_v2_nmi(V2NmiIo &raw, U hz, U chip_id) {
    const auto family = v2_nmi_family(chip_id);
    if (family == V2NmiFamily::Unsupported)
        return V2NmiResult::UnsupportedChip;
    if (!hz || hz > std::numeric_limits<U>::max() - 1000U)
        return V2NmiResult::InvalidArgument;
    CheckedIo io(raw);
    if (!io.demod(1, 0x50) || !io.demod(0x47, 0x30) || !io.demod(0x25, 0) || !io.demod(0x20, 0) ||
        !io.demod(0x23, 0x4d))
        return V2NmiResult::Failed;
    U clock = 0;
    if (!tune_rf(io, hz, family, clock) || !tune_digital(io, hz, family, clock) || !io.wait(250) ||
        !io.demod(0x23, 0x4c) || !io.demod(1, 0x50) || !io.demod(0x71, 1) || !io.demod(0x72, 0x24))
        return V2NmiResult::Failed;
    return V2NmiResult::Completed;
}
} // namespace asicen
