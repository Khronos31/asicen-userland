// SPDX-License-Identifier: GPL-2.0-only
#ifndef ASICEN_DIAGNOSTIC_PROBE_H
#define ASICEN_DIAGNOSTIC_PROBE_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "asicen/hardware_stream_session.h"

namespace asicen {

enum class DiagnosticProbeRole { frontend, transport, card };

struct DiagnosticProbeArguments final {
    bool valid = false;
    bool help = false;
    std::string error;
    std::string base;
    std::string firmware;
    std::string output;
    std::array<int, 2U> descriptors{{-1, -1}};
    std::size_t descriptor_count = 0U;
    std::uint8_t device = 0U;
    std::uint8_t receiver = 0U;
    std::uint32_t frequency_khz = 0U;
    std::uint32_t seconds = 0U;
    std::uint8_t slot = 0U;
    std::uint32_t symbol_rate = 0U;
    std::uint32_t rolloff = 0U;
    std::uint32_t lnb_voltage = 0U;
    bool terrestrial = false;
    bool satellite = false;
    bool detect = false;
    bool atr = false;
    bool apdu_mode = false;
    bool repeat_specified = false;
    std::vector<std::uint8_t> apdu;
    std::uint32_t repeat = 1U;
};

// Same strict decimal, duplicate, help and mode contracts as the pinned PX4
// developer probes. ASICEN's --base identifies the primary USB topology,
// because these devices have no USB serial descriptor. --device remains the
// logical function index (1); --receiver uses the actual ASICEN lane map.
DiagnosticProbeArguments parse_diagnostic_probe_arguments(DiagnosticProbeRole role, int argc,
                                                          const char* const* argv);

// The entry owns claims, finite output/capture and all cleanup. It accepts
// warm runtime devices only: cold upload remains the explicit model-gated
// load-firmware operation, followed by a fresh address/fd grant.
int run_diagnostic_probe(DiagnosticProbeRole role, int argc, const char* const* argv);

// Injected, non-USB-specific operation/cleanup seam used by the production
// diagnostic entry and offline error-boundary tests alike.
int run_frontend_diagnostic(px4::userland::TunerServiceBackend& frontend,
                            StreamCaptureSource& source, DiagnosticProbeRole role,
                            const DiagnosticProbeArguments& args);

using TsProbeWrite = std::size_t (*)(void* context, const void* data,
                                     std::size_t size) noexcept;

inline constexpr std::size_t kTsProbeSelectedReceiverIndex = 1U;

struct TsProbeSinkCounters final {
    std::array<std::size_t, 4U> observed_packets{};
    std::size_t selected_packets = 0U;
    std::size_t selected_bytes = 0U;
    // Bytes reported by the write seam, including a partial failed write.
    std::size_t output_bytes = 0U;
};

struct TsProbeSink final {
    TsProbeWrite write = nullptr;
    void* write_context = nullptr;
    TsProbeSinkCounters counters;
    std::size_t selected_receiver = kTsProbeSelectedReceiverIndex;
};

// This is a decoded transport packet seam. Every valid packet is
// observed before selected filtering. A selected packet is counted only after
// an exact 188-byte write succeeds; non-selected packets are then ignored.
px4::userland::Result<void> write_ts_probe_packet(void* context, std::size_t receiver_index,
                                   px4::userland::ByteView packet) noexcept;

enum class TsProbeAcceptanceFailure : std::uint8_t {
    insufficient_selected_packets,
    sync_loss_events,
    unexpected_receiver_packets,
    selected_observed_packets_mismatch,
    selected_bytes_mismatch,
    output_bytes_mismatch,
    output_not_packet_aligned,
    framer_buffered_bytes,
};

struct TsProbeAcceptanceResult final {
    bool accepted = false;
    std::size_t required_packets = 0U;
    std::array<TsProbeAcceptanceFailure, 8U> failures{};
    std::size_t failure_count = 0U;
};

TsProbeAcceptanceResult evaluate_ts_probe_acceptance(
    std::uint32_t seconds, const TsProbeSinkCounters& sink,
    const StreamCaptureFramingCounters& demux,
    std::size_t selected_receiver = kTsProbeSelectedReceiverIndex) noexcept;

const char* ts_probe_acceptance_failure_string(TsProbeAcceptanceFailure failure) noexcept;

} // namespace asicen

#endif
