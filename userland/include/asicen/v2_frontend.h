#ifndef ASICEN_USERLAND_V2_FRONTEND_H
#define ASICEN_USERLAND_V2_FRONTEND_H

// SPDX-License-Identifier: GPL-2.0-only
// Isolated PX-W3U3 V2 RF frontend. This is deliberately not a product/backend
// activation flag: USB master routing, controller and power ownership must be
// established separately before using it on hardware.

#include "asicen/frontend_sequence.h"
#include <cstdint>
#include <vector>

namespace asicen {

struct V2FrontendTarget {
    // Internal TC905xx source: even=terrestrial, odd=satellite (0..7). This is NOT a
    // USB lane or a topology-sorted receiver index. Resolve the device's
    // customer-info source selector before constructing this target.
    std::uint8_t internal_source = 0xff;
};

enum class V2FrontendResult : std::uint8_t {
    Completed,
    FailedTransfer,
    ShortTransfer,
    InvalidArgument,
    Cancelled,
    DeadlineExceeded,
    UnsupportedChip,
    NotLocked,
};

struct V2FrontendReport {
    std::size_t transfers_completed = 0;
    std::uint32_t chip_id = 0;
    bool chip_id_valid = false;
    bool locked = false;
};

struct V2Pll {
    bool valid = false;
    std::uint8_t lo_divider = 0;
    std::uint8_t reference_ratio = 0;
    std::uint8_t predivider = 0;
    std::uint8_t integer = 0;
    std::uint16_t fraction = 0;
};

bool v2_target_valid(V2FrontendTarget target);
bool v2_target_is_satellite(V2FrontendTarget target);
std::uint8_t v2_demod_slave(V2FrontendTarget target);
// API source order S,T,S,T to internal order T,S,T,S. Input >7 is rejected.
std::uint8_t v2_internal_source_from_api(std::uint8_t api_source);
struct V2SourceRoute {
    bool valid = false;
    std::uint8_t device_role = 0xff;
    std::uint8_t rf_master_role = 0xff;
    V2FrontendTarget target{};
};
// Validate the complete customer-info wire response; identity/board matching
// remains the owner's responsibility. Never use PID or enumeration order as
// a substitute for selecting the indicated shared RF-control master.
V2SourceRoute v2_source_route(const std::uint8_t* customer_info, std::size_t length,
                              std::uint8_t local_lane);
// Official V2 RF-kHz wrapper, including its terrestrial 143 kHz offset.
// Satellite input remains RF kHz (not IF), as with the original W3U3 API.
std::uint32_t v2_tune_frequency_hz(V2FrontendTarget target, std::uint32_t rf_khz);
V2Pll compute_v2_satellite_pll(std::uint32_t intermediate_khz, bool reg21_bit6 = true,
                               bool reg1b_bit0 = false);

// Pure wire plans. Writes use the ASICEN staging buffer; reads terminate in
// a checked read whose response is status byte + little-endian register data.
std::vector<ControlTransfer> v2_tuner_write_plan(V2FrontendTarget target, std::uint16_t reg,
                                                 std::uint32_t value, std::uint8_t width);
std::vector<ControlTransfer> v2_tuner_read_plan(V2FrontendTarget target, std::uint16_t reg,
                                                std::uint8_t width);
FrontendPlan plan_v2_demod_init(V2FrontendTarget target);
FrontendPlan plan_v2_select_tsid(V2FrontendTarget target, std::uint16_t tsid);
FrontendPlan plan_v2_tsids_read(V2FrontendTarget target);
// Full source-0 vendor GPIO sequence. Includes shared GPIO40 reset. Caller
// must own the enclosure and must not run this while a sibling is streaming.
// No LNB-enable write is included. Not automatically called by RF init.
FrontendPlan plan_v2_shared_power_on();
FrontendPlan plan_v2_shared_power_off();
// Revision11 DTV_Start surrounding the private a8:b0 board-identity read.
// Prefix includes shared reset lines; both USB functions must already be held.
FrontendPlan plan_v2_revision11_startup_prefix();
FrontendPlan plan_v2_revision11_startup_tail();

// RF-only operations: no firmware, link setup, card APDU, GPIO or LNB writes.
// The transport must serialize the whole call, including staging transactions.
// Cooperative transport deadlines plus an independent monotonic time budget
// apply even if the transport's default expired() implementation returns false.
V2FrontendResult initialize_v2_frontend(FrontendTransport* transport, V2FrontendTarget target,
                                        V2FrontendReport* report = nullptr,
                                        unsigned budget_ms = 3000);
V2FrontendResult tune_v2_frontend(FrontendTransport* transport, V2FrontendTarget target,
                                  std::uint32_t rf_khz, V2FrontendReport* report = nullptr,
                                  unsigned budget_ms = 3000);
V2FrontendResult read_v2_frontend_lock(FrontendTransport* transport, V2FrontendTarget target,
                                       bool* locked, V2FrontendReport* report = nullptr,
                                       unsigned budget_ms = 1000);
V2FrontendResult read_v2_frontend_tsids(FrontendTransport* transport, V2FrontendTarget target,
                                        std::array<std::uint16_t, 8>* tsids,
                                        V2FrontendReport* report = nullptr,
                                        unsigned budget_ms = 1000);
// The official V2 adapter checks write status; it does not read back a
// selected TSID here. This result therefore confirms the two acknowledged
// selector writes, not reception of a particular transport stream.
V2FrontendResult select_v2_frontend_tsid(FrontendTransport* transport, V2FrontendTarget target,
                                         std::uint16_t tsid, V2FrontendReport* report = nullptr,
                                         unsigned budget_ms = 1000);

} // namespace asicen

#endif // ASICEN_USERLAND_V2_FRONTEND_H
