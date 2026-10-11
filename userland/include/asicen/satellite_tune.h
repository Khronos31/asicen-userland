#ifndef ASICEN_USERLAND_SATELLITE_TUNE_H
#define ASICEN_USERLAND_SATELLITE_TUNE_H

// SPDX-License-Identifier: GPL-2.0-or-later
//
// Bounded, transport-independent W3U3 satellite tune/lock/TSID operations.
// Scalar tuner facts were transcribed from Sat_freq_mapping_list in the
// saved PLEX W3U3 TunerControl.o (.data+0, 24 records, stride 12). Call order
// and register protocol are documented in
// docs/reverse-engineering/satellite-tune-facts-2026-10-09.md.

#include <array>
#include <cstddef>
#include <cstdint>

#include "asicen/frontend_sequence.h"

namespace asicen {

constexpr std::size_t kW3u3SatelliteTsidSlots = 8;
constexpr std::uint16_t kW3u3SatelliteNoTsid = 0xffffU;

enum class SatelliteOperationResult : std::uint8_t {
    Completed,
    InvalidArgument,
    FailedTransfer,
    ShortTransfer,
    Cancelled,
    DeadlineExceeded,
    VerificationFailed,
};

struct SatelliteLockResult {
    SatelliteOperationResult result = SatelliteOperationResult::InvalidArgument;
    bool locked = false;
};

struct SatelliteTsidListResult {
    SatelliteOperationResult result = SatelliteOperationResult::InvalidArgument;
    std::array<std::uint16_t, kW3u3SatelliteTsidSlots> tsids{};
};

struct SatelliteTsidReadyResult {
    SatelliteOperationResult result = SatelliteOperationResult::InvalidArgument;
    std::array<std::uint16_t, kW3u3SatelliteTsidSlots> tsids{};
    std::size_t slot = kW3u3SatelliteTsidSlots;
};

struct SatelliteTsidSelectResult {
    SatelliteOperationResult result = SatelliteOperationResult::InvalidArgument;
    std::uint16_t selected_tsid = kW3u3SatelliteNoTsid;
};

// Supported RF values are exact matches for the 24 source table rows.
bool is_w3u3_satellite_rf_khz(std::uint32_t rf_khz);

// PX4 satellite channel plans expose IF; the W3U3 tuner table contains RF.
// Only the 24 exact source rows are accepted.
bool w3u3_satellite_if_to_rf_khz(std::uint32_t if_khz, std::uint32_t* rf_khz) noexcept;

// Pure source-derived plan. An empty plan means the RF/TSID combination is not
// source-valid. BS rows accept the previously selected TSID; CS rows require
// the vendor's 0xffff initial state. The default is 0xffff for both bands.
FrontendPlan plan_w3u3_satellite_tune(std::uint32_t rf_khz,
                                      std::uint16_t initial_tsid = kW3u3SatelliteNoTsid);

SatelliteOperationResult run_w3u3_satellite_tune(FrontendTransport* transport, std::uint32_t rf_khz,
                                                 std::uint16_t initial_tsid = kW3u3SatelliteNoTsid,
                                                 FrontendRunReport* report = nullptr);

SatelliteLockResult read_w3u3_satellite_lock(FrontendTransport* transport);
SatelliteLockResult poll_w3u3_satellite_lock(FrontendTransport* transport,
                                             std::size_t max_attempts = 50,
                                             unsigned poll_interval_ms = 100);
SatelliteTsidListResult read_w3u3_satellite_tsids(FrontendTransport* transport);

// ASICEN policy: poll the read-only TSID list immediately, then at 10ms
// intervals until the requested nonempty entry appears or the transport's
// absolute deadline/cancellation fires. This is not claimed as vendor retry
// behavior; the source helper performs only one list read.
SatelliteTsidReadyResult wait_w3u3_satellite_slot_ready(FrontendTransport* transport,
                                                        std::size_t slot,
                                                        std::size_t max_attempts = 1000,
                                                        unsigned poll_interval_ms = 10);
SatelliteTsidReadyResult wait_w3u3_satellite_any_ready(FrontendTransport* transport,
                                                       std::size_t max_attempts = 1000,
                                                       unsigned poll_interval_ms = 10);
SatelliteTsidReadyResult wait_w3u3_satellite_tsid_ready(FrontendTransport* transport,
                                                        std::uint16_t tsid,
                                                        std::size_t max_attempts = 1000,
                                                        unsigned poll_interval_ms = 10);

// Select a value from a previously read eight-entry TSID list. Empty 0xffff
// entries and out-of-range slots are rejected before any write. The selected
// TSID is written to 0x8f and read back before success is reported.
SatelliteTsidSelectResult
select_w3u3_satellite_tsid(FrontendTransport* transport, std::size_t slot,
                           const std::array<std::uint16_t, kW3u3SatelliteTsidSlots>& tsids);

// S3U/S3U2 use the same 24 RF rows and index0 satellite demod32. The
// complete tune differs only in the model-specific terrestrial0f finalizer.
// These wrappers keep model validation explicit for lock/TSID operations too.
FrontendPlan plan_legacy_satellite_tune(LegacyFrontendProfile profile, std::uint32_t rf_khz,
                                        std::uint16_t initial_tsid = kW3u3SatelliteNoTsid);
SatelliteOperationResult
run_legacy_satellite_tune(LegacyFrontendProfile profile, FrontendTransport* transport,
                          std::uint32_t rf_khz, std::uint16_t initial_tsid = kW3u3SatelliteNoTsid,
                          FrontendRunReport* report = nullptr);
SatelliteLockResult read_legacy_satellite_lock(LegacyFrontendProfile profile,
                                               FrontendTransport* transport);
SatelliteLockResult poll_legacy_satellite_lock(LegacyFrontendProfile profile,
                                               FrontendTransport* transport,
                                               std::size_t max_attempts = 50,
                                               unsigned poll_interval_ms = 100);
SatelliteTsidListResult read_legacy_satellite_tsids(LegacyFrontendProfile profile,
                                                    FrontendTransport* transport);
SatelliteTsidReadyResult wait_legacy_satellite_slot_ready(LegacyFrontendProfile profile,
                                                          FrontendTransport* transport,
                                                          std::size_t slot,
                                                          std::size_t max_attempts = 1000,
                                                          unsigned poll_interval_ms = 10);
SatelliteTsidReadyResult wait_legacy_satellite_any_ready(LegacyFrontendProfile profile,
                                                         FrontendTransport* transport,
                                                         std::size_t max_attempts = 1000,
                                                         unsigned poll_interval_ms = 10);
SatelliteTsidReadyResult wait_legacy_satellite_tsid_ready(LegacyFrontendProfile profile,
                                                          FrontendTransport* transport,
                                                          std::uint16_t tsid,
                                                          std::size_t max_attempts = 1000,
                                                          unsigned poll_interval_ms = 10);
SatelliteTsidSelectResult
select_legacy_satellite_tsid(LegacyFrontendProfile profile, FrontendTransport* transport,
                             std::size_t slot,
                             const std::array<std::uint16_t, kW3u3SatelliteTsidSlots>& tsids);

const char* satellite_operation_result_name(SatelliteOperationResult result) noexcept;

} // namespace asicen

#endif // ASICEN_USERLAND_SATELLITE_TUNE_H
