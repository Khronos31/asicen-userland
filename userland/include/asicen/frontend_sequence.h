#ifndef ASICEN_USERLAND_FRONTEND_SEQUENCE_H
#define ASICEN_USERLAND_FRONTEND_SEQUENCE_H

// SPDX-License-Identifier: GPL-2.0-or-later
//
// Terrestrial frontend planning for the PX-W3U3.
//
// The FC0012 initialization/PLL arithmetic in the implementation is a
// reimplementation of the algorithm in Linux
//   drivers/media/tuners/fc0012.c
//   Copyright (C) 2012 Hans-Frieder Vogt <hfvogt@gmx.net>
//   SPDX-License-Identifier: GPL-2.0-or-later
// Source revision recorded before incorporation: Linux tag v6.6.
//
// Register/value facts were recovered read-only (ar/objdump) from the PLEX 1.0
// W3U3 vendor userspace archive
//   out/linux64/ReleaseToCustomer_64bit_130109_2/libPlexLib_W3U3.a
// member TunerControl.o. No vendor object code, crypto material or large vendor
// table is copied. The demodulator/FC0012 slave model and every request
// encoding used here are documented in docs/reverse-engineering/linux-abi.md
// and were re-derived from the vendor disassembly.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "asicen/protocol.h"

namespace asicen {

enum class PoweredInitResult : std::uint8_t {
    power_failed,
    controller_guard_failed,
    init_failed,
    completed,
};

// The controller behind the USB bridge is not guaranteed to answer until the
// source-verified startup/power steps have run. Keep its readiness check
// strictly between power and demod initialization.
template <typename PowerStep, typename ControllerGuard, typename InitStep>
PoweredInitResult execute_powered_init_sequence(PowerStep power, ControllerGuard guard,
                                                InitStep init)
{
    if (!power())
        return PoweredInitResult::power_failed;
    if (!guard())
        return PoweredInitResult::controller_guard_failed;
    if (!init())
        return PoweredInitResult::init_failed;
    return PoweredInitResult::completed;
}

// One planned frontend operation. Planning is pure and offline testable;
// execution is a separate concern (FrontendTransport) so tests can assert
// operation order, GPIO masks, I2C modes, exact response lengths and
// stop-on-failure without any hardware.
enum class FrontendOpKind : std::uint8_t {
    // Run a fully encoded control transfer in `transfer`.
    Control,
    // Wait `delay_ms`.
    Delay,
    // Read `reg` from `transfer`'s slave in `transfer`'s mode, apply the mask,
    // then write one byte back with I2C mode 0.
    I2cMask,
    // Read one tuner register through the demodulator bridge, apply the mask,
    // then write it back through the bridge.
    TunerMask,
    // FC0012 VCO calibration: tuner 0x0e pulse train plus the conditional
    // reg6 adjust recovered from Adpater_SetFreqISDBT.
    Fc0012VcoCalibrate,
    // Full terrestrial tune recovered from TC_SetFrequency (terrestrial
    // branch, DTV_SetTunerFreq defaults): demod 0x25/0x23, up to four
    // tuner-reg13/LNA/adapter/reg0e passes, demod 0x1c reset, FC0012 re-init,
    // then demod 0x0f, ReAcqDemod and demod 0x23=0x4c.
    TerrestrialTune,
    // One source-recovered terrestrial FC0012 feedback step (Fiti_LAN_Gain).
    Fc0012GainOnce,
    // USB_FilterReset(block_rmw=1): read the 0x45-byte CF block, set/clear
    // byte 0x40 bit2 from `flag` (the separate reset value), call ResetChannel,
    // then write back. `block_rmw` is the third argument; `flag` is the fourth.
    FilterReset,
};

struct FrontendOp {
    FrontendOpKind kind = FrontendOpKind::Control;
    ControlTransfer transfer{};
    bool require_status = false; // response byte 0 must equal 1
    unsigned delay_ms = 0;
    std::uint8_t local = 0;
    std::uint8_t source = 0;
    std::uint8_t reg = 0;
    std::uint8_t and_mask = 0xff;
    std::uint8_t or_mask = 0x00;
    std::uint8_t reg6 = 0;
    bool vco_select = false;
    bool block_rmw = false;
    std::uint32_t frequency_khz = 0;
    std::uint8_t bandwidth_mhz = 0;
    std::uint8_t flag = 0;
    const char* label = "";
};

using FrontendPlan = std::vector<FrontendOp>;

struct Fc0012Pll {
    std::uint8_t reg1 = 0;
    std::uint8_t reg2 = 0;
    std::uint8_t reg3 = 0;
    std::uint8_t reg4 = 0;
    std::uint8_t reg5 = 0;
    std::uint8_t reg6 = 0;
    bool vco_select = false;
    bool valid = false;
};

// Recovered small register/value facts (exact offsets in the block comment of
// frontend_sequence.cpp). Indexed accessors keep the tables out of the public
// ABI.
std::size_t terrestrial_demod_init_count();
std::uint8_t terrestrial_demod_init_reg(std::size_t index);
std::uint8_t terrestrial_demod_init_value(std::size_t index);
std::size_t satellite_demod_init_count();
std::uint8_t satellite_demod_init_reg(std::size_t index);
std::uint8_t satellite_demod_init_value(std::size_t index);
std::size_t fc0012_init_count();
std::uint8_t fc0012_init_reg(std::size_t index);
std::uint8_t fc0012_init_value(std::size_t index);

// Pure FC0012 PLL computation for a terrestrial RF center frequency in kHz.
Fc0012Pll compute_fc0012_pll(std::uint32_t freq_khz);

// Original single-function products have distinct board and tune wrappers.
// These APIs are pure source descriptions. Complete vendor power plans include
// GPIO20 and (S3U2) GPIOEx writes whose electrical effects remain hardware-
// unverified. They are NOT safe-no-LNB subsets; use model-specific ownership,
// restoration and runtime controller checks before executing them.
enum class LegacyFrontendProfile : std::uint8_t { S3u, S3u2 };

// Source DTV_Start board sequence AFTER a separate silicon-revision check.
// Ordinary11/52 includes the complete GPIO cold prefix and A8 probes before
// its final GPIO tail. The16/52 branch skips that prefix and finalGPIO40.
// Probe bytes are internal only; never print or persist identity material.
FrontendPlan plan_legacy_frontend_startup(LegacyFrontendProfile profile,
                                          bool silicon_16_52 = false);
// DTV_Start calls TC_PowerTunerDemod(off) for hardware indexes0 and1 after
// its GPIO tail. S3U acts twice; S3U2's index1 call is a no-op. In particular
// S3U2 must release GPIOEx01/02 high here before power-on drives them low.
FrontendPlan plan_legacy_frontend_startup_off(LegacyFrontendProfile profile);
// DTV_Init's board-power prelude, AFTER DTV_Start's GPIO tail and BEFORE
// TC_PowerTunerDemod(on). Both models clear GPIO08 and wait50ms here.
// Keep this separate from the isolated TC power sequence: S3U repeats its
// own GPIO08 clear, while S3U2 relies on this preceding DTV_Init operation.
FrontendPlan plan_legacy_frontend_init_prelude(LegacyFrontendProfile profile);
// Shared physical power sequence (TC_PowerTunerDemod, hardware index0).
FrontendPlan plan_legacy_frontend_power(LegacyFrontendProfile profile, bool on);
// Shared physical init, always hardware index0. S3U: S42,T13,RF21.
// S3U2: T13,RF21,RF10 pulse,S42,RF10 pulse,T0f=34.
FrontendPlan plan_legacy_frontend_init(LegacyFrontendProfile profile);
// Adapter only, center frequency already normalized. S3U includes its S-demod
// band write and omits the W3U3/S3U2 terrestrial-demod AGC tail.
FrontendPlan plan_legacy_fc0012_tune(LegacyFrontendProfile profile, std::uint32_t center_khz);
// Complete T prefix, one adapter acquisition and model-specific tail. No
// W3U3 LNA/GPIO01 operation or retry/reinit loop is added. Bandwidth6 only.
FrontendPlan plan_legacy_terrestrial_tune(LegacyFrontendProfile profile,
                                          std::uint32_t frequency_khz, std::uint8_t bandwidth_mhz);
FrontendPlan plan_legacy_terrestrial_lock(LegacyFrontendProfile profile,
                                          std::uint32_t frequency_khz);

// One bounded gain step for our default tune state. The caller must affirm
// source_default: the official DTV_SetTunerFreq clears cached field+8 before
// tuning. S3U2's zero-field branch writes13=0f; its nondefault cached-state
// feedback is deliberately not implemented by this API. S3U uses its shared
// feedback algorithm at hardware index0. No periodic worker is started.
FrontendPlan plan_legacy_default_gain(LegacyFrontendProfile profile, bool source_default);

// Planning. Invalid arguments produce an empty plan (callers treat that as a
// usage error before touching USB).

// Safe startup subset: the single GPIO write value 0x27 mask 0xBB (DTV_Start's
// value 0x27 mask 0xFB with the sibling bit 0x40 excluded). It never clears
// LNB bit 0x20 (0x20 is set) and never touches bit 0x40.
FrontendPlan plan_startup_subset();
FrontendPlan plan_safe_power_on(); // TC_PowerTunerDemod(local 0, power 1)
// Bounded raw multiplex setup. The one-argument form preserves the earlier
// research plan; callers requiring source-accurate reset behavior should pass
// the observed fourth-argument reset state explicitly.
FrontendPlan plan_stream_setup(std::uint8_t local);
FrontendPlan plan_stream_setup(std::uint8_t local, std::uint8_t reset_state);
FrontendPlan plan_sibling40_restore();
FrontendPlan plan_demod_read(std::uint8_t local, std::uint8_t reg, std::uint16_t length);
FrontendPlan plan_demod_init_terrestrial();
// Source-selected InitDemod SIG_SOURCE=1 register writes only (I2C slave 0x32).
FrontendPlan plan_demod_init_satellite();
FrontendPlan plan_fc0012_init();
// Demod table + FC0012 table + demod register 0x0f = 0x34 (TC_Initialise,
// terrestrial lane only).
FrontendPlan plan_terrestrial_init();
// Existing terrestrial init plus the source-proven satellite demod table,
// inserted after the terrestrial FC0012 initialization and before demod 0x0f.
FrontendPlan plan_terrestrial_init_with_satellite_demod();
// Adapter-only: FC0012 RSSI calibration + PLL + demod 0x1e AGC tweak
// (Adpater_SetFreqISDBT). This is NOT a complete terrestrial tune; use
// plan_terrestrial_tune_full for the TC_SetFrequency wrapper.
FrontendPlan plan_fc0012_tune(std::uint32_t freq_khz);
// Full terrestrial tune recovered from TC_SetFrequency (terrestrial branch)
// with the DTV_SetTunerFreq field defaults (ptr[8]=ptr[0x10]=ptr[0x18]=0).
FrontendPlan plan_terrestrial_tune_full(std::uint32_t freq_khz, std::uint8_t bandwidth_mhz);
FrontendPlan plan_terrestrial_lock_read(std::uint32_t freq_khz);
// One terrestrial feedback iteration. It performs the source-derived tuner
// reads and only the conditional FC0012 writes selected by those readings.
// Recovered from TunerControl.o Fiti_LAN_Gain (.text 0x13a0); this is an
// explicit diagnostic command, never an automatic capture/setup step.
FrontendPlan plan_fc0012_gain_once(std::uint8_t local, std::uint8_t source = 0);

enum class FrontendRunResult : std::uint8_t {
    Completed,
    FailedTransfer,
    ShortTransfer,
    InvalidArgument,
    Cancelled,
    DeadlineExceeded,
};

struct FrontendRunReport {
    std::size_t ops_completed = 0;
    std::uint8_t last_read = 0;
    bool have_last_read = false;
};

class FrontendTransport {
  public:
    virtual ~FrontendTransport() = default;
    virtual int control(const ControlTransfer& transfer, unsigned char* data) = 0;
    virtual void delay_ms(unsigned ms) = 0;
    // Optional cooperative cancellation/deadline. Defaults keep read-only
    // transports and offline test doubles unaffected.
    virtual bool cancelled() const
    {
        return false;
    }
    virtual bool expired() const
    {
        return false;
    }
};

// TC_SetFrequency terrestrial center-frequency transform (recovered at
// TunerControl.o .text 0x23bc-0x23f5 and 0x25c8/0x2788). Returns the value
// actually passed to Adpater_SetFreqISDBT.
std::uint32_t terrestrial_tune_center_khz(std::uint32_t freq_khz);

// Runs ops in order, stopping at the first libusb failure, short transfer or
// (where `require_status` is set) non-success status byte.
FrontendRunResult run_frontend_plan(const FrontendPlan& plan, FrontendTransport* transport,
                                    FrontendRunReport* report = nullptr);
FrontendRunResult run_filter_reset_operation(FrontendTransport* transport, std::uint8_t local,
                                             std::uint8_t reset_state,
                                             std::array<std::uint8_t, 0x45>* block_before = nullptr,
                                             std::array<std::uint8_t, 0x45>* block_after = nullptr);

// Offline target/argument parsing used by the diagnostic tool.
bool parse_usb_location(const std::string& text, std::uint8_t* bus, std::uint8_t* address);
bool parse_port_path(const std::string& text);

} // namespace asicen

#endif // ASICEN_USERLAND_FRONTEND_SEQUENCE_H
