// SPDX-License-Identifier: GPL-2.0-or-later

#include "asicen/satellite_tune.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include "asicen/write_protocol.h"

namespace asicen {
namespace {

// Sat_freq_mapping_list is 24 records of {RF-kHz LE32, 8 tuner bytes} at
// TunerControl.o .data+0, stride 12. Keep only the scalar protocol bytes;
// frequency spacing is represented by the source-verified BS/CS formulas.
constexpr std::array<std::array<std::uint8_t, 8>, 24> kTunerBytes{{
    {{0x28, 0x29, 0xe0, 0xd2, 0xe4, 0xf4, 0xd6, 0x48}},
    {{0x24, 0x40, 0xe0, 0xe2, 0xe4, 0xf4, 0xe6, 0x44}},
    {{0x24, 0x66, 0xe0, 0xe2, 0xe4, 0xf4, 0xe6, 0x44}},
    {{0x24, 0x8d, 0xe0, 0x20, 0xe4, 0xf4, 0x24, 0x44}},
    {{0x24, 0xb3, 0xe0, 0x20, 0xe4, 0xf4, 0x24, 0x44}},
    {{0x24, 0xd9, 0xe0, 0x20, 0xe4, 0xf4, 0x24, 0x44}},
    {{0x25, 0x00, 0xe0, 0x20, 0xe4, 0xf4, 0x24, 0x45}},
    {{0x25, 0x26, 0xe0, 0x40, 0xe4, 0xf4, 0x44, 0x45}},
    {{0x25, 0x4c, 0xe0, 0x40, 0xe4, 0xf4, 0x44, 0x45}},
    {{0x25, 0x73, 0xe0, 0x40, 0xe4, 0xf4, 0x44, 0x45}},
    {{0x25, 0x99, 0xe0, 0x40, 0xe4, 0xf4, 0x44, 0x45}},
    {{0x25, 0xbf, 0xe0, 0x60, 0xe4, 0xf4, 0x64, 0x45}},
    {{0x26, 0x4d, 0xe0, 0x60, 0xe4, 0xf4, 0x64, 0x46}},
    {{0x26, 0x75, 0xe0, 0x80, 0xe4, 0xf4, 0x84, 0x46}},
    {{0x26, 0x9d, 0xe0, 0x80, 0xe4, 0xf4, 0x84, 0x46}},
    {{0x26, 0xc5, 0xe0, 0x80, 0xe4, 0xf4, 0x84, 0x46}},
    {{0x26, 0xed, 0xe0, 0x80, 0xe4, 0xf4, 0x84, 0x46}},
    {{0x27, 0x15, 0xe0, 0xa0, 0xe4, 0xf4, 0xa4, 0x47}},
    {{0x27, 0x3d, 0xe0, 0xa0, 0xe4, 0xf4, 0xa4, 0x47}},
    {{0x27, 0x65, 0xe0, 0xa0, 0xe4, 0xf4, 0xa4, 0x47}},
    {{0x27, 0x8d, 0xe0, 0xa0, 0xe4, 0xf4, 0xa4, 0x47}},
    {{0x27, 0xb5, 0xe0, 0xc0, 0xe4, 0xf4, 0xc4, 0x47}},
    {{0x27, 0xdd, 0xe0, 0xc0, 0xe4, 0xf4, 0xc4, 0x47}},
    {{0x28, 0x05, 0xe0, 0xc0, 0xe4, 0xf4, 0xc4, 0x48}},
}};

constexpr std::uint32_t kBsFirstRfKhz = 11727480U;
constexpr std::uint32_t kBsStepKhz = 38360U;
constexpr std::uint32_t kCsFirstRfKhz = 12291000U;
constexpr std::uint32_t kCsStepKhz = 40000U;

std::uint32_t rf_for_row(std::size_t row) {
    if (row < 12) {
        return kBsFirstRfKhz + static_cast<std::uint32_t>(row) * kBsStepKhz;
    }
    return kCsFirstRfKhz + static_cast<std::uint32_t>(row - 12) * kCsStepKhz;
}

bool find_row(std::uint32_t rf_khz, std::size_t* row) {
    if (row == nullptr) return false;
    for (std::size_t i = 0; i < kTunerBytes.size(); ++i) {
        if (rf_for_row(i) == rf_khz) {
            *row = i;
            return true;
        }
    }
    return false;
}

void append_control_sequence(FrontendPlan* plan,
                             const std::vector<ControlTransfer>& transfers,
                             const char* label) {
    if (plan == nullptr) return;
    for (const ControlTransfer& transfer : transfers) {
        FrontendOp op{};
        op.kind = FrontendOpKind::Control;
        op.transfer = transfer;
        op.require_status = true;
        op.label = label;
        plan->push_back(op);
    }
}

bool append_i2c_write(FrontendPlan* plan, std::uint8_t slave,
                      std::uint8_t reg, const std::uint8_t* data,
                      std::size_t size, std::uint8_t mode,
                      const char* label) {
    if (plan == nullptr) return false;
    const auto transfers = build_i2c_write_sequence(slave, reg, data, size, mode);
    if (transfers.empty()) return false;
    append_control_sequence(plan, transfers, label);
    return true;
}

void append_delay(FrontendPlan* plan, unsigned delay_ms, const char* label) {
    FrontendOp op{};
    op.kind = FrontendOpKind::Delay;
    op.delay_ms = delay_ms;
    op.label = label;
    plan->push_back(op);
}

SatelliteOperationResult convert_result(FrontendRunResult result) {
    switch (result) {
        case FrontendRunResult::Completed:
            return SatelliteOperationResult::Completed;
        case FrontendRunResult::FailedTransfer:
            return SatelliteOperationResult::FailedTransfer;
        case FrontendRunResult::ShortTransfer:
            return SatelliteOperationResult::ShortTransfer;
        case FrontendRunResult::InvalidArgument:
            return SatelliteOperationResult::InvalidArgument;
        case FrontendRunResult::Cancelled:
            return SatelliteOperationResult::Cancelled;
        case FrontendRunResult::DeadlineExceeded:
            return SatelliteOperationResult::DeadlineExceeded;
    }
    return SatelliteOperationResult::FailedTransfer;
}

SatelliteOperationResult run_transfer(FrontendTransport* transport,
                                      const ControlTransfer& transfer,
                                      std::vector<unsigned char>* response) {
    if (transport == nullptr || response == nullptr || transfer.length == 0) {
        return SatelliteOperationResult::InvalidArgument;
    }
    if (transport->cancelled()) return SatelliteOperationResult::Cancelled;
    if (transport->expired()) return SatelliteOperationResult::DeadlineExceeded;

    response->assign(transfer.length, 0);
    const int rc = transport->control(transfer, response->data());
    if (rc < 0) return SatelliteOperationResult::FailedTransfer;
    if (rc != static_cast<int>(transfer.length)) {
        return SatelliteOperationResult::ShortTransfer;
    }
    if ((*response)[0] != 1U) return SatelliteOperationResult::FailedTransfer;
    return SatelliteOperationResult::Completed;
}

SatelliteOperationResult run_i2c_write(FrontendTransport* transport,
                                       std::uint8_t reg,
                                       const std::uint8_t* data,
                                       std::size_t size) {
    if (transport == nullptr || data == nullptr || size == 0) {
        return SatelliteOperationResult::InvalidArgument;
    }
    const auto transfers = build_i2c_write_sequence(0x32, reg, data, size, 0);
    if (transfers.empty()) return SatelliteOperationResult::InvalidArgument;
    for (const ControlTransfer& transfer : transfers) {
        std::vector<unsigned char> response;
        const SatelliteOperationResult result =
            run_transfer(transport, transfer, &response);
        if (result != SatelliteOperationResult::Completed) return result;
    }
    return SatelliteOperationResult::Completed;
}

SatelliteOperationResult run_i2c_read(FrontendTransport* transport,
                                      std::uint8_t reg,
                                      std::uint16_t length,
                                      std::uint8_t mode,
                                      std::vector<unsigned char>* data) {
    if (transport == nullptr || data == nullptr || length == 0) {
        return SatelliteOperationResult::InvalidArgument;
    }
    const ControlTransfer transfer = make_i2c_read(0x32, reg, length, mode);
    std::vector<unsigned char> response;
    const SatelliteOperationResult result =
        run_transfer(transport, transfer, &response);
    if (result != SatelliteOperationResult::Completed) return result;
    data->assign(response.begin() + 1, response.end());
    return SatelliteOperationResult::Completed;
}

}  // namespace

bool is_w3u3_satellite_rf_khz(std::uint32_t rf_khz) {
    std::size_t row = 0;
    return find_row(rf_khz, &row);
}

FrontendPlan plan_w3u3_satellite_tune(std::uint32_t rf_khz,
                                      std::uint16_t initial_tsid) {
    std::size_t row = 0;
    if (!find_row(rf_khz, &row) ||
        (row >= 12 && initial_tsid != kW3u3SatelliteNoTsid)) {
        return {};
    }

    FrontendPlan plan;
    const std::uint8_t demod_25 = 0x00;
    const std::uint8_t demod_23_start = 0x4d;
    const std::uint16_t tune_tsid = row >= 12 ? kW3u3SatelliteNoTsid : initial_tsid;
    const std::uint8_t tsid_bytes[] = {
        static_cast<std::uint8_t>(tune_tsid >> 8U),
        static_cast<std::uint8_t>(tune_tsid & 0xffU),
    };
    if (!append_i2c_write(&plan, 0x32, 0x25, &demod_25, 1, 0,
                          "satellite demod tune start") ||
        !append_i2c_write(&plan, 0x32, 0x23, &demod_23_start, 1, 0,
                          "satellite demod acquisition start") ||
        !append_i2c_write(&plan, 0x32, 0x8f, tsid_bytes, 2, 0,
                          "satellite initial TSID")) {
        return {};
    }

    const auto& tuner = kTunerBytes[row];
    const std::uint8_t tuner_message_1[] = {
        0xfe, 0xc0, tuner[0], tuner[1], tuner[2], tuner[3],
    };
    const std::uint8_t tuner_message_2[] = {0xfe, 0xc0, tuner[4]};
    const std::uint8_t tuner_message_3[] = {
        0xfe, 0xc0, tuner[5], tuner[6],
    };
    const std::uint8_t tuner_message_4[] = {0xfe, 0xc0, tuner[7]};
    append_control_sequence(
        &plan, build_i2c_write_sequence(0x32, 0, tuner_message_1,
                                        sizeof(tuner_message_1), 2),
        "satellite tuner row bytes 0-3");
    append_control_sequence(
        &plan, build_i2c_write_sequence(0x32, 0, tuner_message_2,
                                        sizeof(tuner_message_2), 2),
        "satellite tuner row byte 4");
    append_delay(&plan, 10, "satellite tuner settle");
    append_control_sequence(
        &plan, build_i2c_write_sequence(0x32, 0, tuner_message_3,
                                        sizeof(tuner_message_3), 2),
        "satellite tuner row bytes 5-6");
    append_control_sequence(
        &plan, build_i2c_write_sequence(0x32, 0, tuner_message_4,
                                        sizeof(tuner_message_4), 2),
        "satellite tuner row byte 7");

    const std::uint8_t terrestrial_adapter = 0x34;
    const std::uint8_t reacquire = 0x01;
    const std::uint8_t demod_23_finish = 0x4c;
    if (!append_i2c_write(&plan, 0x30, 0x0f, &terrestrial_adapter, 1, 0,
                          "satellite adapter finalize") ||
        !append_i2c_write(&plan, 0x32, 0x03, &reacquire, 1, 0,
                          "satellite reacquire") ||
        !append_i2c_write(&plan, 0x32, 0x23, &demod_23_finish, 1, 0,
                          "satellite tune complete")) {
        return {};
    }
    return plan;
}

SatelliteOperationResult run_w3u3_satellite_tune(
    FrontendTransport* transport, std::uint32_t rf_khz,
    std::uint16_t initial_tsid, FrontendRunReport* report) {
    if (transport == nullptr) return SatelliteOperationResult::InvalidArgument;
    const FrontendPlan plan = plan_w3u3_satellite_tune(rf_khz, initial_tsid);
    if (plan.empty()) return SatelliteOperationResult::InvalidArgument;
    return convert_result(run_frontend_plan(plan, transport, report));
}

SatelliteLockResult read_w3u3_satellite_lock(FrontendTransport* transport) {
    SatelliteLockResult result{};
    std::vector<unsigned char> data;
    result.result = run_i2c_read(transport, 0xc3, 1, 1, &data);
    if (result.result == SatelliteOperationResult::Completed) {
        result.locked = (data[0] & 0x10U) == 0U;
    }
    return result;
}

SatelliteLockResult poll_w3u3_satellite_lock(FrontendTransport* transport,
                                             std::size_t max_attempts,
                                             unsigned poll_interval_ms) {
    SatelliteLockResult result{};
    if (transport == nullptr || max_attempts == 0) {
        result.result = SatelliteOperationResult::InvalidArgument;
        return result;
    }
    for (std::size_t attempt = 0; attempt < max_attempts; ++attempt) {
        if (transport->cancelled()) {
            result.result = SatelliteOperationResult::Cancelled;
            return result;
        }
        if (transport->expired()) {
            result.result = SatelliteOperationResult::DeadlineExceeded;
            return result;
        }
        result = read_w3u3_satellite_lock(transport);
        if (result.result != SatelliteOperationResult::Completed || result.locked) {
            return result;
        }
        if (attempt + 1U == max_attempts) {
            result.result = SatelliteOperationResult::DeadlineExceeded;
            return result;
        }
        if (transport->cancelled()) {
            result.result = SatelliteOperationResult::Cancelled;
            return result;
        }
        if (transport->expired()) {
            result.result = SatelliteOperationResult::DeadlineExceeded;
            return result;
        }
        transport->delay_ms(poll_interval_ms);
    }
    result.result = SatelliteOperationResult::DeadlineExceeded;
    return result;
}

SatelliteTsidListResult read_w3u3_satellite_tsids(FrontendTransport* transport) {
    SatelliteTsidListResult result{};
    std::vector<unsigned char> data;
    result.result = run_i2c_read(transport, 0xce, 16, 1, &data);
    if (result.result != SatelliteOperationResult::Completed) return result;
    for (std::size_t i = 0; i < result.tsids.size(); ++i) {
        result.tsids[i] = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(data[i * 2]) << 8U) |
            static_cast<std::uint16_t>(data[i * 2 + 1]));
    }
    return result;
}

SatelliteTsidSelectResult select_w3u3_satellite_tsid(
    FrontendTransport* transport, std::size_t slot,
    const std::array<std::uint16_t, kW3u3SatelliteTsidSlots>& tsids) {
    SatelliteTsidSelectResult result{};
    if (transport == nullptr || slot >= tsids.size() ||
        tsids[slot] == kW3u3SatelliteNoTsid) {
        result.result = SatelliteOperationResult::InvalidArgument;
        return result;
    }
    const std::uint16_t selected = tsids[slot];
    const std::uint8_t bytes[] = {
        static_cast<std::uint8_t>(selected >> 8U),
        static_cast<std::uint8_t>(selected & 0xffU),
    };
    result.result = run_i2c_write(transport, 0x8f, bytes, sizeof(bytes));
    if (result.result != SatelliteOperationResult::Completed) return result;

    std::vector<unsigned char> readback;
    result.result = run_i2c_read(transport, 0x8f, 2, 0, &readback);
    if (result.result != SatelliteOperationResult::Completed) return result;
    const std::uint16_t observed = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(readback[0]) << 8U) |
        static_cast<std::uint16_t>(readback[1]));
    if (observed != selected) {
        result.result = SatelliteOperationResult::VerificationFailed;
        return result;
    }
    result.selected_tsid = selected;
    result.result = SatelliteOperationResult::Completed;
    return result;
}

const char* satellite_operation_result_name(SatelliteOperationResult result) noexcept {
    switch (result) {
        case SatelliteOperationResult::Completed: return "completed";
        case SatelliteOperationResult::InvalidArgument: return "invalid-argument";
        case SatelliteOperationResult::FailedTransfer: return "usb-failed";
        case SatelliteOperationResult::ShortTransfer: return "usb-short";
        case SatelliteOperationResult::Cancelled: return "cancelled";
        case SatelliteOperationResult::DeadlineExceeded: return "deadline";
        case SatelliteOperationResult::VerificationFailed: return "readback-mismatch";
    }
    return "unknown";
}

}  // namespace asicen
