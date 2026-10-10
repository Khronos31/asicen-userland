// SPDX-License-Identifier: GPL-2.0-only
#include "asicen/v2_nmi.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <map>
#include <vector>

using namespace asicen;
namespace {
#define CHECK(condition, message)                                                     \
    do {                                                                              \
        if (!(condition)) {                                                           \
            std::fprintf(stderr, "check failed at line %d: %s\n", __LINE__, message);  \
            return false;                                                             \
        }                                                                             \
    } while (false)
struct Event {
    char kind;
    std::uint16_t reg;
    std::uint32_t value;
    std::uint8_t width;
};
class Io final : public V2NmiIo {
  public:
    explicit Io(std::uint32_t id)
    {
        regs[0x3fc] = id;
        regs[0x36] = 0xff;
    }
    std::map<std::uint16_t, std::uint32_t> regs;
    std::vector<Event> events;
    std::size_t fail_at = 0;
    bool good = true;
    bool perform(Event event)
    {
        events.push_back(event);
        // Keep healthy() true on injected transfer failure: the implementation
        // must stop because this operation failed, not because of sticky mocks.
        return !fail_at || events.size() != fail_at;
    }
    bool read(std::uint16_t reg, std::uint32_t* value, std::uint8_t width) override
    {
        if (!perform({'r', reg, 0, width}))
            return false;
        *value = regs[reg];
        return true;
    }
    bool write(std::uint16_t reg, std::uint32_t value, std::uint8_t width) override
    {
        if (!perform({'w', reg, value, width}))
            return false;
        regs[reg] = value;
        return true;
    }
    bool demod_write(std::uint8_t reg, std::uint8_t value) override
    {
        return perform({'d', reg, value, 1});
    }
    bool delay_ms(unsigned ms) override
    {
        return perform({'t', 0, ms, 0});
    }
    bool healthy() const override
    {
        return good;
    }
};
std::size_t count(const Io& io, char kind, std::uint16_t reg = 0xffff)
{
    return std::count_if(io.events.begin(), io.events.end(), [=](const Event& e) {
        return e.kind == kind && (reg == 0xffff || e.reg == reg);
    });
}
bool initialization()
{
    constexpr std::uint32_t ids[] = {0x12001, 0x13001, 0x13101, 0x813001};
    constexpr std::size_t writes[] = {43, 41, 35, 37};
    for (unsigned f = 0; f != 4; ++f) {
        Io io(ids[f]);
        std::uint32_t id = 0;
        CHECK(initialize_v2_nmi(io, &id) == V2NmiResult::Completed, "init result");
        CHECK(id == ids[f], "init ID read");
        CHECK(count(io, 'r') == 6 && count(io, 'w') == writes[f], "init source trace count");
        CHECK(!count(io, 't') && !count(io, 'd'), "RF init has no demod/delay side effects");
        CHECK(io.events.front().kind == 'r' && io.events.front().reg == 0x3fc,
                "ID precedes writes");
        CHECK(io.events.back().kind == 'r' && io.events.back().reg == 0x3fc,
                "final ID verification read");
        CHECK(io.regs[0x06] == 0x48 && io.regs[0x21] == 0xc5 && io.regs[0x39] == 0x2f,
                "common RF defaults");
        CHECK(io.regs[0x164] == 0x800 && io.regs[0x1c0] == 0x2d8c19c7, "common digital defaults");
        CHECK(io.regs[0x0a] == 0xfb && !(io.regs[0x36] & 0x80), "ltgain and LDO");
        if (f == 0)
            CHECK(io.regs[0x23] == 0xff && io.regs[0x35] == 0x18, "NM120 defaults");
        if (f == 1)
            CHECK(io.regs[0x26] == 0x80 && io.regs[0x27] == 0x5f && io.regs[0x36] == 0x7c,
                    "NM130 defaults");
        if (f == 2)
            CHECK(io.regs[0x28] == 0 && !count(io, 'w', 0x34), "NM131 defaults");
        if (f == 3)
            CHECK(io.regs[0x2e] == 0x56 && io.regs[0x34] == 0x78, "extended defaults");
        for (std::size_t failure = 1; failure <= io.events.size(); ++failure) {
            Io broken(ids[f]);
            broken.fail_at = failure;
            CHECK(initialize_v2_nmi(broken, &id) == V2NmiResult::Failed,
                    "init injected IO failure");
            CHECK(broken.events.size() == failure, "init stops at failed IO");
        }
    }
    Io unknown(0x12345678);
    std::uint32_t id = 0;
    CHECK(initialize_v2_nmi(unknown, &id) == V2NmiResult::UnsupportedChip, "unknown ID rejected");
    CHECK(unknown.events.size() == 1 && !count(unknown, 'w'), "unknown ID does not write");
    Io null(0x12000);
    CHECK(initialize_v2_nmi(null, nullptr) == V2NmiResult::InvalidArgument && null.events.empty(),
            "null ID output");
    null.good = false;
    CHECK(initialize_v2_nmi(null, &id) == V2NmiResult::Failed && null.events.empty(),
            "unhealthy init no IO");
    CHECK(v2_nmi_family(0x131ff) == V2NmiFamily::Nm131 &&
                v2_nmi_family(0x8130ff) == V2NmiFamily::Extended813000 &&
                v2_nmi_family(0x813200) == V2NmiFamily::Unsupported,
            "exact source ID masks");
    return true;
}
bool tune_families()
{
    constexpr std::uint32_t ids[] = {0x12000, 0x13000, 0x13100, 0x813000};
    for (auto chip : ids) {
        Io io(chip);
        std::uint32_t id = 0;
        CHECK(initialize_v2_nmi(io, &id) == V2NmiResult::Completed, "fixture init");
        io.events.clear();
        CHECK(tune_v2_nmi(io, 473143000, chip) == V2NmiResult::Completed, "family tune");
        // Independently calculated from official 0x19944..0x19aa9. Effective
        // RF=473144000 after one retry, VCO=3785152, reference=24000,
        // integer=157, fraction=0x5b7a1, divider=17, clock=3407872.
        CHECK(io.regs[1] == 0x4e && io.regs[2] == 0x43 && io.regs[3] == 0x6f &&
                    io.regs[4] == 0x1b && io.regs[0x1d] == 0x2e,
                "473 MHz PLL fixture");
        CHECK(io.regs[0x230] == 0xbbca4 && io.regs[0x21c] == 0x08012656 &&
                    io.regs[0x210] == 0x37a3,
                "473 MHz digital numerical fixture");
        const bool older = chip == 0x12000 || chip == 0x13000;
        CHECK(io.regs[0x104] == (older ? 0x30a18821U : 0x32618821U), "per-family output enables");
        CHECK(io.regs[0x164] == (chip == 0x13000 ? 0x300U : 0x500U), "per-family BB bandwidth");
        if (chip == 0x12000)
            CHECK(io.regs[0x25] == 0xf4 && io.regs[0x27] == 0xef && io.regs[0x29] == 0x4f &&
                        io.regs[0x2e] == 0x34,
                    "NM120 UHF path");
        if (chip == 0x13000)
            CHECK(io.regs[0x0e] == 0x25 && io.regs[0x25] == 0x56 && io.regs[0x30] == 1 &&
                        io.regs[0x26] == 0x80,
                    "NM130 UHF path");
        if (!older)
            CHECK(io.regs[0x25] == 0xfa && io.regs[0x27] == 0x7f && io.regs[0x30] == 0xdf &&
                        io.regs[0x34] == 0x78 && io.regs[0x35] == 0x54,
                    "NM131/extended UHF gain path");
        CHECK(count(io, 'd') == 9 && count(io, 't') == 2, "demod tune bounded sequence");
        CHECK(io.events[0].kind == 'd' && io.events[0].reg == 1 && io.events[4].reg == 0x23 &&
                    io.events[4].value == 0x4d,
                "demod prepare prefix");
        const auto n = io.events.size();
        const std::size_t expected_events = chip == 0x12000   ? 66
                                            : chip == 0x13000 ? 71
                                            : chip == 0x13100 ? 74
                                                              : 72;
        CHECK(n == expected_events && count(io, 'w', 5) == 1,
                "source first-tune event count and RF05 cache");
        CHECK(io.events[n - 5].kind == 't' && io.events[n - 5].value == 250 &&
                    io.events[n - 4].reg == 0x23 && io.events[n - 4].value == 0x4c &&
                    io.events[n - 1].reg == 0x72 && io.events[n - 1].value == 0x24,
                "250 ms before acquire writes");
        CHECK(count(io, 'w', 0x104) == 2, "normal reset writes");
        for (std::size_t failure = 1; failure <= n; ++failure) {
            Io broken(chip);
            std::uint32_t init_id = 0;
            CHECK(initialize_v2_nmi(broken, &init_id) == V2NmiResult::Completed,
                    "fault fixture init");
            broken.events.clear();
            broken.fail_at = failure;
            CHECK(tune_v2_nmi(broken, 473143000, chip) == V2NmiResult::Failed,
                    "tune injected IO failure");
            CHECK(broken.events.size() == failure, "tune stops at failed operation");
        }
        io.events.clear();
        io.regs[0x328] = 1;
        CHECK(tune_v2_nmi(io, 473143000, chip) == V2NmiResult::Completed &&
                    count(io, 'w', 0x104) == 4,
                "one bounded status-triggered reset pulse");
    }
    return true;
}
bool frequency_edges()
{
    struct Fixture {
        std::uint32_t hz;
        std::uint8_t r1, r2, r3, r4, r5, r8, r9;
        std::uint32_t r230, r21c, r210;
    };
    constexpr Fixture fixtures[] = {
        {90143000, 0x5a, 0xa4, 0x9b, 0x44, 0x85, 0x52, 0x35, 0xbd882, 0x08012ef4, 0x3613},
        {120143000, 0x50, 0x6c, 0x12, 0x13, 0x05, 0x40, 0x34, 0xbadce, 0x080121cc, 0x3883},
        {770143000, 0x80, 0x42, 0x6f, 0xcb, 0x85, 0, 0, 0xbc7fd, 0x080129d8, 0x3703},
    };
    for (auto f : fixtures) {
        Io io(0x12000);
        std::uint32_t id = 0;
        initialize_v2_nmi(io, &id);
        CHECK(tune_v2_nmi(io, f.hz, id) == V2NmiResult::Completed, "frequency fixture tune");
        CHECK(io.regs[1] == f.r1 && io.regs[2] == f.r2 && io.regs[3] == f.r3 &&
                    io.regs[4] == f.r4 && io.regs[5] == f.r5 && io.regs[8] == f.r8 &&
                    io.regs[9] == f.r9,
                "VHF/UHF RF fixture");
        CHECK(io.regs[0x230] == f.r230 && io.regs[0x21c] == f.r21c && io.regs[0x210] == f.r210,
                "VHF/UHF digital fixture");
    }
    for (auto hz : {147000000U, 171000000U}) {
        Io io(0x13000);
        std::uint32_t id = 0;
        initialize_v2_nmi(io, &id);
        io.events.clear();
        CHECK(tune_v2_nmi(io, hz, id) == V2NmiResult::Completed, "NM130 reference exception");
        CHECK((io.regs[0x21] & 3) == (hz == 147000000U ? 3U : 2U) && io.regs[0x0e] == 0x45,
                "NM130 exact-frequency divider exception");
        CHECK(io.regs[1] == (hz == 147000000U ? 0xc4U : 0x39U) && io.regs[2] == 0 &&
                    io.regs[3] == 0 && count(io, 'w', 8) == 1,
                "NM130 integer PLL does not retry");
    }
    for (auto hz : {762000000U, 786000000U, 818000000U}) {
        Io io(0x13100);
        std::uint32_t id = 0;
        initialize_v2_nmi(io, &id);
        CHECK(tune_v2_nmi(io, hz, id) == V2NmiResult::Completed, "high-band threshold");
        CHECK(io.regs[0x27] == (hz >= 786000000U ? 0x3fU : 0x5fU) &&
                    io.regs[0x30] == (hz >= 818000000U ? 0xafU : 0xdfU),
                "high-band gain threshold fixture");
    }
    Io invalid(0x12000);
    CHECK(tune_v2_nmi(invalid, 0, 0x12000) == V2NmiResult::InvalidArgument &&
                invalid.events.empty(),
            "zero frequency no IO");
    CHECK(tune_v2_nmi(invalid, 0xffffffff, 0x12000) == V2NmiResult::InvalidArgument &&
                invalid.events.empty(),
            "overflow frequency no IO");
    CHECK(tune_v2_nmi(invalid, 473143000, 0x123400) == V2NmiResult::UnsupportedChip &&
                invalid.events.empty(),
            "unknown tune no IO");
    invalid.good = false;
    CHECK(tune_v2_nmi(invalid, 473143000, 0x12000) == V2NmiResult::Failed &&
                invalid.events.empty(),
            "unhealthy tune no IO");
    return true;
}
} // namespace
int main()
{
    if (!initialization() ||
        !tune_families() ||
        !frequency_edges()) {
        return 1;
    }
    return 0;
}
