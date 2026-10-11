#include "asicen/frontend_sequence.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
        transfers.push_back(transfer);
        for (std::uint16_t i = 0; i < transfer.length; ++i)
            data[i] = 0;
        if (transfer.length != 0)
            data[0] = 1;
        if (static_cast<int>(transfers.size()) - 1 == fail_at)
            return -1;
        if (static_cast<int>(transfers.size()) - 1 == short_at)
            return static_cast<int>(transfer.length) - 1;
        if (static_cast<int>(transfers.size()) - 1 == status_at && transfer.length != 0)
            data[0] = 0;
        return transfer.length;
    }
    void delay_ms(unsigned) override
    {
    }

    std::vector<asicen::ControlTransfer> transfers;
    int fail_at = -1;
    int short_at = -1;
    int status_at = -1;
};

constexpr std::array<std::uint8_t, 42> kExpectedRegs{
    0x01, 0x03, 0x04, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11,
    0x12, 0x13, 0x14, 0x15, 0x17, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x38, 0x39,
    0x3b, 0x51, 0x52, 0x53, 0x5a, 0x5b, 0x85, 0x87, 0x8d, 0x8e, 0xa3, 0xa4, 0xa5, 0xa6};
// Literal bytes read from the independently verified TunerControl.o
// .rodata 0x240..0x269 region (exactly 42 bytes; SHA-256
// 26956331982fce11b4f1b9abbfe5439fcd46f82dca1b2a1499c75f429d83e84f).
constexpr std::uint8_t kRawSatelliteValues[]{
    0x90, 0x00, 0x02, 0x00, 0x41, 0x00, 0x00, 0xff, 0x59, 0xf2, 0xf0, 0x50, 0xb2, 0x00,
    0x30, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x10,
    0x90, 0xb0, 0x89, 0xb3, 0x2d, 0xd3, 0x69, 0x04, 0x00, 0x00, 0x11, 0x00, 0x40, 0x04};
static_assert(sizeof(kRawSatelliteValues) / sizeof(kRawSatelliteValues[0]) == 42,
              "the bounded .rodata value slice contains exactly 42 bytes");

bool same_transfer(const asicen::ControlTransfer& a, const asicen::ControlTransfer& b)
{
    return a.request == b.request && a.value == b.value && a.index == b.index &&
           a.length == b.length && a.direction == b.direction;
}

bool source_table_and_i2c_encoding()
{
    const auto plan = asicen::plan_demod_init_satellite();
    CHECK(check(asicen::satellite_demod_init_count() == 42, "satellite table count"));
    CHECK(check(plan.size() == 42, "one I2C transfer per satellite register"));
    if (plan.size() != 42)
        return false;
    for (std::size_t i = 0; i < plan.size(); ++i) {
        CHECK(check(asicen::satellite_demod_init_reg(i) == kExpectedRegs[i],
                    "register ordering matches InitDemod SIG_SOURCE=1"));
        CHECK(check(asicen::satellite_demod_init_value(i) == kRawSatelliteValues[i],
                    "register value matches literal .rodata 0x240..0x269 byte"));
        const auto& op = plan[i];
        CHECK(check(op.kind == asicen::FrontendOpKind::Control && op.require_status,
                    "each satellite write requires success status"));
        CHECK(check(op.transfer.request == asicen::Request::I2cWrite &&
                        op.transfer.direction == asicen::Direction::In && op.transfer.length == 2,
                    "source I2C mode0 one-byte write encoding"));
        CHECK(check((op.transfer.value & 0xffU) == 0x32 &&
                        (op.transfer.value >> 8U) == kExpectedRegs[i] &&
                        (op.transfer.index & 0xffU) == kRawSatelliteValues[i],
                    "slave32/register/value encoding"));
    }
    return true;
}

bool shared_plan_preserves_default_and_inserts_in_source_order()
{
    const auto original = asicen::plan_terrestrial_init();
    const auto shared = asicen::plan_terrestrial_init_with_satellite_demod();
    const std::size_t insertion =
        asicen::plan_demod_init_terrestrial().size() + asicen::plan_fc0012_init().size();
    CHECK(check(original.size() == asicen::plan_demod_init_terrestrial().size() +
                                       asicen::plan_fc0012_init().size() + 1,
                "default terrestrial init retains original operation count"));
    CHECK(check(shared.size() == original.size() + 42,
                "shared option adds only the satellite demod table"));
    if (shared.size() != original.size() + 42 || insertion >= original.size())
        return false;
    for (std::size_t i = 0; i < insertion; ++i)
        CHECK(check(same_transfer(shared[i].transfer, original[i].transfer),
                    "shared init preserves original prefix"));
    const auto satellite = asicen::plan_demod_init_satellite();
    for (std::size_t i = 0; i < satellite.size(); ++i)
        CHECK(check(same_transfer(shared[insertion + i].transfer, satellite[i].transfer),
                    "satellite writes inserted after terrestrial RF init"));
    CHECK(check(same_transfer(shared.back().transfer, original.back().transfer),
                "existing demod 0f tail remains last"));
    return true;
}

bool status_failure_stops_sequence()
{
    const auto plan = asicen::plan_demod_init_satellite();
    FakeTransport transport;
    transport.status_at = 7;
    const auto result = asicen::run_frontend_plan(plan, &transport);
    CHECK(check(result == asicen::FrontendRunResult::FailedTransfer,
                "non-success I2C status fails satellite initialization"));
    CHECK(check(transport.transfers.size() == 8,
                "satellite initializer stops immediately on failed status"));

    FakeTransport short_transport;
    short_transport.short_at = 3;
    CHECK(check(asicen::run_frontend_plan(plan, &short_transport) ==
                    asicen::FrontendRunResult::ShortTransfer,
                "short satellite I2C write fails"));
    CHECK(check(short_transport.transfers.size() == 4,
                "short write stops later satellite register writes"));
    return true;
}

} // namespace

bool test_all()
{
    CHECK(source_table_and_i2c_encoding());
    CHECK(shared_plan_preserves_default_and_inserts_in_source_order());
    CHECK(status_failure_stops_sequence());
    std::cout << "shared demod tests passed\n";
    return true;
}

int main()
{
    return test_all() ? 0 : 1;
}
