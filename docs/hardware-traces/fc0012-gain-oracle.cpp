// SPDX-License-Identifier: GPL-2.0-only
// Offline synthetic comparator for Fiti_LAN_Gain. Calls only the archived
// TunerControl.o code with test stubs for its unresolved TLIB bus functions.
#include "asicen/frontend_sequence.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

extern "C" unsigned char official_fiti(void*, unsigned char, void*)
    asm("_Z13Fiti_LAN_GainPvhS_");
extern "C" unsigned char oracle_write(void*, unsigned char, unsigned char,
                                      unsigned char*, unsigned char, unsigned char)
    asm("_Z14TLIB_I2C_WritePvhhPhhh");
extern "C" unsigned char oracle_read(void*, unsigned char, unsigned char,
                                     unsigned char*, unsigned char, unsigned char)
    asm("_Z13TLIB_I2C_ReadPvhhPhhh");
extern "C" void oracle_delay(unsigned long) asm("_Z10TLIB_Delaym");
extern "C" unsigned char oracle_gpio(void*, unsigned char, unsigned char)
    asm("_Z12TLIB_SetGPIOPvhh");
extern "C" unsigned char oracle_gpio_ex(void*, unsigned char, unsigned char)
    asm("_Z14TLIB_SetGPIOExPvhh");

namespace {

struct BusModel {
    std::vector<std::uint8_t> reads;
    std::size_t next = 0;
    std::uint8_t selected_reg = 0;
    std::vector<std::uint8_t> read_regs;
    std::vector<std::array<std::uint8_t, 2>> writes;
} *active = nullptr;

class FrontendModel final : public asicen::FrontendTransport {
public:
    explicit FrontendModel(const std::vector<std::uint8_t>& input) : reads(input) {}
    int control(const asicen::ControlTransfer& transfer, unsigned char* data) override {
        for (std::uint16_t i = 0; i < transfer.length; ++i) data[i] = 0;
        if (transfer.length) data[0] = 1;
        if (transfer.request == asicen::Request::I2cWrite &&
            static_cast<std::uint8_t>(transfer.value >> 8U) == 0xfeU &&
            static_cast<std::uint8_t>(transfer.index & 0xffU) == 0xc6U) {
            selected_reg = static_cast<std::uint8_t>(transfer.index >> 8U);
        }
        if (transfer.request == asicen::Request::I2cReadNoWait && transfer.length > 1) {
            if (next >= reads.size()) return -1;
            data[1] = reads[next++];
            read_regs.push_back(selected_reg);
        }
        if (transfer.request == asicen::Request::I2cBufferFill) {
            const std::size_t offset = transfer.value & 0xffU;
            if (offset < staged.size()) staged[offset] = static_cast<std::uint8_t>(transfer.value >> 8U);
            if (offset + 1 < staged.size()) staged[offset + 1] = static_cast<std::uint8_t>(transfer.index);
            if (offset + 2 < staged.size()) staged[offset + 2] = static_cast<std::uint8_t>(transfer.index >> 8U);
            staged_count = offset + (staged.size() - offset < 3 ? staged.size() - offset : 3);
        } else if (transfer.request == asicen::Request::I2cBufferSend) {
            if (staged_count == staged.size() && staged[0] == 0xfe && staged[1] == 0xc6)
                writes.push_back({staged[2], staged[3]});
            staged.fill(0);
            staged_count = 0;
        }
        return transfer.length;
    }
    void delay_ms(unsigned) override {}
    std::vector<std::uint8_t> reads;
    std::size_t next = 0;
    std::uint8_t selected_reg = 0;
    std::vector<std::uint8_t> read_regs;
    std::vector<std::array<std::uint8_t, 2>> writes;
private:
    std::array<std::uint8_t, 4> staged{};
    std::size_t staged_count = 0;
};

}  // namespace

extern "C" unsigned char oracle_write(void*, unsigned char, unsigned char reg,
                                      unsigned char* data, unsigned char len,
                                      unsigned char) {
    if (active == nullptr) return 0;
    if (reg == 0xfe && len >= 2 && data[0] == 0xc6) {
        active->selected_reg = data[1];
    } else if (reg == 0 && len >= 4 && data[0] == 0xfe && data[1] == 0xc6) {
        active->writes.push_back({data[2], data[3]});
    }
    return 1;
}

extern "C" unsigned char oracle_read(void*, unsigned char, unsigned char,
                                     unsigned char* data, unsigned char len,
                                     unsigned char) {
    if (active == nullptr || len == 0 || active->next >= active->reads.size()) return 0;
    active->read_regs.push_back(active->selected_reg);
    data[0] = active->reads[active->next++];
    return 1;
}

extern "C" void oracle_delay(unsigned long) {}
extern "C" unsigned char oracle_gpio(void*, unsigned char, unsigned char) { return 1; }
extern "C" unsigned char oracle_gpio_ex(void*, unsigned char, unsigned char) { return 1; }

int main() {
    struct Case { std::uint8_t mode, sample, d; std::vector<std::uint8_t> reads; };
    const std::array<Case, 12> cases{{
        {0x02, 251, 0x00, {251, 0x02, 0x00, 0x55}},
        {0x02, 252, 0xa2, {252, 0x02, 0xa2, 0x55, 0xa2, 9, 0xa2}},
        {0x0a, 240, 0x00, {240, 0x0a, 0x00, 0x55}},
        {0x0a, 239, 0x00, {239, 0x0a, 0x00, 0x55, 0x00, 0, 0x00}},
        {0x0a, 253, 0x00, {253, 0x0a, 0x00, 0x55, 0x00}},
        {0x14, 245, 0x00, {245, 0x14, 0x00, 0x55}},
        {0x14, 244, 0x00, {244, 0x14, 0x00, 0x55, 0x00, 0, 0x00}},
        {0x14, 252, 0x00, {252, 0x14, 0x00, 0x55, 0x00}},
        {0x10, 249, 0x00, {249, 0x10, 0x00, 0x55, 0x00}},
        {0x10, 250, 0x00, {250, 0x10, 0x00, 0x55}},
        {0x03, 249, 0x00, {249, 0x03, 0x00, 0x55, 0x00}},
        {0x02, 251, 0x10, {251, 0x02, 0x10, 0x55}},
    }};
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const Case& c = cases[i];
        BusModel vendor{};
        vendor.reads = c.reads;
        active = &vendor;
        alignas(8) std::array<std::uint8_t, 8> state{};
        const unsigned char vendor_rc = official_fiti(nullptr, 1, state.data());

        FrontendModel portable(c.reads);
        const auto portable_rc = asicen::run_frontend_plan(
            asicen::plan_fc0012_gain_once(1), &portable);
        if (vendor_rc != 1 || portable_rc != asicen::FrontendRunResult::Completed ||
            vendor.read_regs != portable.read_regs || vendor.writes != portable.writes) {
            std::cerr << "oracle mismatch case=" << i << '\n';
            return 1;
        }
    }
    active = nullptr;
    std::cout << "matched 12 synthetic Fiti_LAN_Gain cases\n";
    return 0;
}
