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

class TuneTransport final : public asicen::FrontendTransport {
  public:
    int control(const asicen::ControlTransfer& transfer, unsigned char* data) override
    {
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
        return transfer.length;
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

    std::vector<asicen::ControlTransfer> transfers;
    std::vector<unsigned> delays;
    std::uint8_t read_data = 0x40; // tuner reg 0x0e bit6 set -> one tune pass
    bool cancel = false;
    bool expire = false;
};

const asicen::ControlTransfer* find_gpio(const std::vector<asicen::ControlTransfer>& list,
                                         std::uint16_t value)
{
    for (const asicen::ControlTransfer& transfer : list) {
        if (transfer.request == asicen::Request::Gpio && transfer.value == value) {
            return &transfer;
        }
    }
    return nullptr;
}

} // namespace

bool test_all()
{
    // Recovered center-frequency transform (TunerControl.o .text 0x23bc).
    CHECK(check(asicen::terrestrial_tune_center_khz(473142U) == 473143U, "center T13"));
    CHECK(check(asicen::terrestrial_tune_center_khz(521142U) == 521143U, "center default"));
    CHECK(check(asicen::terrestrial_tune_center_khz(167000U) == 167143U, "center 165 case"));
    CHECK(check(asicen::terrestrial_tune_center_khz(195000U) == 195143U, "center 195 case"));
    CHECK(check(asicen::terrestrial_tune_center_khz(0U) == 0U, "center zero"));

    // Full tune plan exposes the deterministic TC_SetFrequency prefix.
    const asicen::FrontendPlan plan = asicen::plan_terrestrial_tune_full(473142U, 6);
    CHECK(check(plan.size() == 3, "full tune plan size"));
    CHECK(check(plan[0].kind == asicen::FrontendOpKind::Control &&
                    plan[0].transfer.value == 0x2530 && plan[0].transfer.index == 0x0000,
                "full tune demod 0x25 prefix"));
    CHECK(check(plan[1].kind == asicen::FrontendOpKind::Control &&
                    plan[1].transfer.value == 0x2330 && plan[1].transfer.index == 0x004d,
                "full tune demod 0x23 prefix"));
    CHECK(check(plan[2].kind == asicen::FrontendOpKind::TerrestrialTune &&
                    plan[2].frequency_khz == 473142U && plan[2].bandwidth_mhz == 6,
                "full tune composite op"));
    CHECK(check(asicen::plan_terrestrial_tune_full(0U, 6).empty(), "full tune rejects zero"));
    CHECK(check(asicen::plan_terrestrial_tune_full(473142U, 0).empty(), "full tune rejects bw0"));
    CHECK(check(asicen::plan_terrestrial_tune_full(1002000U, 6).empty(),
                "full tune rejects FC0012 out of band"));

    // Execute the full tune against a read-only double.
    TuneTransport transport;
    asicen::FrontendRunReport report{};
    const asicen::FrontendRunResult result = asicen::run_frontend_plan(plan, &transport, &report);
    CHECK(check(result == asicen::FrontendRunResult::Completed, "full tune completes"));
    CHECK(check(transport.transfers.size() > 6, "full tune emits transfers"));

    // No GPIO write clears the LNB bit 0x20.
    bool clears_lnb = false;
    for (const asicen::ControlTransfer& transfer : transport.transfers) {
        if (transfer.request != asicen::Request::Gpio) {
            continue;
        }
        const std::uint8_t value = static_cast<std::uint8_t>(transfer.value & 0xffU);
        const std::uint8_t mask = static_cast<std::uint8_t>((transfer.value >> 8U) & 0xffU);
        if ((mask & 0x20U) != 0U && (value & 0x20U) == 0U) {
            clears_lnb = true;
        }
    }
    CHECK(check(!clears_lnb, "full tune never clears LNB bit"));

    // The LNA control clears bit 0 (value 0x00, mask 0x01) and is the only GPIO.
    CHECK(check(find_gpio(transport.transfers, 0x0100) != nullptr, "full tune LNA off"));

    // Tail: demod 0x0f=0x34, ReAcqDemod 0x01=0x40, demod 0x23=0x4c.
    CHECK(check(transport.transfers.size() >= 3, "full tune tail present"));
    const std::size_t n = transport.transfers.size();
    CHECK(check(transport.transfers[n - 3].value == 0x0f30 &&
                    transport.transfers[n - 3].index == 0x0034,
                "full tune demod 0x0f tail"));
    CHECK(check(transport.transfers[n - 2].value == 0x0130 &&
                    transport.transfers[n - 2].index == 0x0040,
                "full tune ReAcqDemod tail"));
    CHECK(check(transport.transfers[n - 1].value == 0x2330 &&
                    transport.transfers[n - 1].index == 0x004c,
                "full tune demod 0x23 final"));

    // Cancellation and deadline are cooperative boundaries.
    {
        TuneTransport cancelled;
        cancelled.cancel = true;
        const asicen::FrontendRunResult r = asicen::run_frontend_plan(plan, &cancelled);
        CHECK(check(r == asicen::FrontendRunResult::Cancelled, "cancel boundary"));
        CHECK(check(cancelled.transfers.empty(), "cancel before first transfer"));
    }
    {
        TuneTransport expired;
        expired.expire = true;
        const asicen::FrontendRunResult r = asicen::run_frontend_plan(plan, &expired);
        CHECK(check(r == asicen::FrontendRunResult::DeadlineExceeded, "deadline boundary"));
        CHECK(check(expired.transfers.empty(), "deadline before first transfer"));
    }

    return true;
}

int main()
{
    return test_all() ? 0 : 1;
}
