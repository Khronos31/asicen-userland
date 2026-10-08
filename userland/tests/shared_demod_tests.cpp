#include "asicen/frontend_sequence.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

int failures = 0;

void check(bool value, const char* message) {
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

class FakeTransport final : public asicen::FrontendTransport {
public:
    int control(const asicen::ControlTransfer& transfer, unsigned char* data) override {
        transfers.push_back(transfer);
        for (std::uint16_t i = 0; i < transfer.length; ++i) data[i] = 0;
        if (transfer.length != 0) data[0] = 1;
        if (static_cast<int>(transfers.size()) - 1 == fail_at) return -1;
        if (static_cast<int>(transfers.size()) - 1 == short_at)
            return static_cast<int>(transfer.length) - 1;
        if (static_cast<int>(transfers.size()) - 1 == status_at && transfer.length != 0)
            data[0] = 0;
        return transfer.length;
    }
    void delay_ms(unsigned) override {}

    std::vector<asicen::ControlTransfer> transfers;
    int fail_at = -1;
    int short_at = -1;
    int status_at = -1;
};

constexpr std::array<std::uint8_t, 42> kExpectedRegs{
    0x01, 0x03, 0x04, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0c, 0x0d, 0x0e,
    0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x17, 0x1a, 0x1b, 0x1c,
    0x1d, 0x1e, 0x1f, 0x20, 0x38, 0x39, 0x3b, 0x51, 0x52, 0x53, 0x5a,
    0x5b, 0x85, 0x87, 0x8d, 0x8e, 0xa3, 0xa4, 0xa5, 0xa6};
constexpr std::array<std::uint8_t, 42> kExpectedValues{
    0x90, 0x00, 0x02, 0x00, 0x41, 0x00, 0x00, 0xff, 0x59, 0xf2, 0xf0,
    0x50, 0xb2, 0x00, 0x30, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x40, 0x10, 0x90, 0xb0, 0x89, 0xb3, 0x2d, 0xd3, 0x69, 0x04, 0x00,
    0x00, 0x11, 0x00, 0x40, 0x04, 0x00, 0x00, 0x00, 0x00};

bool same_transfer(const asicen::ControlTransfer& a,
                   const asicen::ControlTransfer& b) {
    return a.request == b.request && a.value == b.value && a.index == b.index &&
           a.length == b.length && a.direction == b.direction;
}

void source_table_and_i2c_encoding() {
    const auto plan = asicen::plan_demod_init_satellite();
    check(asicen::satellite_demod_init_count() == 42, "satellite table count");
    check(plan.size() == 42, "one I2C transfer per satellite register");
    if (plan.size() != 42) return;
    for (std::size_t i = 0; i < plan.size(); ++i) {
        check(asicen::satellite_demod_init_reg(i) == kExpectedRegs[i],
              "register ordering matches InitDemod SIG_SOURCE=1");
        check(asicen::satellite_demod_init_value(i) == kExpectedValues[i],
              "register values match InitDemod SIG_SOURCE=1");
        const auto& op = plan[i];
        check(op.kind == asicen::FrontendOpKind::Control && op.require_status,
              "each satellite write requires success status");
        check(op.transfer.request == asicen::Request::I2cWrite &&
                  op.transfer.direction == asicen::Direction::In &&
                  op.transfer.length == 2,
              "source I2C mode0 one-byte write encoding");
        check((op.transfer.value & 0xffU) == 0x32 &&
                  (op.transfer.value >> 8U) == kExpectedRegs[i] &&
                  (op.transfer.index & 0xffU) == kExpectedValues[i],
              "slave32/register/value encoding");
    }
}

void shared_plan_preserves_default_and_inserts_in_source_order() {
    const auto original = asicen::plan_terrestrial_init();
    const auto shared = asicen::plan_terrestrial_init_with_satellite_demod();
    const std::size_t insertion = asicen::plan_demod_init_terrestrial().size() +
                                  asicen::plan_fc0012_init().size();
    check(original.size() == asicen::plan_demod_init_terrestrial().size() +
                                  asicen::plan_fc0012_init().size() + 1,
          "default terrestrial init retains original operation count");
    check(shared.size() == original.size() + 42,
          "shared option adds only the satellite demod table");
    if (shared.size() != original.size() + 42 || insertion >= original.size()) return;
    for (std::size_t i = 0; i < insertion; ++i)
        check(same_transfer(shared[i].transfer, original[i].transfer),
              "shared init preserves original prefix");
    const auto satellite = asicen::plan_demod_init_satellite();
    for (std::size_t i = 0; i < satellite.size(); ++i)
        check(same_transfer(shared[insertion + i].transfer, satellite[i].transfer),
              "satellite writes inserted after terrestrial RF init");
    check(same_transfer(shared.back().transfer, original.back().transfer),
          "existing demod 0f tail remains last");
}

void status_failure_stops_sequence() {
    const auto plan = asicen::plan_demod_init_satellite();
    FakeTransport transport;
    transport.status_at = 7;
    const auto result = asicen::run_frontend_plan(plan, &transport);
    check(result == asicen::FrontendRunResult::FailedTransfer,
          "non-success I2C status fails satellite initialization");
    check(transport.transfers.size() == 8,
          "satellite initializer stops immediately on failed status");

    FakeTransport short_transport;
    short_transport.short_at = 3;
    check(asicen::run_frontend_plan(plan, &short_transport) ==
              asicen::FrontendRunResult::ShortTransfer,
          "short satellite I2C write fails");
    check(short_transport.transfers.size() == 4,
          "short write stops later satellite register writes");
}

}  // namespace

int main() {
    source_table_and_i2c_encoding();
    shared_plan_preserves_default_and_inserts_in_source_order();
    status_failure_stops_sequence();
    if (failures != 0) {
        std::cerr << failures << " shared demod test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "shared demod tests passed\n";
    return EXIT_SUCCESS;
}
