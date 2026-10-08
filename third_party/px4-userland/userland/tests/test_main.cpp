// Modified/ported for px4-userland on 2026-09-03; MLT5 support added on 2026-09-24.
//
// Copyright (c) 2018-2021 nns779
// Derived from tsukumijima/px4_drv commit d748866f0da1cb3656106a520de4e9d7f073aacd (v0.6.1).
// Origin paths: driver/it930x.c, driver/itedtv_bus.c, driver/px4_usb.c,
// driver/px4_device.c, driver/pxmlt_device.c,
// winusb/src/DriverHost_PX4/itedtv_bus_winusb.c,
// winusb/src/DriverHost_PX4/px4_device.cpp.
// Source snapshot maintained by tsukumijima.
// SPDX-License-Identifier: GPL-2.0-only
#include "px4/error.h"
#include "px4/firmware.h"
#include "px4/identity.h"
#include "px4/it930x.h"
#include "px4/logging.h"
#include "px4/mock_transport.h"

#include "it930x_test_access.h"
#include "it930x_probe_args.h"
#include "q3u4_power.h"
#include "single_receiver_frontend.h"

#if PX4_ENABLE_LIBUSB
#include "px4/libusb_transport.h"
#include "libusb_transport_internal.h"
#include <libusb.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#if PX4_ENABLE_LIBUSB
#include <atomic>
#include <cerrno>
#endif
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#if PX4_ENABLE_LIBUSB
#include <thread>
#endif
#include <utility>
#include <vector>

bool run_frontend_tests();
bool run_card_tests();
bool run_card_service_tests();
bool run_it930x_card_tests();
bool run_ipc_tests();
bool run_ipc_state_tests();
bool run_px4d_args_tests();
bool run_px4d_signal_tests();
#if PX4_ENABLE_POSIX_IPC
bool run_control_integration_tests();
bool run_posix_ipc_tests();
bool run_posix_tuner_nonce_tests();
bool run_control_workers_tests();
bool run_px4_ts_tests();
bool run_px4ctl_format_tests();
bool run_px4d_list_format_tests();
#endif
bool run_q3u4_frontend_tests();
bool run_q3u4_card_backend_tests();
bool run_q3u4_power_tests();
bool run_q3u4_lnb_power_tests();
bool run_tuner_service_tests();
bool run_r850_tests();
bool run_rt710_tests();
bool run_frontend_probe_tests();
bool run_ts_probe_tests();
bool run_tagged_ts_demux_tests();
bool run_q3u4_stream_tests();
bool run_mlt5pe_tests();

#if PX4_ENABLE_LIBUSB && (defined(__linux__) || defined(__ANDROID__))
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

using namespace px4::userland;

constexpr CommandPacingOptions kFastPacing{CommandPacingMode::no_delay};

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            std::fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return false;                                                                   \
        }                                                                                   \
    } while (false)

class TemporaryFileCleanup final {
public:
    explicit TemporaryFileCleanup(std::filesystem::path path) noexcept
        : path_(std::move(path))
    {
    }

    ~TemporaryFileCleanup() noexcept
    {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    TemporaryFileCleanup(const TemporaryFileCleanup&) = delete;
    TemporaryFileCleanup& operator=(const TemporaryFileCleanup&) = delete;

private:
    std::filesystem::path path_;
};

#if !PX4_ENABLE_LIBUSB
static_assert(PX4_ENABLE_LIBUSB == 0);
#endif

bool test_error_contract()
{
    static_assert(static_cast<std::uint8_t>(Error::OK) == 0U);
    static_assert(static_cast<std::uint8_t>(Error::INVALID_ARGUMENT) == 1U);
    static_assert(static_cast<std::uint8_t>(Error::VERSION_MISMATCH) == 2U);
    static_assert(static_cast<std::uint8_t>(Error::NOT_FOUND) == 3U);
    static_assert(static_cast<std::uint8_t>(Error::BUSY) == 4U);
    static_assert(static_cast<std::uint8_t>(Error::NOT_READY) == 5U);
    static_assert(static_cast<std::uint8_t>(Error::TIMEOUT) == 6U);
    static_assert(static_cast<std::uint8_t>(Error::USB_IO) == 7U);
    static_assert(static_cast<std::uint8_t>(Error::DISCONNECTED) == 8U);
    static_assert(static_cast<std::uint8_t>(Error::PROTOCOL_ERROR) == 9U);
    static_assert(static_cast<std::uint8_t>(Error::FIRMWARE_REJECTED) == 10U);
    static_assert(static_cast<std::uint8_t>(Error::UNSUPPORTED) == 11U);
    static_assert(static_cast<std::uint8_t>(Error::NO_CARD) == 12U);
    static_assert(static_cast<std::uint8_t>(Error::CARD_REMOVED) == 13U);
    static_assert(static_cast<std::uint8_t>(Error::BUFFER_TOO_SMALL) == 14U);
    static_assert(static_cast<std::uint8_t>(Error::SLOW_CONSUMER) == 15U);
    static_assert(static_cast<std::uint8_t>(Error::INTERNAL) == 255U);

    constexpr std::array<const char*, 16> names{
        "OK", "INVALID_ARGUMENT", "VERSION_MISMATCH", "NOT_FOUND", "BUSY", "NOT_READY", "TIMEOUT",
        "USB_IO", "DISCONNECTED", "PROTOCOL_ERROR", "FIRMWARE_REJECTED", "UNSUPPORTED", "NO_CARD",
        "CARD_REMOVED", "BUFFER_TOO_SMALL", "SLOW_CONSUMER"};
    for (std::size_t index = 0U; index < names.size(); ++index) {
        CHECK(std::strcmp(error_string(static_cast<Error>(index)), names[index]) == 0);
    }
    CHECK(std::strcmp(error_string(Error::INTERNAL), "INTERNAL") == 0);
    CHECK(std::strcmp(error_string(static_cast<Error>(254U)), "UNKNOWN") == 0);
    return true;
}

std::uint16_t test_checksum(const std::uint8_t* data, std::size_t size)
{
    std::uint16_t sum = 0U;
    std::size_t index = 0U;
    while (index + 1U < size) {
        sum = static_cast<std::uint16_t>(
            sum + static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[index]) << 8U) |
                                              data[index + 1U]));
        index += 2U;
    }
    if (index < size) {
        sum = static_cast<std::uint16_t>(
            sum + static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[index]) << 8U));
    }
    return static_cast<std::uint16_t>(~sum);
}

std::vector<std::uint8_t> test_request(std::uint16_t command, std::uint8_t sequence,
                                       ByteView payload)
{
    std::vector<std::uint8_t> frame(payload.size + 6U, 0U);
    frame[0] = static_cast<std::uint8_t>(frame.size() - 1U);
    frame[1] = static_cast<std::uint8_t>(command >> 8U);
    frame[2] = static_cast<std::uint8_t>(command);
    frame[3] = sequence;
    if (payload.size != 0U) {
        std::memcpy(frame.data() + 4U, payload.data, payload.size);
    }
    const std::uint16_t sum = test_checksum(frame.data() + 1U, frame.size() - 3U);
    frame[frame.size() - 2U] = static_cast<std::uint8_t>(sum >> 8U);
    frame[frame.size() - 1U] = static_cast<std::uint8_t>(sum);
    return frame;
}

std::vector<std::uint8_t> test_response(std::uint8_t sequence, std::uint8_t status,
                                        ByteView payload)
{
    std::vector<std::uint8_t> frame(payload.size + 5U, 0U);
    frame[0] = static_cast<std::uint8_t>(frame.size() - 1U);
    frame[1] = sequence;
    frame[2] = status;
    if (payload.size != 0U) {
        std::memcpy(frame.data() + 3U, payload.data, payload.size);
    }
    const std::uint16_t sum = test_checksum(frame.data() + 1U, frame.size() - 3U);
    frame[frame.size() - 2U] = static_cast<std::uint8_t>(sum >> 8U);
    frame[frame.size() - 1U] = static_cast<std::uint8_t>(sum);
    return frame;
}

constexpr std::array<std::uint8_t, 9U> kSyntheticFirmwareImage{
    0x03U, 0x00U, 0x00U, 0x01U, 0x00U, 0x10U, 0x02U, 0xaaU, 0xbbU};

void expect_command(MockTransport& transport, std::uint16_t command, std::uint8_t sequence,
                    ByteView request_payload, ByteView response_payload,
                    std::uint8_t status = 0U,
                    MockOutcome write_outcome = MockOutcome::success,
                    MockOutcome read_outcome = MockOutcome::success)
{
    const auto request = test_request(command, sequence, request_payload);
    const auto response = test_response(sequence, status, response_payload);
    transport.expect_bulk_write(kCommandOutEndpoint,
                                ByteView{request.data(), request.size()}, write_outcome);
    if (write_outcome == MockOutcome::success) {
        transport.expect_bulk_read(kCommandInEndpoint,
                                   ByteView{response.data(), response.size()}, read_outcome);
    }
}

std::vector<std::uint8_t> register_payload(std::uint32_t reg,
                                            const std::vector<std::uint8_t>& values)
{
    const std::uint8_t register_size = (reg & 0xff000000U) != 0U
                                           ? 4U
                                           : (reg & 0x00ff0000U) != 0U
                                                 ? 3U
                                                 : (reg & 0x0000ff00U) != 0U ? 2U : 1U;
    std::vector<std::uint8_t> payload(6U + values.size(), 0U);
    payload[0] = static_cast<std::uint8_t>(values.size());
    payload[1] = register_size;
    payload[2] = static_cast<std::uint8_t>(reg >> 24U);
    payload[3] = static_cast<std::uint8_t>(reg >> 16U);
    payload[4] = static_cast<std::uint8_t>(reg >> 8U);
    payload[5] = static_cast<std::uint8_t>(reg);
    std::copy(values.begin(), values.end(), payload.begin() + 6U);
    return payload;
}

void expect_register_write(MockTransport& transport, std::uint8_t& sequence,
                           std::uint32_t reg, const std::vector<std::uint8_t>& values,
                           std::uint8_t status = 0U,
                           MockOutcome outcome = MockOutcome::success)
{
    const auto payload = register_payload(reg, values);
    expect_command(transport, 0x01U, sequence++, ByteView{payload.data(), payload.size()},
                   ByteView{nullptr, 0U}, status, outcome);
}

void expect_register_read(MockTransport& transport, std::uint8_t& sequence,
                          std::uint32_t reg, const std::vector<std::uint8_t>& values,
                          MockOutcome outcome = MockOutcome::success)
{
    const auto payload = register_payload(reg, {});
    auto read_payload = payload;
    read_payload[0] = static_cast<std::uint8_t>(values.size());
    expect_command(transport, 0x00U, sequence++,
                   ByteView{read_payload.data(), read_payload.size()},
                   ByteView{values.data(), values.size()}, 0U,
                   MockOutcome::success, outcome);
}

void expect_register_rmw(MockTransport& transport, std::uint8_t& sequence, std::uint32_t reg,
                          std::uint8_t current, std::uint8_t expected,
                          std::uint8_t read_status = 0U, std::uint8_t write_status = 0U,
                          MockOutcome read_outcome = MockOutcome::success,
                          MockOutcome write_outcome = MockOutcome::success)
{
    std::vector<std::uint8_t> read_payload(6U, 0U);
    read_payload[0] = 1U;
    read_payload[1] = (reg & 0xff000000U) != 0U
                          ? 4U
                          : (reg & 0x00ff0000U) != 0U
                                ? 3U
                                : (reg & 0x0000ff00U) != 0U ? 2U : 1U;
    read_payload[2] = static_cast<std::uint8_t>(reg >> 24U);
    read_payload[3] = static_cast<std::uint8_t>(reg >> 16U);
    read_payload[4] = static_cast<std::uint8_t>(reg >> 8U);
    read_payload[5] = static_cast<std::uint8_t>(reg);
    const std::array<std::uint8_t, 1U> current_value{current};
    expect_command(transport, 0x00U, sequence++,
                   ByteView{read_payload.data(), read_payload.size()},
                   ByteView{current_value.data(), current_value.size()}, read_status,
                   MockOutcome::success, read_outcome);
    if (read_status == 0U && read_outcome == MockOutcome::success) {
        expect_register_write(transport, sequence, reg, std::vector<std::uint8_t>{expected},
                              write_status, write_outcome);
    }
}

void expect_q3u4_readback(MockTransport& transport, std::uint8_t& sequence,
                          MockOutcome first_outcome = MockOutcome::success)
{
    expect_register_read(transport, sequence, 0xda1dU, {0U}, first_outcome);
    if (first_outcome != MockOutcome::success) {
        return;
    }
    expect_register_read(transport, sequence, 0xdd11U, {0x20U});
    expect_register_read(transport, sequence, 0xdd13U, {0U});
    expect_register_read(transport, sequence, 0xdd88U, {0xd0U, 0x95U});
    expect_register_read(transport, sequence, 0xdd0cU, {0x80U});
    expect_register_read(transport, sequence, 0xda05U, {0U});
    expect_register_read(transport, sequence, 0xda06U, {0U});
    expect_register_read(transport, sequence, 0xd920U, {0U});
    expect_register_read(transport, sequence, 0xd8b7U, {0U});
    expect_register_read(transport, sequence, 0xd8c3U, {1U});
    expect_register_read(transport, sequence, 0xd8d3U, {0U});
}

void expect_single_receiver_readback(MockTransport& transport, std::uint8_t& sequence)
{
    expect_register_read(transport, sequence, 0xda1dU, {0U});
    expect_register_read(transport, sequence, 0xdd11U, {0x20U});
    expect_register_read(transport, sequence, 0xdd13U, {0U});
    expect_register_read(transport, sequence, 0xdd88U, {0xd0U, 0x95U});
    expect_register_read(transport, sequence, 0xdd0cU, {0x80U});
    expect_register_read(transport, sequence, 0xda05U, {0U});
    expect_register_read(transport, sequence, 0xda06U, {0U});
    expect_register_read(transport, sequence, 0xd920U, {0U});
    expect_register_read(transport, sequence, 0xd8b7U, {0U});
    expect_register_read(transport, sequence, 0xd8b3U, {1U});
}

std::uint8_t expect_q3u4_warm_sequence(MockTransport& transport,
                                       std::uint8_t initial_sequence = 0U,
                                       bool include_readback = true)
{
    std::uint8_t sequence = initial_sequence;
    expect_register_write(transport, sequence, 0x4976U, {0U});
    expect_register_write(transport, sequence, 0x4bfBU, {0U});
    expect_register_write(transport, sequence, 0x4978U, {0U});
    expect_register_write(transport, sequence, 0x4977U, {0U});
    expect_register_write(transport, sequence, 0xda1aU, {0U});
    expect_register_rmw(transport, sequence, 0xf41fU, 0xa1U, 0xa5U);
    expect_register_rmw(transport, sequence, 0xda10U, 0xf3U, 0xf2U);
    expect_register_rmw(transport, sequence, 0xf41aU, 0xa0U, 0xa1U);
    expect_register_rmw(transport, sequence, 0xda1dU, 0x80U, 0x81U);
    expect_register_rmw(transport, sequence, 0xdd11U, 0xffU, 0xdfU);
    expect_register_rmw(transport, sequence, 0xdd13U, 0xffU, 0xdfU);
    expect_register_rmw(transport, sequence, 0xdd11U, 0xdfU, 0xffU);
    expect_register_write(transport, sequence, 0xdd88U, {0xd0U, 0x95U});
    expect_register_write(transport, sequence, 0xdd0cU, {0x80U});
    expect_register_rmw(transport, sequence, 0xda05U, 0x80U, 0x80U);
    expect_register_rmw(transport, sequence, 0xda06U, 0x01U, 0x00U);
    expect_register_rmw(transport, sequence, 0xda1dU, 0x81U, 0x80U);
    expect_register_write(transport, sequence, 0xd920U, {0U});
    expect_register_write(transport, sequence, 0xd833U, {1U});
    expect_register_write(transport, sequence, 0xd830U, {0U});
    expect_register_write(transport, sequence, 0xd831U, {1U});
    expect_register_write(transport, sequence, 0xd832U, {0U});
    expect_register_write(transport, sequence, 0xf6a7U, {0x07U});
    expect_register_write(transport, sequence, 0xf103U, {0x07U});
    expect_register_write(transport, sequence, 0x4975U, {0x22U});
    expect_register_write(transport, sequence, 0x4971U, {2U});
    expect_register_write(transport, sequence, 0x4974U, {0x26U});
    expect_register_write(transport, sequence, 0x4970U, {2U});
    expect_register_write(transport, sequence, 0x4973U, {0x20U});
    expect_register_write(transport, sequence, 0x496fU, {2U});
    expect_register_write(transport, sequence, 0x4972U, {0x24U});
    expect_register_write(transport, sequence, 0x496eU, {2U});
    expect_register_write(transport, sequence, 0xda59U, {0U});
    expect_register_write(transport, sequence, 0xda74U, {1U});
    expect_register_write(transport, sequence, 0xda79U, {0x17U});
    expect_register_write(transport, sequence, 0xda4dU, {1U});
    expect_register_write(transport, sequence, 0xda75U, {1U});
    expect_register_write(transport, sequence, 0xda7aU, {0x27U});
    expect_register_write(transport, sequence, 0xda4eU, {1U});
    expect_register_write(transport, sequence, 0xda76U, {1U});
    expect_register_write(transport, sequence, 0xda7bU, {0x37U});
    expect_register_write(transport, sequence, 0xda4fU, {1U});
    expect_register_write(transport, sequence, 0xda77U, {1U});
    expect_register_write(transport, sequence, 0xda7cU, {0x47U});
    expect_register_write(transport, sequence, 0xda50U, {1U});
    expect_register_write(transport, sequence, 0xda4cU, {0U});
    expect_register_write(transport, sequence, 0xd8c4U, {1U});
    expect_register_write(transport, sequence, 0xd8c5U, {1U});
    expect_register_write(transport, sequence, 0xd8b8U, {1U});
    expect_register_write(transport, sequence, 0xd8b9U, {1U});
    expect_register_write(transport, sequence, 0xd8b7U, {0U});
    expect_register_write(transport, sequence, 0xd8c3U, {1U});
    expect_register_write(transport, sequence, 0xd8d4U, {1U});
    expect_register_write(transport, sequence, 0xd8d5U, {1U});
    expect_register_write(transport, sequence, 0xd8d3U, {0U});
    if (include_readback) {
        expect_q3u4_readback(transport, sequence);
    }
    return sequence;
}

std::uint8_t expect_single_receiver_warm_sequence(MockTransport& transport,
                                                   DeviceModel model,
                                                   std::uint8_t initial_sequence = 0U,
                                                   bool include_readback = true)
{
    const std::uint8_t address = model == DeviceModel::dtv03a_1tu ? 0x18U : 0x10U;
    const std::uint8_t port = model == DeviceModel::dtv03a_1tu ? 4U : 0U;
    std::uint8_t sequence = initial_sequence;
    expect_register_write(transport, sequence, 0x4976U, {0U});
    expect_register_write(transport, sequence, 0x4bfBU, {0U});
    expect_register_write(transport, sequence, 0x4978U, {0U});
    expect_register_write(transport, sequence, 0x4977U, {0U});
    expect_register_write(transport, sequence, 0xda1aU, {0U});
    expect_register_rmw(transport, sequence, 0xf41fU, 0xa1U, 0xa5U);
    expect_register_rmw(transport, sequence, 0xda10U, 0xf3U, 0xf2U);
    expect_register_rmw(transport, sequence, 0xf41aU, 0xa0U, 0xa1U);
    expect_register_rmw(transport, sequence, 0xda1dU, 0x80U, 0x81U);
    expect_register_rmw(transport, sequence, 0xdd11U, 0xffU, 0xdfU);
    expect_register_rmw(transport, sequence, 0xdd13U, 0xffU, 0xdfU);
    expect_register_rmw(transport, sequence, 0xdd11U, 0xdfU, 0xffU);
    expect_register_write(transport, sequence, 0xdd88U, {0xd0U, 0x95U});
    expect_register_write(transport, sequence, 0xdd0cU, {0x80U});
    expect_register_rmw(transport, sequence, 0xda05U, 0x80U, 0x80U);
    expect_register_rmw(transport, sequence, 0xda06U, 0x01U, 0x00U);
    expect_register_rmw(transport, sequence, 0xda1dU, 0x81U, 0x80U);
    expect_register_write(transport, sequence, 0xd920U, {0U});
    expect_register_write(transport, sequence, 0xd833U, {1U});
    expect_register_write(transport, sequence, 0xd830U, {0U});
    expect_register_write(transport, sequence, 0xd831U, {1U});
    expect_register_write(transport, sequence, 0xd832U, {0U});
    expect_register_write(transport, sequence, 0xf6a7U, {0x07U});
    expect_register_write(transport, sequence, 0xf103U, {0x07U});
    expect_register_write(transport, sequence, 0x4975U,
                          {static_cast<std::uint8_t>(address << 1U)});
    expect_register_write(transport, sequence, 0x4971U, {3U});
    if (port < 2U) {
        expect_register_write(transport, sequence,
                              static_cast<std::uint32_t>(0xda58U + port), {0U});
    }
    expect_register_write(transport, sequence,
                          static_cast<std::uint32_t>(0xda73U + port), {1U});
    expect_register_write(transport, sequence,
                          static_cast<std::uint32_t>(0xda78U + port), {0x47U});
    expect_register_write(transport, sequence,
                          static_cast<std::uint32_t>(0xda4cU + port), {1U});
    expect_register_write(transport, sequence, 0xd8b4U, {1U});
    expect_register_write(transport, sequence, 0xd8b5U, {1U});
    expect_register_write(transport, sequence, 0xd8b3U, {1U});
    expect_register_write(transport, sequence, 0xd8b8U, {1U});
    expect_register_write(transport, sequence, 0xd8b9U, {1U});
    expect_register_write(transport, sequence, 0xd8b7U, {0U});
    if (include_readback) {
        expect_single_receiver_readback(transport, sequence);
    }
    return sequence;
}

std::uint8_t expect_single_receiver_cold_sequence(MockTransport& transport,
                                                   DeviceModel model,
                                                   std::uint8_t initial_sequence = 0U)
{
    constexpr std::array<std::uint8_t, 1U> query{1U};
    constexpr std::array<std::uint8_t, 4U> zero_version{0U, 0U, 0U, 0U};
    constexpr std::array<std::uint8_t, 4U> loaded_version{0U, 0U, 2U, 1U};
    const std::array<std::uint8_t, 7U> speed_payload{1U, 2U, 0U, 0U, 0xf1U, 0x03U, 0x07U};
    std::uint8_t sequence = initial_sequence;
    expect_command(transport, 0x22U, sequence++,
                   ByteView{query.data(), query.size()},
                   ByteView{zero_version.data(), zero_version.size()});
    expect_command(transport, 0x01U, sequence++,
                   ByteView{speed_payload.data(), speed_payload.size()},
                   ByteView{nullptr, 0U});
    expect_command(transport, 0x29U, sequence++,
                   ByteView{kSyntheticFirmwareImage.data(), kSyntheticFirmwareImage.size()},
                   ByteView{nullptr, 0U});
    expect_command(transport, 0x23U, sequence++,
                   ByteView{nullptr, 0U}, ByteView{nullptr, 0U});
    expect_command(transport, 0x22U, sequence++,
                   ByteView{query.data(), query.size()},
                   ByteView{loaded_version.data(), loaded_version.size()});
    return expect_single_receiver_warm_sequence(transport, model, sequence);
}

// px4_drv v0.6.1 pxmlt_device.c (PXMLT5PE_MODEL) and it930x.c: the common
// warm prefix, five demodulator slaves on I2C buses 3/1/1/3/3, five enabled
// serial TS ports tagged 0x17..0x57, then GPIO 7/2/11 idle in board order.
std::uint8_t expect_mlt5pe_warm_sequence(MockTransport& transport,
                                         std::uint8_t initial_sequence = 0U)
{
    std::uint8_t sequence = initial_sequence;
    expect_register_write(transport, sequence, 0x4976U, {0U});
    expect_register_write(transport, sequence, 0x4bfBU, {0U});
    expect_register_write(transport, sequence, 0x4978U, {0U});
    expect_register_write(transport, sequence, 0x4977U, {0U});
    expect_register_write(transport, sequence, 0xda1aU, {0U});
    expect_register_rmw(transport, sequence, 0xf41fU, 0xa1U, 0xa5U);
    expect_register_rmw(transport, sequence, 0xda10U, 0xf3U, 0xf2U);
    expect_register_rmw(transport, sequence, 0xf41aU, 0xa0U, 0xa1U);
    expect_register_rmw(transport, sequence, 0xda1dU, 0x80U, 0x81U);
    expect_register_rmw(transport, sequence, 0xdd11U, 0xffU, 0xdfU);
    expect_register_rmw(transport, sequence, 0xdd13U, 0xffU, 0xdfU);
    expect_register_rmw(transport, sequence, 0xdd11U, 0xdfU, 0xffU);
    expect_register_write(transport, sequence, 0xdd88U, {0xd0U, 0x95U});
    expect_register_write(transport, sequence, 0xdd0cU, {0x80U});
    expect_register_rmw(transport, sequence, 0xda05U, 0x80U, 0x80U);
    expect_register_rmw(transport, sequence, 0xda06U, 0x01U, 0x00U);
    expect_register_rmw(transport, sequence, 0xda1dU, 0x81U, 0x80U);
    expect_register_write(transport, sequence, 0xd920U, {0U});
    expect_register_write(transport, sequence, 0xd833U, {1U});
    expect_register_write(transport, sequence, 0xd830U, {0U});
    expect_register_write(transport, sequence, 0xd831U, {1U});
    expect_register_write(transport, sequence, 0xd832U, {0U});
    expect_register_write(transport, sequence, 0xf6a7U, {0x07U});
    expect_register_write(transport, sequence, 0xf103U, {0x07U});
    expect_register_write(transport, sequence, 0x4975U, {0xcaU});
    expect_register_write(transport, sequence, 0x4971U, {3U});
    expect_register_write(transport, sequence, 0x4974U, {0xd8U});
    expect_register_write(transport, sequence, 0x4970U, {1U});
    expect_register_write(transport, sequence, 0x4973U, {0xc8U});
    expect_register_write(transport, sequence, 0x496fU, {1U});
    expect_register_write(transport, sequence, 0x4972U, {0xd8U});
    expect_register_write(transport, sequence, 0x496eU, {3U});
    expect_register_write(transport, sequence, 0x4964U, {0xc8U});
    expect_register_write(transport, sequence, 0x4963U, {3U});
    expect_register_write(transport, sequence, 0xda58U, {0U});
    expect_register_write(transport, sequence, 0xda73U, {1U});
    expect_register_write(transport, sequence, 0xda78U, {0x17U});
    expect_register_write(transport, sequence, 0xda4cU, {1U});
    expect_register_write(transport, sequence, 0xda59U, {0U});
    expect_register_write(transport, sequence, 0xda74U, {1U});
    expect_register_write(transport, sequence, 0xda79U, {0x27U});
    expect_register_write(transport, sequence, 0xda4dU, {1U});
    expect_register_write(transport, sequence, 0xda75U, {1U});
    expect_register_write(transport, sequence, 0xda7aU, {0x37U});
    expect_register_write(transport, sequence, 0xda4eU, {1U});
    expect_register_write(transport, sequence, 0xda76U, {1U});
    expect_register_write(transport, sequence, 0xda7bU, {0x47U});
    expect_register_write(transport, sequence, 0xda4fU, {1U});
    expect_register_write(transport, sequence, 0xda77U, {1U});
    expect_register_write(transport, sequence, 0xda7cU, {0x57U});
    expect_register_write(transport, sequence, 0xda50U, {1U});
    expect_register_write(transport, sequence, 0xd8c4U, {1U});
    expect_register_write(transport, sequence, 0xd8c5U, {1U});
    expect_register_write(transport, sequence, 0xd8c3U, {1U});
    expect_register_write(transport, sequence, 0xd8b8U, {1U});
    expect_register_write(transport, sequence, 0xd8b9U, {1U});
    expect_register_write(transport, sequence, 0xd8b7U, {0U});
    expect_register_write(transport, sequence, 0xd8d4U, {1U});
    expect_register_write(transport, sequence, 0xd8d5U, {1U});
    expect_register_write(transport, sequence, 0xd8d3U, {0U});
    expect_q3u4_readback(transport, sequence);
    return sequence;
}

class RecordingQ3U4Delay final : public Q3U4Delay {
public:
    void sleep_ms(std::uint32_t milliseconds) noexcept override
    {
        sleeps.push_back(milliseconds);
    }

    std::vector<std::uint32_t> sleeps;
};

bool test_it930x_q3u4_warm_initialization()
{
    constexpr std::array<std::uint8_t, 4U> loaded_version{0U, 0U, 2U, 1U};
    constexpr std::array<std::uint8_t, 1U> query{1U};
    const auto image = FirmwareTestAccess::make_image(ByteView{kSyntheticFirmwareImage.data(),
                                                                kSyntheticFirmwareImage.size()});
    constexpr std::array<DeviceModel, 6U> q3_w3_models{{
        DeviceModel::px_q3u4, DeviceModel::px_q3pe4, DeviceModel::px_q3pe5,
        DeviceModel::px_w3u4, DeviceModel::px_w3pe4, DeviceModel::px_w3pe5}};
    for (const DeviceModel model : q3_w3_models) {
        CHECK(device_profile(model).supports_lnb_15v);
        MockTransport transport;
        expect_command(transport, 0x22U, 0U, ByteView{query.data(), query.size()},
                       ByteView{loaded_version.data(), loaded_version.size()});
        expect_q3u4_warm_sequence(transport, 1U);
        It930xController controller(transport, kFastPacing);
        const auto result = controller.initialize_q3u4(
            image, InitializationPolicy::accept_cold_or_warm, model);
        CHECK(result && result.value().already_loaded &&
              result.value().firmware_version == 0x00000201U && result.value().verified);
        CHECK(transport.remaining_expectations() == 0U);
        CHECK(transport.timeouts().size() == 154U);
        for (const Timeout timeout : transport.timeouts()) {
            CHECK(timeout.milliseconds == 3000U);
        }
    }
    MockTransport wrong_layout_transport;
    It930xController wrong_layout(wrong_layout_transport, kFastPacing);
    const auto invalid_model = wrong_layout.initialize_q3u4(
        image, InitializationPolicy::accept_cold_or_warm, DeviceModel::px_m1ur);
    CHECK(!invalid_model && invalid_model.error() == Error::UNSUPPORTED);
    CHECK(wrong_layout_transport.remaining_expectations() == 0U);
    return true;
}

bool test_it930x_mlt5pe_warm_initialization()
{
    MockTransport transport;
    constexpr std::array<std::uint8_t, 4U> loaded_version{0U, 0U, 2U, 1U};
    constexpr std::array<std::uint8_t, 1U> query{1U};
    expect_command(transport, 0x22U, 0U, ByteView{query.data(), query.size()},
                   ByteView{loaded_version.data(), loaded_version.size()});
    expect_mlt5pe_warm_sequence(transport, 1U);
    It930xController controller(transport, kFastPacing);
    const auto image = FirmwareTestAccess::make_image(ByteView{kSyntheticFirmwareImage.data(),
                                                                kSyntheticFirmwareImage.size()});
    const auto result = controller.initialize_mlt5pe(image);
    CHECK(result && result.value().already_loaded && result.value().verified);
    CHECK(transport.remaining_expectations() == 0U);
    return true;
}

bool test_it930x_q3u4_power_state_after_initialization()
{
    constexpr std::array<std::uint8_t, 4U> loaded_version{0U, 0U, 2U, 1U};
    constexpr std::array<std::uint8_t, 1U> query{1U};
    const auto image = FirmwareTestAccess::make_image(
        ByteView{kSyntheticFirmwareImage.data(), kSyntheticFirmwareImage.size()});

    MockTransport successful;
    std::uint8_t sequence = 0U;
    expect_register_write(successful, sequence, 0xd8c3U, {0U});
    expect_register_write(successful, sequence, 0xd8b7U, {1U});
    expect_command(successful, 0x22U, sequence++, ByteView{query.data(), query.size()},
                   ByteView{loaded_version.data(), loaded_version.size()});
    sequence = expect_q3u4_warm_sequence(successful, sequence);
    expect_register_write(successful, sequence, 0xd8c3U, {0U});
    expect_register_write(successful, sequence, 0xd8b7U, {1U});
    RecordingQ3U4Delay successful_delay;
    It930xController successful_controller(successful, kFastPacing);
    CHECK(successful_controller.set_q3u4_backend_power(true, successful_delay));
    CHECK(successful_controller.initialize_q3u4(image));
    CHECK(successful_controller.set_q3u4_backend_power(true, successful_delay));
    CHECK((successful_delay.sleeps == std::vector<std::uint32_t>{80U, 20U, 80U, 20U}));
    CHECK(successful.remaining_expectations() == 0U);

    MockTransport failed;
    sequence = 0U;
    expect_register_write(failed, sequence, 0xd8b7U, {0U});
    expect_register_write(failed, sequence, 0xd8c3U, {1U});
    expect_command(failed, 0x22U, sequence++, ByteView{query.data(), query.size()},
                   ByteView{loaded_version.data(), loaded_version.size()}, 0U,
                   MockOutcome::success, MockOutcome::timeout);
    expect_register_write(failed, sequence, 0xd8b7U, {0U});
    expect_register_write(failed, sequence, 0xd8c3U, {1U});
    RecordingQ3U4Delay failed_delay;
    It930xController failed_controller(failed, kFastPacing);
    CHECK(failed_controller.set_q3u4_backend_power(false, failed_delay));
    const auto failed_init = failed_controller.initialize_q3u4(image);
    CHECK(!failed_init && failed_init.error() == Error::TIMEOUT);
    CHECK(failed_controller.set_q3u4_backend_power(false, failed_delay));
    CHECK(failed.remaining_expectations() == 0U);
    return true;
}

bool test_it930x_q3u4_lnb_gpio_authority()
{
    {
        MockTransport transport;
        std::uint8_t sequence = 0U;
        expect_register_write(transport, sequence, 0xd8d3U, {1U});
        expect_register_write(transport, sequence, 0xd8d3U, {0U});
        It930xController controller(transport, kFastPacing);
        CHECK(controller.set_q3u4_lnb_power(true));
        CHECK(controller.set_q3u4_lnb_power(false));
        CHECK(transport.remaining_expectations() == 0U);
    }
    {
        MockTransport transport;
        std::uint8_t sequence = 0U;
        expect_register_write(transport, sequence, 0xd8d3U, {1U}, 0U,
                              MockOutcome::disconnect);
        It930xController controller(transport, kFastPacing);
        const auto disconnected = controller.set_q3u4_lnb_power(true);
        CHECK(!disconnected && disconnected.error() == Error::DISCONNECTED);
        // Terminal transport loss suppresses every later GPIO 11 write.
        const auto cleanup = controller.set_q3u4_lnb_power(false);
        CHECK(!cleanup && cleanup.error() == Error::DISCONNECTED);
        CHECK(transport.remaining_expectations() == 0U);
    }
    return true;
}

namespace {

class NoopSingleReceiverBridge final : public BridgeI2cMaster {
public:
    Result<void> request(BridgeI2cRequest*, std::size_t) noexcept override
    {
        return Result<void>::success();
    }
};

class NoopSingleReceiverBackendPower final : public Q3U4BackendPower {
public:
    Result<void> set_backend_power(bool, Q3U4Delay&) noexcept override
    {
        return Result<void>::success();
    }
};

class NoopSingleReceiverDelay final : public Q3U4FrontendDelay {
public:
    void sleep_ms(std::uint32_t) noexcept override {}
};

}  // namespace

bool test_single_receiver_lnb_15v_rejected()
{
    NoopSingleReceiverBridge bridge;
    NoopSingleReceiverBackendPower power;
    NoopSingleReceiverDelay delay;
    constexpr std::array<DeviceModel, 5U> single_receiver_models{{
        DeviceModel::px_m1ur,
        DeviceModel::px_s1ur,
        DeviceModel::dtv03a_1tu,
        DeviceModel::dtv02_1t1s_u,
        DeviceModel::dtv02a_1t1s_u}};
    for (const DeviceModel model : single_receiver_models) {
        for (const bool allow : {false, true}) {
            MockTransport transport;
            It930xController controller(transport, kFastPacing);
            SingleReceiverFrontend frontend(bridge, controller, power, delay,
                                            model, allow);
            CHECK(frontend.open_receiver(0U));
            const auto denied = frontend.begin_tune_power(
                0U, ipc::System::ISDB_S, 15U);
            CHECK(!denied && denied.error() == Error::UNSUPPORTED);
            CHECK(transport.remaining_expectations() == 0U);
            CHECK(frontend.close_receiver(0U));
        }
    }
    // 0V T/S paths remain accepted on the T/S-capable models; T-only models
    // continue to reject ISDB-S via receiver_supports.
    constexpr std::array<DeviceModel, 3U> ts_models{{
        DeviceModel::px_m1ur,
        DeviceModel::dtv02_1t1s_u,
        DeviceModel::dtv02a_1t1s_u}};
    for (const DeviceModel model : ts_models) {
        MockTransport transport;
        It930xController controller(transport, kFastPacing);
        SingleReceiverFrontend frontend(bridge, controller, power, delay,
                                        model, true);
        CHECK(frontend.open_receiver(0U));
        CHECK(frontend.begin_tune_power(0U, ipc::System::ISDB_S, 0U));
        CHECK(frontend.begin_tune_power(0U, ipc::System::ISDB_T, 0U));
        const auto invalid_t = frontend.begin_tune_power(
            0U, ipc::System::ISDB_T, 15U);
        CHECK(!invalid_t && invalid_t.error() == Error::INVALID_ARGUMENT);
        CHECK(transport.remaining_expectations() == 0U);
        CHECK(frontend.close_receiver(0U));
    }
    return true;
}

bool test_single_receiver_terrestrial_lock_settle()
{
    // px4_drv sets PTX_CHRDEV_WAIT_AFTER_LOCK_TC_T for ISDB2056, M1UR, and
    // S1UR (including ISDBT2071), which covers every one-receiver model.
    NoopSingleReceiverBridge bridge;
    NoopSingleReceiverBackendPower power;
    NoopSingleReceiverDelay delay;
    constexpr std::array<DeviceModel, 5U> single_receiver_models{{
        DeviceModel::px_m1ur,
        DeviceModel::px_s1ur,
        DeviceModel::dtv03a_1tu,
        DeviceModel::dtv02_1t1s_u,
        DeviceModel::dtv02a_1t1s_u}};
    for (const DeviceModel model : single_receiver_models) {
        MockTransport transport;
        It930xController controller(transport, kFastPacing);
        const SingleReceiverFrontend frontend(bridge, controller, power, delay, model);
        CHECK(frontend.requires_terrestrial_lock_settle());
    }
    return true;
}

bool test_single_receiver_warm_initialization_skips_gpio11()
{
    constexpr std::array<std::uint8_t, 4U> loaded_version{0U, 0U, 2U, 1U};
    constexpr std::array<std::uint8_t, 1U> query{1U};
    constexpr std::array<DeviceModel, 5U> single_receiver_models{{
        DeviceModel::px_m1ur,
        DeviceModel::px_s1ur,
        DeviceModel::dtv03a_1tu,
        DeviceModel::dtv02_1t1s_u,
        DeviceModel::dtv02a_1t1s_u}};
    const auto image = FirmwareTestAccess::make_image(
        ByteView{kSyntheticFirmwareImage.data(), kSyntheticFirmwareImage.size()});
    for (const DeviceModel model : single_receiver_models) {
        CHECK(!device_profile(model).supports_lnb_15v);
        MockTransport transport;
        expect_command(transport, 0x22U, 0U, ByteView{query.data(), query.size()},
                       ByteView{loaded_version.data(), loaded_version.size()});
        expect_single_receiver_warm_sequence(transport, model, 1U);
        It930xController controller(transport, kFastPacing);
        const auto result = controller.initialize_single_receiver(image, model);
        CHECK(result && result.value().already_loaded && result.value().verified);
        CHECK(transport.remaining_expectations() == 0U);
    }
    return true;
}

bool test_single_receiver_cold_initialization_skips_gpio11()
{
    constexpr std::array<DeviceModel, 5U> single_receiver_models{{
        DeviceModel::px_m1ur,
        DeviceModel::px_s1ur,
        DeviceModel::dtv03a_1tu,
        DeviceModel::dtv02_1t1s_u,
        DeviceModel::dtv02a_1t1s_u}};
    const auto image = FirmwareTestAccess::make_image(
        ByteView{kSyntheticFirmwareImage.data(), kSyntheticFirmwareImage.size()});
    for (const DeviceModel model : single_receiver_models) {
        CHECK(!device_profile(model).supports_lnb_15v);
        MockTransport transport;
        expect_single_receiver_cold_sequence(transport, model, 0U);
        It930xController controller(transport, kFastPacing);
        const auto result = controller.initialize_single_receiver(image, model);
        CHECK(result && !result.value().already_loaded && result.value().verified);
        CHECK(transport.remaining_expectations() == 0U);
    }
    return true;
}

void expect_q3u4_warm_until_stream_output_a(MockTransport& transport, std::uint8_t& sequence,
                                            MockOutcome output_a_write = MockOutcome::success)
{
    for (const auto item : std::array<std::uint32_t, 5U>{0x4976U, 0x4bfBU, 0x4978U,
                                                          0x4977U, 0xda1aU}) {
        expect_register_write(transport, sequence, item, {0U});
    }
    expect_register_rmw(transport, sequence, 0xf41fU, 0U, 0x04U);
    expect_register_rmw(transport, sequence, 0xda10U, 0U, 0U);
    expect_register_rmw(transport, sequence, 0xf41aU, 0U, 1U);
    expect_register_rmw(transport, sequence, 0xda1dU, 0U, 1U);
    expect_register_rmw(transport, sequence, 0xdd11U, 0U, 0U);
    expect_register_rmw(transport, sequence, 0xdd13U, 0U, 0U);
    expect_register_rmw(transport, sequence, 0xdd11U, 0U, 0x20U);
    expect_register_write(transport, sequence, 0xdd88U, {0xd0U, 0x95U});
    expect_register_write(transport, sequence, 0xdd0cU, {0x80U});
    expect_register_rmw(transport, sequence, 0xda05U, 0U, 0U,
                        0U, 0U, MockOutcome::success, output_a_write);
}

void expect_q3u4_stream_cleanup(MockTransport& transport, std::uint8_t& sequence,
                                MockOutcome gate_cleanup_write = MockOutcome::success,
                                MockOutcome reverse_cleanup = MockOutcome::success)
{
    expect_register_rmw(transport, sequence, 0xda1dU, 0x81U, 0x80U,
                        0U, 0U, MockOutcome::success, gate_cleanup_write);
    expect_register_write(transport, sequence, 0xd920U, {0U}, 0U, reverse_cleanup);
}

bool test_it930x_q3u4_warm_failure_and_gate_cleanup()
{
    constexpr std::array<std::uint8_t, 4U> loaded_version{0U, 0U, 2U, 1U};
    constexpr std::array<std::uint8_t, 1U> query{1U};
    const auto image = FirmwareTestAccess::make_image(ByteView{kSyntheticFirmwareImage.data(),
                                                                kSyntheticFirmwareImage.size()});

    MockTransport forward_failure;
    std::uint8_t sequence = 0U;
    expect_command(forward_failure, 0x22U, sequence++, ByteView{query.data(), query.size()},
                   ByteView{loaded_version.data(), loaded_version.size()});
    expect_q3u4_warm_until_stream_output_a(forward_failure, sequence, MockOutcome::timeout);
    expect_q3u4_stream_cleanup(forward_failure, sequence);
    It930xController forward_controller(forward_failure, kFastPacing);
    const auto forward_result = forward_controller.initialize_q3u4(image);
    CHECK(!forward_result && forward_result.error() == Error::TIMEOUT);
    CHECK(forward_failure.remaining_expectations() == 0U);

    MockTransport gate_cleanup_failure;
    sequence = 0U;
    expect_command(gate_cleanup_failure, 0x22U, sequence++,
                   ByteView{query.data(), query.size()},
                   ByteView{loaded_version.data(), loaded_version.size()});
    expect_q3u4_warm_until_stream_output_a(gate_cleanup_failure, sequence);
    expect_register_rmw(gate_cleanup_failure, sequence, 0xda06U, 0U, 0U);
    expect_q3u4_stream_cleanup(gate_cleanup_failure, sequence, MockOutcome::disconnect);
    It930xController gate_controller(gate_cleanup_failure, kFastPacing);
    const auto gate_result = gate_controller.initialize_q3u4(image);
    CHECK(!gate_result && gate_result.error() == Error::DISCONNECTED);
    CHECK(gate_cleanup_failure.remaining_expectations() == 0U);

    MockTransport reverse_cleanup_failure;
    sequence = 0U;
    expect_command(reverse_cleanup_failure, 0x22U, sequence++,
                   ByteView{query.data(), query.size()},
                   ByteView{loaded_version.data(), loaded_version.size()});
    expect_q3u4_warm_until_stream_output_a(reverse_cleanup_failure, sequence);
    expect_register_rmw(reverse_cleanup_failure, sequence, 0xda06U, 0U, 0U);
    expect_q3u4_stream_cleanup(reverse_cleanup_failure, sequence,
                               MockOutcome::success, MockOutcome::timeout);
    It930xController reverse_controller(reverse_cleanup_failure, kFastPacing);
    const auto reverse_result = reverse_controller.initialize_q3u4(image);
    CHECK(!reverse_result && reverse_result.error() == Error::TIMEOUT);
    CHECK(reverse_cleanup_failure.remaining_expectations() == 0U);
    return true;
}

bool test_it930x_probe_argument_parser()
{
    const char* query[] = {"px4-it930x-probe", "--base", "12345678901234", "--device", "1"};
    const auto parsed_query = parse_it930x_probe_arguments(5, query);
    CHECK(parsed_query.valid && !parsed_query.initialize && parsed_query.device == 1U);
    const char* initialize[] = {"px4-it930x-probe", "--initialize", "--firmware", "firmware.bin",
                                "--base", "12345678901234", "--device", "2"};
    const auto parsed_initialize = parse_it930x_probe_arguments(8, initialize);
    CHECK(parsed_initialize.valid && parsed_initialize.initialize && parsed_initialize.device == 2U);
    const char* explicit_options[] = {"px4-it930x-probe", "--initialize", "--firmware", "firmware.bin",
                                      "--require-cold", "--command-delay-ms", "0", "--base",
                                      "12345678901234", "--device", "1"};
    const auto parsed_options = parse_it930x_probe_arguments(11, explicit_options);
    CHECK(parsed_options.valid && parsed_options.require_cold &&
          parsed_options.pacing_mode == CommandPacingMode::no_delay);
    const char* explicit_one[] = {"px4-it930x-probe", "--base", "12345678901234", "--device", "1",
                                  "--command-delay-ms", "1"};
    const auto parsed_one = parse_it930x_probe_arguments(7, explicit_one);
    CHECK(parsed_one.valid &&
          parsed_one.pacing_mode == CommandPacingMode::linux_reference_1ms);
    const char* invalid_delay[] = {"px4-it930x-probe", "--base", "12345678901234", "--device", "1",
                                   "--command-delay-ms", "2"};
    CHECK(!parse_it930x_probe_arguments(7, invalid_delay).valid);
    const char* duplicate_delay[] = {"px4-it930x-probe", "--base", "12345678901234", "--device", "1",
                                     "--command-delay-ms", "0", "--command-delay-ms", "1"};
    CHECK(!parse_it930x_probe_arguments(9, duplicate_delay).valid);
    const char* require_cold_query[] = {"px4-it930x-probe", "--base", "12345678901234", "--device", "1",
                                        "--require-cold"};
    CHECK(!parse_it930x_probe_arguments(6, require_cold_query).valid);
    const char* firmware_without_initialize[] = {"px4-it930x-probe", "--firmware", "firmware.bin",
                                                 "--base", "12345678901234", "--device", "1"};
    CHECK(!parse_it930x_probe_arguments(7, firmware_without_initialize).valid);
    const char* initialize_without_firmware[] = {"px4-it930x-probe", "--initialize", "--base",
                                                 "12345678901234", "--device", "1"};
    CHECK(!parse_it930x_probe_arguments(6, initialize_without_firmware).valid);
    const char* missing_device[] = {"px4-it930x-probe", "--base", "12345678901234"};
    CHECK(!parse_it930x_probe_arguments(3, missing_device).valid);
    const char* invalid_base[] = {"px4-it930x-probe", "--base", "bad", "--device", "1"};
    CHECK(!parse_it930x_probe_arguments(5, invalid_base).valid);
    const char* help[] = {"px4-it930x-probe", "--help"};
    const auto parsed_help = parse_it930x_probe_arguments(2, help);
    CHECK(parsed_help.valid && parsed_help.help);
    return true;
}

bool test_sha256_and_firmware_policy()
{
    constexpr std::array<std::uint8_t, 32U> empty_digest{
        0xe3U, 0xb0U, 0xc4U, 0x42U, 0x98U, 0xfcU, 0x1cU, 0x14U,
        0x9aU, 0xfbU, 0xf4U, 0xc8U, 0x99U, 0x6fU, 0xb9U, 0x24U,
        0x27U, 0xaeU, 0x41U, 0xe4U, 0x64U, 0x9bU, 0x93U, 0x4cU,
        0xa4U, 0x95U, 0x99U, 0x1bU, 0x78U, 0x52U, 0xb8U, 0x55U};
    constexpr std::array<std::uint8_t, 3U> abc{'a', 'b', 'c'};
    constexpr std::array<std::uint8_t, 32U> abc_digest{
        0xbaU, 0x78U, 0x16U, 0xbfU, 0x8fU, 0x01U, 0xcfU, 0xeaU,
        0x41U, 0x41U, 0x40U, 0xdeU, 0x5dU, 0xaeU, 0x22U, 0x23U,
        0xb0U, 0x03U, 0x61U, 0xa3U, 0x96U, 0x17U, 0x7aU, 0x9cU,
        0xb4U, 0x10U, 0xffU, 0x61U, 0xf2U, 0x00U, 0x15U, 0xadU};
    CHECK(FirmwareTestAccess::sha256(ByteView{nullptr, 0U}) == empty_digest);
    CHECK(FirmwareTestAccess::sha256(ByteView{abc.data(), abc.size()}) == abc_digest);
    CHECK(FirmwareTestAccess::accepts_policy(kIt930xFirmwareSize, kIt930xFirmwareSha256));
    CHECK(!FirmwareTestAccess::accepts_policy(kIt930xFirmwareSize - 1U,
                                              kIt930xFirmwareSha256));
    auto wrong_digest = kIt930xFirmwareSha256;
    wrong_digest[0] ^= 0x01U;
    CHECK(!FirmwareTestAccess::accepts_policy(kIt930xFirmwareSize, wrong_digest));

    std::error_code temp_directory_error;
    const std::filesystem::path temp_directory =
        std::filesystem::temp_directory_path(temp_directory_error);
    CHECK(!temp_directory_error);
    const std::filesystem::path path = temp_directory / "px4-userland-3a-invalid-firmware.bin";
    [[maybe_unused]] const TemporaryFileCleanup path_cleanup(path);
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        CHECK(file.is_open());
        const std::array<std::uint8_t, kIt930xFirmwareSize> wrong_bytes{};
        file.write(reinterpret_cast<const char*>(wrong_bytes.data()),
                   static_cast<std::streamsize>(wrong_bytes.size()));
        CHECK(file.good());
    }
    FirmwareProvider wrong_hash_provider(path.string());
    const auto wrong_hash = wrong_hash_provider.load();
    CHECK(!wrong_hash);
    CHECK(wrong_hash.error() == Error::FIRMWARE_REJECTED);
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        CHECK(file.is_open());
        const std::array<std::uint8_t, kIt930xFirmwareSize - 1U> short_bytes{};
        file.write(reinterpret_cast<const char*>(short_bytes.data()),
                   static_cast<std::streamsize>(short_bytes.size()));
        CHECK(file.good());
    }
    FirmwareProvider wrong_size_provider(path.string());
    const auto wrong_size = wrong_size_provider.load();
    CHECK(!wrong_size);
    CHECK(wrong_size.error() == Error::FIRMWARE_REJECTED);
    std::error_code remove_error;
    std::filesystem::remove(path, remove_error);
    CHECK(!remove_error);

    FirmwareProvider missing_provider(path.string());
    const auto missing = missing_provider.load();
    CHECK(!missing);
    CHECK(missing.error() == Error::NOT_FOUND);
    const std::string nul_path = (temp_directory / "px4-userland-3a").string() + '\0' +
                                 "firmware.bin";
    FirmwareProvider nul_provider(nul_path);
    CHECK(nul_provider.load().error() == Error::INVALID_ARGUMENT);
    return true;
}

bool test_it930x_scatter_parser()
{
    const std::array<std::uint8_t, 9U> block{0x03U, 0x00U, 0x00U, 0x01U,
                                               0x00U, 0x10U, 0x02U, 0xaaU, 0xbbU};
    const auto parsed = It930xTestAccess::parse_scatter_block(
        ByteView{block.data(), block.size()}, 0U);
    CHECK(parsed);
    CHECK(parsed.value().offset == 0U && parsed.value().size == block.size());
    CHECK(It930xTestAccess::validate_scatter_image(
        ByteView{block.data(), block.size()}));
    for (std::size_t size = 0U; size < block.size(); ++size) {
        CHECK(!It930xTestAccess::validate_scatter_image(ByteView{block.data(), size}));
    }

    auto wrong_marker = block;
    wrong_marker[0] = 0x02U;
    CHECK(!It930xTestAccess::parse_scatter_block(
        ByteView{wrong_marker.data(), wrong_marker.size()}, 0U));
    auto zero_data = block;
    zero_data[6] = 0U;
    CHECK(!It930xTestAccess::validate_scatter_image(
        ByteView{zero_data.data(), zero_data.size()}));
    const std::array<std::uint8_t, 4U> zero_segments{0x03U, 0x00U, 0x00U, 0x00U};
    CHECK(!It930xTestAccess::validate_scatter_image(
        ByteView{zero_segments.data(), zero_segments.size()}));
    auto oversized = block;
    oversized[6] = 244U;
    CHECK(!It930xTestAccess::validate_scatter_image(
        ByteView{oversized.data(), oversized.size()}));
    std::array<std::uint8_t, 250U> maximum_block{};
    maximum_block[0] = 0x03U;
    maximum_block[3] = 0x01U;
    maximum_block[6] = 243U;
    CHECK(It930xTestAccess::parse_scatter_block(
        ByteView{maximum_block.data(), maximum_block.size()}, 0U));
    CHECK(It930xTestAccess::validate_scatter_image(
        ByteView{maximum_block.data(), maximum_block.size()}));
    const std::array<std::uint8_t, 7U> many_segments{0x03U, 0x00U, 0x00U, 0xffU,
                                                       0U, 0U, 1U};
    CHECK(!It930xTestAccess::parse_scatter_block(
        ByteView{many_segments.data(), many_segments.size()}, 0U));

    std::array<std::uint8_t, 10U> trailing{};
    std::copy(block.begin(), block.end(), trailing.begin());
    CHECK(!It930xTestAccess::validate_scatter_image(
        ByteView{trailing.data(), trailing.size()}));
    CHECK(!It930xTestAccess::parse_scatter_block(
        ByteView{block.data(), block.size()}, block.size()));
    CHECK(It930xTestAccess::validate_scatter_image(
        ByteView{block.data(), block.size()}));
    return true;
}

bool test_it930x_golden_and_typed_operations()
{
    MockTransport transport;
    const std::array<std::uint8_t, 7U> golden_request{
        0x06U, 0x00U, 0x22U, 0x00U, 0x01U, 0xffU, 0xdcU};
    const std::array<std::uint8_t, 9U> golden_response{
        0x08U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0xffU, 0xffU};
    transport.expect_bulk_write(kCommandOutEndpoint,
                                ByteView{golden_request.data(), golden_request.size()});
    transport.expect_bulk_read(kCommandInEndpoint,
                               ByteView{golden_response.data(), golden_response.size()});
    constexpr std::array<std::uint8_t, 6U> read_payload{1U, 2U, 0U, 0U, 0x12U, 0x34U};
    constexpr std::array<std::uint8_t, 1U> read_value{0x12U};
    const auto expected_read_request = test_request(
        0x00U, 1U, ByteView{read_payload.data(), read_payload.size()});
    const std::array<std::uint8_t, 12U> golden_register_request{
        0x0bU, 0x00U, 0x00U, 0x01U, 0x01U, 0x02U, 0x00U, 0x00U,
        0x12U, 0x34U, 0xc8U, 0xecU};
    CHECK(expected_read_request == std::vector<std::uint8_t>(golden_register_request.begin(),
                                                              golden_register_request.end()));
    const auto register_response = test_response(
        1U, 0U, ByteView{read_value.data(), read_value.size()});
    transport.expect_bulk_write(kCommandOutEndpoint,
                                ByteView{golden_register_request.data(), golden_register_request.size()});
    transport.expect_bulk_read(kCommandInEndpoint,
                               ByteView{register_response.data(), register_response.size()});
    constexpr std::array<std::uint8_t, 1U> write_payload{0x07U};
    expect_command(transport, 0x01U, 2U,
                   ByteView{std::array<std::uint8_t, 7U>{1U, 2U, 0U, 0U, 0xf1U, 0x03U, 0x07U}.data(), 7U},
                   ByteView{nullptr, 0U});
    const std::array<std::uint8_t, 3U> i2c_read_payload{2U, 1U, 0xa0U};
    const std::array<std::uint8_t, 2U> i2c_value{0x33U, 0x44U};
    expect_command(transport, 0x2aU, 3U,
                   ByteView{i2c_read_payload.data(), i2c_read_payload.size()},
                   ByteView{i2c_value.data(), i2c_value.size()});
    const std::array<std::uint8_t, 4U> i2c_write_payload{1U, 2U, 0x4aU, 0x55U};
    expect_command(transport, 0x2bU, 4U,
                   ByteView{i2c_write_payload.data(), i2c_write_payload.size()},
                   ByteView{nullptr, 0U});

    It930xController controller(transport, kFastPacing);
    CHECK(controller.firmware_version().value() == 0U);
    CHECK(controller.read_register(0x1234U).value() == 0x12U);
    CHECK(controller.write_register(0xf103U, write_payload[0U]));
    const auto read = controller.i2c_read(1U, 0x50U, 2U);
    CHECK(read && read.value() == std::vector<std::uint8_t>(i2c_value.begin(), i2c_value.end()));
    CHECK(controller.i2c_write(2U, 0x25U,
                               ByteView{i2c_write_payload.data() + 3U, 1U}));
    CHECK(transport.remaining_expectations() == 0U);
    return true;
}

bool test_it930x_limits_and_failures()
{
    MockTransport no_io;
    It930xController controller(no_io, kFastPacing);
    CHECK(controller.read_registers(0U, 0U).error() == Error::INVALID_ARGUMENT);
    CHECK(controller.read_registers(0U, 252U).error() == Error::INVALID_ARGUMENT);
    std::array<std::uint8_t, 245U> register_data{};
    CHECK(controller.write_registers(0U, ByteView{register_data.data(), register_data.size()}).error() ==
          Error::INVALID_ARGUMENT);
    CHECK(controller.i2c_read(0U, 0U, 1U).error() == Error::INVALID_ARGUMENT);
    CHECK(controller.i2c_read(1U, 0x80U, 1U).error() == Error::INVALID_ARGUMENT);
    CHECK(controller.i2c_read(1U, 0U, 252U).error() == Error::INVALID_ARGUMENT);
    CHECK(controller.i2c_write(3U, 0x7fU,
                               ByteView{register_data.data(), 248U}).error() == Error::INVALID_ARGUMENT);

    constexpr std::array<std::uint8_t, 1U> query{1U};
    constexpr std::array<std::uint8_t, 4U> version{0U, 0U, 0U, 1U};
    const auto request = test_request(0x22U, 0U, ByteView{query.data(), query.size()});
    const auto good_response = test_response(0U, 0U, ByteView{version.data(), version.size()});
    MockTransport short_out;
    short_out.expect_bulk_write(kCommandOutEndpoint,
                                ByteView{request.data(), request.size()}, MockOutcome::short_transfer);
    CHECK(It930xController(short_out, kFastPacing).firmware_version().error() == Error::USB_IO);

    MockTransport short_in;
    short_in.expect_bulk_write(kCommandOutEndpoint,
                               ByteView{request.data(), request.size()});
    short_in.expect_bulk_read(kCommandInEndpoint,
                              ByteView{good_response.data(), good_response.size()},
                              MockOutcome::short_transfer);
    CHECK(It930xController(short_in, kFastPacing).firmware_version().error() == Error::USB_IO);

    const auto malformed_request = request;
    auto bad_checksum = good_response;
    bad_checksum.back() ^= 0x01U;
    MockTransport malformed;
    malformed.expect_bulk_write(kCommandOutEndpoint,
                                ByteView{malformed_request.data(), malformed_request.size()});
    malformed.expect_bulk_read(kCommandInEndpoint,
                               ByteView{bad_checksum.data(), bad_checksum.size()});
    CHECK(It930xController(malformed, kFastPacing).firmware_version().error() == Error::PROTOCOL_ERROR);

    auto bad_status = test_response(0U, 1U, ByteView{version.data(), version.size()});
    MockTransport status;
    status.expect_bulk_write(kCommandOutEndpoint,
                             ByteView{request.data(), request.size()});
    status.expect_bulk_read(kCommandInEndpoint,
                            ByteView{bad_status.data(), bad_status.size()});
    CHECK(It930xController(status, kFastPacing).firmware_version().error() == Error::USB_IO);

    auto bad_sequence = test_response(1U, 0U, ByteView{version.data(), version.size()});
    MockTransport sequence;
    sequence.expect_bulk_write(kCommandOutEndpoint,
                               ByteView{request.data(), request.size()});
    sequence.expect_bulk_read(kCommandInEndpoint,
                              ByteView{bad_sequence.data(), bad_sequence.size()});
    CHECK(It930xController(sequence, kFastPacing).firmware_version().error() == Error::PROTOCOL_ERROR);

    auto short_declared = good_response;
    short_declared[0] = static_cast<std::uint8_t>(short_declared[0] - 1U);
    MockTransport wrong_length;
    wrong_length.expect_bulk_write(kCommandOutEndpoint,
                                   ByteView{request.data(), request.size()});
    wrong_length.expect_bulk_read(kCommandInEndpoint,
                                  ByteView{short_declared.data(), short_declared.size()});
    CHECK(It930xController(wrong_length, kFastPacing).firmware_version().error() == Error::PROTOCOL_ERROR);

    MockTransport timeout;
    timeout.expect_bulk_write(kCommandOutEndpoint,
                              ByteView{request.data(), request.size()}, MockOutcome::timeout);
    CHECK(It930xController(timeout, kFastPacing).firmware_version().error() == Error::TIMEOUT);
    MockTransport disconnect;
    disconnect.expect_bulk_write(kCommandOutEndpoint,
                                 ByteView{request.data(), request.size()});
    disconnect.expect_bulk_read(kCommandInEndpoint, ByteView{nullptr, 0U},
                                MockOutcome::disconnect);
    CHECK(It930xController(disconnect, kFastPacing).firmware_version().error() == Error::DISCONNECTED);
    return true;
}

bool test_it930x_command_pacing()
{
    constexpr std::array<std::uint8_t, 1U> query{1U};
    constexpr std::array<std::uint8_t, 4U> version{0U, 0U, 0U, 1U};
    const auto request = test_request(0x22U, 0U, ByteView{query.data(), query.size()});
    const auto response = test_response(0U, 0U, ByteView{version.data(), version.size()});

    MockTransport success_transport;
    success_transport.expect_bulk_write(kCommandOutEndpoint,
                                        ByteView{request.data(), request.size()});
    success_transport.expect_bulk_read(kCommandInEndpoint,
                                       ByteView{response.data(), response.size()});
    It930xController success(success_transport);
    const auto success_start = std::chrono::steady_clock::now();
    CHECK(success.firmware_version().value() == 1U);
    const auto success_elapsed = std::chrono::steady_clock::now() - success_start;
    CHECK(success_elapsed >= std::chrono::milliseconds(2));

    MockTransport failure_transport;
    failure_transport.expect_bulk_write(kCommandOutEndpoint,
                                        ByteView{request.data(), request.size()},
                                        MockOutcome::timeout);
    It930xController failure(failure_transport);
    const auto failure_start = std::chrono::steady_clock::now();
    CHECK(failure.firmware_version().error() == Error::TIMEOUT);
    const auto failure_elapsed = std::chrono::steady_clock::now() - failure_start;
    CHECK(failure_elapsed >= std::chrono::milliseconds(1));
    return true;
}

bool test_it930x_sequence_wrap()
{
    constexpr std::array<std::uint8_t, 1U> query{1U};
    constexpr std::array<std::uint8_t, 4U> version{0U, 0U, 0U, 1U};
    MockTransport transport;
    for (std::size_t index = 0U; index < 257U; ++index) {
        const auto request = test_request(0x22U, static_cast<std::uint8_t>(index),
                                          ByteView{query.data(), query.size()});
        const auto response = test_response(static_cast<std::uint8_t>(index), 0U,
                                             ByteView{version.data(), version.size()});
        transport.expect_bulk_write(kCommandOutEndpoint,
                                    ByteView{request.data(), request.size()});
        transport.expect_bulk_read(kCommandInEndpoint,
                                   ByteView{response.data(), response.size()});
    }
    It930xController controller(transport, kFastPacing);
    for (std::size_t index = 0U; index < 257U; ++index) {
        CHECK(controller.firmware_version().value() == 1U);
    }
    CHECK(transport.remaining_expectations() == 0U);
    return true;
}

bool test_it930x_firmware_load_paths()
{
    constexpr std::array<std::uint8_t, 1U> query{1U};
    constexpr std::array<std::uint8_t, 4U> zero_version{0U, 0U, 0U, 0U};
    constexpr std::array<std::uint8_t, 4U> loaded_version{0U, 0U, 2U, 1U};
    const std::array<std::uint8_t, 7U> speed_payload{1U, 2U, 0U, 0U, 0xf1U, 0x03U, 0x07U};

    const std::array<std::uint8_t, 4U> invalid_image{0x03U, 0x00U, 0x00U, 0x00U};
    MockTransport invalid_transport;
    It930xController invalid_controller(invalid_transport, kFastPacing);
    const auto invalid = invalid_controller.initialize_q3u4(
        FirmwareTestAccess::make_image(ByteView{invalid_image.data(), invalid_image.size()}));
    CHECK(!invalid && invalid.error() == Error::FIRMWARE_REJECTED);
    CHECK(invalid_transport.operations().empty());

    MockTransport cold_transport;
    expect_command(cold_transport, 0x22U, 0U,
                   ByteView{query.data(), query.size()},
                   ByteView{zero_version.data(), zero_version.size()});
    expect_command(cold_transport, 0x01U, 1U,
                   ByteView{speed_payload.data(), speed_payload.size()}, ByteView{nullptr, 0U});
    expect_command(cold_transport, 0x29U, 2U,
                   ByteView{kSyntheticFirmwareImage.data(), kSyntheticFirmwareImage.size()},
                   ByteView{nullptr, 0U});
    expect_command(cold_transport, 0x23U, 3U,
                   ByteView{nullptr, 0U}, ByteView{nullptr, 0U});
    expect_command(cold_transport, 0x22U, 4U,
                   ByteView{query.data(), query.size()},
                   ByteView{loaded_version.data(), loaded_version.size()});
    expect_q3u4_warm_sequence(cold_transport, 5U);
    It930xController cold(cold_transport, kFastPacing);
    const auto loaded = cold.initialize_q3u4(
        FirmwareTestAccess::make_image(
            ByteView{kSyntheticFirmwareImage.data(), kSyntheticFirmwareImage.size()}));
    CHECK(loaded && !loaded.value().already_loaded &&
          loaded.value().firmware_version == 0x00000201U && loaded.value().verified);
    CHECK(cold_transport.remaining_expectations() == 0U);

    MockTransport warm_transport;
    expect_command(warm_transport, 0x22U, 0U,
                   ByteView{query.data(), query.size()},
                   ByteView{loaded_version.data(), loaded_version.size()});
    expect_q3u4_warm_sequence(warm_transport, 1U);
    It930xController warm(warm_transport, kFastPacing);
    const auto already_loaded = warm.initialize_q3u4(
        FirmwareTestAccess::make_image(
            ByteView{kSyntheticFirmwareImage.data(), kSyntheticFirmwareImage.size()}));
    CHECK(already_loaded && already_loaded.value().already_loaded &&
          already_loaded.value().firmware_version == 0x00000201U &&
          already_loaded.value().verified);
    CHECK(warm_transport.remaining_expectations() == 0U);

    MockTransport require_cold_transport;
    expect_command(require_cold_transport, 0x22U, 0U,
                   ByteView{query.data(), query.size()},
                   ByteView{loaded_version.data(), loaded_version.size()});
    It930xController require_cold(require_cold_transport, kFastPacing);
    const auto cold_rejected = require_cold.initialize_q3u4(
        FirmwareTestAccess::make_image(
            ByteView{kSyntheticFirmwareImage.data(), kSyntheticFirmwareImage.size()}),
        InitializationPolicy::require_cold);
    CHECK(!cold_rejected && cold_rejected.error() == Error::NOT_READY);
    CHECK(require_cold_transport.remaining_expectations() == 0U);

    MockTransport mismatch_transport;
    std::uint8_t mismatch_sequence = 0U;
    expect_command(mismatch_transport, 0x22U, mismatch_sequence++,
                   ByteView{query.data(), query.size()},
                   ByteView{loaded_version.data(), loaded_version.size()});
    mismatch_sequence = expect_q3u4_warm_sequence(
        mismatch_transport, mismatch_sequence, false);
    expect_register_read(mismatch_transport, mismatch_sequence, 0xda1dU, {1U});
    It930xController mismatch(mismatch_transport, kFastPacing);
    const auto mismatched = mismatch.initialize_q3u4(
        FirmwareTestAccess::make_image(
            ByteView{kSyntheticFirmwareImage.data(), kSyntheticFirmwareImage.size()}));
    CHECK(!mismatched && mismatched.error() == Error::PROTOCOL_ERROR);
    CHECK(mismatch_transport.remaining_expectations() == 0U);

    MockTransport readback_failure_transport;
    std::uint8_t failure_sequence = 0U;
    expect_command(readback_failure_transport, 0x22U, failure_sequence++,
                   ByteView{query.data(), query.size()},
                   ByteView{loaded_version.data(), loaded_version.size()});
    failure_sequence = expect_q3u4_warm_sequence(
        readback_failure_transport, failure_sequence, false);
    expect_register_read(readback_failure_transport, failure_sequence, 0xda1dU, {0U},
                         MockOutcome::timeout);
    It930xController readback_failure(readback_failure_transport, kFastPacing);
    const auto failed_readback = readback_failure.initialize_q3u4(
        FirmwareTestAccess::make_image(
            ByteView{kSyntheticFirmwareImage.data(), kSyntheticFirmwareImage.size()}));
    CHECK(!failed_readback && failed_readback.error() == Error::TIMEOUT);
    CHECK(readback_failure_transport.remaining_expectations() == 0U);
    return true;
}

struct SinkCapture final {
    std::array<LogLevel, 4> levels{};
    std::array<std::string_view, 4> messages{};
    std::size_t count = 0U;
};

void capture_log(const LogRecord& record, void* context) noexcept
{
    auto* capture = static_cast<SinkCapture*>(context);
    if (capture->count < capture->levels.size()) {
        capture->levels[capture->count] = record.level;
        capture->messages[capture->count] = record.message;
    }
    ++capture->count;
}

bool test_logging()
{
    SinkCapture capture;
    Logger logger(LogLevel::warn, capture_log, &capture);
    CHECK(!logger.enabled(LogLevel::info));
    CHECK(logger.enabled(LogLevel::warn));
    logger.log(LogLevel::info, "ignored");
    logger.log(LogLevel::warn, "warning");
    logger.log(LogLevel::error, "failure");
    CHECK(capture.count == 2U);
    CHECK(capture.levels[0] == LogLevel::warn);
    CHECK(capture.messages[0] == "warning");
    CHECK(capture.levels[1] == LogLevel::error);
    CHECK(capture.messages[1] == "failure");
    CHECK(std::strcmp(log_level_string(LogLevel::trace), "TRACE") == 0);
    CHECK(std::strcmp(log_level_string(LogLevel::debug), "DEBUG") == 0);
    CHECK(std::strcmp(log_level_string(LogLevel::info), "INFO") == 0);
    CHECK(std::strcmp(log_level_string(LogLevel::warn), "WARN") == 0);
    CHECK(std::strcmp(log_level_string(LogLevel::error), "ERROR") == 0);
    logger.set_minimum_level(LogLevel::trace);
    logger.log(LogLevel::trace, "no clock text");
    CHECK(capture.count == 3U);
    CHECK(capture.messages[2] == "no clock text");
    return true;
}

bool test_mock_transport()
{
    constexpr std::array<std::uint8_t, 3> read_response{1U, 2U, 3U};
    constexpr std::array<std::uint8_t, 2> write_request{9U, 8U};
    constexpr std::array<std::uint8_t, 4> stream_data{0x47U, 0x01U, 0x02U, 0x03U};
    const StreamConfig stream_config{kTsInEndpoint, 188U, 2U};

    MockTransport transport;
    transport.expect_bulk_read(kCommandInEndpoint,
                               ByteView{read_response.data(), read_response.size()});
    transport.expect_bulk_write(kCommandOutEndpoint,
                                ByteView{write_request.data(), write_request.size()});
    transport.expect_stream_start(stream_config);
    transport.expect_stream_event(ByteView{stream_data.data(), stream_data.size()},
                                  MockOutcome::short_transfer, 3U);
    transport.expect_stream_cancel();

    std::array<std::uint8_t, 8> output{};
    auto read = transport.bulk_read(kCommandInEndpoint,
                                    MutableByteView{output.data(), output.size()}, Timeout{100U});
    CHECK(read);
    CHECK(read.value() == read_response.size());
    CHECK(std::memcmp(output.data(), read_response.data(), read_response.size()) == 0);
    auto write = transport.bulk_write(kCommandOutEndpoint,
                                      ByteView{write_request.data(), write_request.size()}, Timeout{100U});
    CHECK(write);
    CHECK(write.value() == write_request.size());
    CHECK(transport.start_stream(stream_config));
    CHECK(transport.stream_active());
    auto event = transport.wait_stream(Timeout{100U});
    CHECK(event);
    CHECK(event.value().kind == StreamEventKind::short_transfer);
    CHECK(event.value().size == 3U);
    CHECK(event.value().data[0] == 0x47U);
    CHECK(transport.cancel_stream());
    CHECK(!transport.stream_active());
    CHECK(transport.cancel_stream());
    CHECK(transport.stop_stream());
    CHECK(transport.remaining_expectations() == 0U);

    const std::array<MockOperation, 5> expected_operations{
        MockOperation::bulk_read, MockOperation::bulk_write, MockOperation::stream_start,
        MockOperation::stream_wait, MockOperation::stream_cancel};
    CHECK(transport.operations().size() == expected_operations.size());
    for (std::size_t index = 0U; index < expected_operations.size(); ++index) {
        CHECK(transport.operations()[index] == expected_operations[index]);
    }
    return true;
}

bool test_mock_failures_and_bounds()
{
    std::array<std::uint8_t, 1> byte{};
    constexpr std::array<std::uint8_t, 3> short_response{0x21U, 0x22U, 0x23U};
    MockTransport short_transfer;
    short_transfer.expect_bulk_read(kCommandInEndpoint,
                                    ByteView{short_response.data(), short_response.size()},
                                    MockOutcome::short_transfer);
    std::array<std::uint8_t, 3> short_output{};
    BulkReadObservation short_observation{Error::USB_IO, 99U};
    auto short_read = short_transfer.bulk_read(
        kCommandInEndpoint, MutableByteView{short_output.data(), short_output.size()}, Timeout{1U},
        &short_observation);
    CHECK(short_read);
    CHECK(short_read.value() == 2U);
    CHECK(short_observation.completion_error == Error::OK);
    CHECK(short_observation.transferred == 2U);
    CHECK(short_output[0] == 0x21U && short_output[1] == 0x22U);

    MockTransport partial_timeout;
    std::array<std::uint8_t, 512U> timeout_response{};
    partial_timeout.expect_bulk_read(kCommandInEndpoint,
                                     ByteView{timeout_response.data(), timeout_response.size()},
                                     MockOutcome::timeout, timeout_response.size());
    BulkReadObservation partial_timeout_observation{Error::OK, 99U};
    auto partial_timeout_read = partial_timeout.bulk_read(
        kCommandInEndpoint,
        MutableByteView{timeout_response.data(), timeout_response.size()}, Timeout{1U},
        &partial_timeout_observation);
    CHECK(partial_timeout_read && partial_timeout_read.value() == timeout_response.size());
    CHECK(partial_timeout_observation.completion_error == Error::TIMEOUT);
    CHECK(partial_timeout_observation.transferred == timeout_response.size());

    MockTransport zero_timeout;
    zero_timeout.expect_bulk_read(kCommandInEndpoint, ByteView{nullptr, 0U},
                                  MockOutcome::timeout);
    BulkReadObservation zero_timeout_observation{Error::OK, 99U};
    auto zero_timeout_read = zero_timeout.bulk_read(
        kCommandInEndpoint, MutableByteView{byte.data(), byte.size()}, Timeout{1U},
        &zero_timeout_observation);
    CHECK(!zero_timeout_read && zero_timeout_read.error() == Error::TIMEOUT);
    CHECK(zero_timeout_observation.completion_error == Error::TIMEOUT);
    CHECK(zero_timeout_observation.transferred == 0U);

    MockTransport invalid_transfer;
    invalid_transfer.expect_bulk_read(kCommandInEndpoint, ByteView{short_response.data(), 1U},
                                      MockOutcome::success, 2U);
    BulkReadObservation invalid_transfer_observation{Error::OK, 99U};
    auto invalid_transfer_read = invalid_transfer.bulk_read(
        kCommandInEndpoint, MutableByteView{byte.data(), byte.size()}, Timeout{1U},
        &invalid_transfer_observation);
    CHECK(!invalid_transfer_read && invalid_transfer_read.error() == Error::INTERNAL);
    CHECK(invalid_transfer_observation.completion_error == Error::INTERNAL);
    CHECK(invalid_transfer_observation.transferred == 0U);

    MockTransport bounds;
    constexpr std::size_t max_command_size = kMaxCommandTransfer;
    std::array<std::uint8_t, max_command_size> max_command_data{};
    bounds.expect_bulk_read(kCommandInEndpoint,
                            ByteView{max_command_data.data(), max_command_data.size()});
    auto max_read = bounds.bulk_read(kCommandInEndpoint,
                                     MutableByteView{max_command_data.data(), max_command_data.size()},
                                     Timeout{1U});
    CHECK(max_read);
    CHECK(max_read.value() == max_command_size);
    auto too_large_read = bounds.bulk_read(kCommandInEndpoint,
                                           MutableByteView{byte.data(), kMaxCommandTransfer + 1U},
                                           Timeout{1U});
    CHECK(!too_large_read);
    CHECK(too_large_read.error() == Error::BUFFER_TOO_SMALL);
    bounds.expect_bulk_write(kCommandOutEndpoint,
                             ByteView{max_command_data.data(), max_command_data.size()});
    auto max_write = bounds.bulk_write(kCommandOutEndpoint,
                                       ByteView{max_command_data.data(), max_command_data.size()},
                                       Timeout{1U});
    CHECK(max_write);
    CHECK(max_write.value() == max_command_size);
    auto too_large_write = bounds.bulk_write(kCommandOutEndpoint,
                                             ByteView{byte.data(), kMaxCommandTransfer + 1U}, Timeout{1U});
    CHECK(!too_large_write);
    CHECK(too_large_write.error() == Error::BUFFER_TOO_SMALL);

    MockTransport null_pointer;
    auto null_read = null_pointer.bulk_read(kCommandInEndpoint,
                                            MutableByteView{nullptr, 1U}, Timeout{1U});
    CHECK(!null_read);
    CHECK(null_read.error() == Error::INVALID_ARGUMENT);
    auto null_write = null_pointer.bulk_write(kCommandOutEndpoint, ByteView{nullptr, 1U}, Timeout{1U});
    CHECK(!null_write);
    CHECK(null_write.error() == Error::INVALID_ARGUMENT);
    CHECK(null_pointer.operations().empty());

    MockTransport stream_bounds;
    const StreamConfig too_large_stream{kTsInEndpoint, kMaxStreamTransfer + 1U, 1U};
    auto too_large_stream_result = stream_bounds.start_stream(too_large_stream);
    CHECK(!too_large_stream_result);
    CHECK(too_large_stream_result.error() == Error::INVALID_ARGUMENT);
    const StreamConfig wrong_endpoint{ kCommandInEndpoint, 188U, 1U };
    CHECK(stream_bounds.start_stream(wrong_endpoint).error() == Error::INVALID_ARGUMENT);

    MockTransport ordering;
    constexpr std::array<std::uint8_t, 1> expected{0x11U};
    ordering.expect_bulk_read(kCommandInEndpoint, ByteView{expected.data(), expected.size()});
    auto wrong_order = ordering.bulk_write(kCommandOutEndpoint,
                                           ByteView{expected.data(), expected.size()}, Timeout{1U});
    CHECK(!wrong_order);
    CHECK(wrong_order.error() == Error::PROTOCOL_ERROR);
    CHECK(ordering.remaining_expectations() == 1U);

    const MockOutcome outcomes[] = {MockOutcome::timeout, MockOutcome::disconnect,
                                    MockOutcome::protocol_error};
    const Error errors[] = {Error::TIMEOUT, Error::DISCONNECTED, Error::PROTOCOL_ERROR};
    for (std::size_t index = 0U; index < 3U; ++index) {
        MockTransport failing;
        failing.expect_bulk_read(kCommandInEndpoint, ByteView{nullptr, 0U}, outcomes[index]);
        auto result = failing.bulk_read(kCommandInEndpoint,
                                        MutableByteView{byte.data(), byte.size()}, Timeout{1U});
        CHECK(!result);
        CHECK(result.error() == errors[index]);
    }

    MockTransport stream_failure;
    const StreamConfig config{kTsInEndpoint, 188U, 1U};
    stream_failure.expect_stream_start(config);
    stream_failure.expect_stream_event(ByteView{nullptr, 0U}, MockOutcome::timeout);
    stream_failure.expect_stream_stop();
    CHECK(stream_failure.start_stream(config));
    auto timed_out = stream_failure.wait_stream(Timeout{1U});
    CHECK(!timed_out);
    CHECK(timed_out.error() == Error::TIMEOUT);
    CHECK(stream_failure.stream_active());
    CHECK(stream_failure.stop_stream());
    CHECK(stream_failure.stop_stream());
    CHECK(stream_failure.remaining_expectations() == 0U);
    return true;
}

UsbTopologyObservation valid_topology()
{
    UsbInterfaceObservation interface;
    interface.number = 0U;
    interface.alternate_setting = 0U;
    interface.endpoints = {
        {0x81U, EndpointType::bulk, 512U},
        {0x02U, EndpointType::bulk, 512U},
        {0x84U, EndpointType::bulk, 512U},
        {0x85U, EndpointType::bulk, 512U},
    };
    return UsbTopologyObservation{{std::move(interface)}};
}

DeviceObservation observation(const char* base, std::uint8_t dev_id,
                              UsbSpeed speed = UsbSpeed::high)
{
    DeviceObservation result;
    result.vendor_id = kQ3U4VendorId;
    result.product_id = kQ3U4ProductId;
    result.serial = std::string(base) + static_cast<char>('0' + dev_id);
    result.speed = speed;
    result.topology = valid_topology();
    return result;
}

bool test_error_and_serial_contract()
{
    static_assert(static_cast<std::uint8_t>(Error::INTERNAL) == 255U);
    static_assert(kCommandInEndpoint == 0x81U && kCommandOutEndpoint == 0x02U &&
                  kTsInEndpoint == 0x84U && kObservedTsInEndpoint == 0x85U);
    CHECK(std::strcmp(error_string(Error::USB_IO), "USB_IO") == 0);
    const auto parsed = parse_q3u4_serial("000012050009601");
    CHECK(parsed);
    CHECK(parsed.value().base_serial == "00001205000960");
    CHECK(parsed.value().dev_id == 1U);
    CHECK(parse_q3u4_serial("000012050009602").value().dev_id == 2U);
    CHECK(!parse_q3u4_serial("00001205000960").has_value());
    CHECK(!parse_q3u4_serial("00001205000960x").has_value());
    CHECK(!parse_q3u4_serial("000012050009603").has_value());
    CHECK(std::strcmp(observation_status_string(ObservationStatus::unsupported), "unsupported") == 0);
    return true;
}

bool test_identity_grouping_and_topology()
{
    const DeviceObservation main = observation("00000000000010", 1U);
    const DeviceObservation sub = observation("00000000000010", 2U);
    const DeviceObservation other_main = observation("00000000000020", 1U);
    const DeviceObservation unsupported = [] {
        DeviceObservation value = observation("00000000000030", 1U);
        value.vendor_id = 0x1234U;
        return value;
    }();

    const std::vector<DeviceObservation> input{sub, other_main, unsupported, main};
    const auto grouped = group_q3u4_devices(input);
    CHECK(grouped);
    CHECK(grouped.value().groups.size() == 2U);
    CHECK(grouped.value().groups[0].base_serial == "00000000000010");
    CHECK(grouped.value().groups[0].status == GroupStatus::ready);
    CHECK(grouped.value().groups[0].devices[0U]->serial.back() == '1');
    CHECK(grouped.value().groups[0].devices[1U]->serial.back() == '2');
    CHECK(grouped.value().groups[1].status == GroupStatus::incomplete);
    CHECK(grouped.value().rejected.size() == 1U);

    const auto duplicate = group_q3u4_devices(std::vector<DeviceObservation>{main, main, sub});
    CHECK(duplicate && duplicate.value().groups[0].status == GroupStatus::duplicate);
    const auto mismatched = group_q3u4_devices(std::vector<DeviceObservation>{main, other_main});
    CHECK(mismatched && mismatched.value().groups.size() == 2U);
    CHECK(mismatched.value().groups[0].status == GroupStatus::incomplete);

    DeviceObservation bad_speed = main;
    bad_speed.speed = UsbSpeed::full;
    CHECK(validate_q3u4_observation(bad_speed) == ObservationStatus::insufficient_speed);
    DeviceObservation bad_endpoint = main;
    bad_endpoint.topology.interfaces[0].endpoints[3].max_packet_size = 64U;
    CHECK(validate_q3u4_observation(bad_endpoint) == ObservationStatus::invalid_topology);
    DeviceObservation wrong_endpoint_type = main;
    wrong_endpoint_type.topology.interfaces[0].endpoints[0].type = EndpointType::other;
    CHECK(validate_q3u4_observation(wrong_endpoint_type) == ObservationStatus::invalid_topology);
    DeviceObservation missing_endpoint = main;
    missing_endpoint.topology.interfaces[0].endpoints.pop_back();
    CHECK(validate_q3u4_observation(missing_endpoint) == ObservationStatus::invalid_topology);
    DeviceObservation low_speed = main;
    low_speed.speed = UsbSpeed::low;
    CHECK(validate_q3u4_observation(low_speed) == ObservationStatus::insufficient_speed);
    DeviceObservation bad_alt = main;
    bad_alt.topology.interfaces[0].alternate_setting = 1U;
    CHECK(!q3u4_topology_is_usable(bad_alt.topology));
    DeviceObservation bad_serial = main;
    bad_serial.serial = "000000000000103";
    CHECK(validate_q3u4_observation(bad_serial) == ObservationStatus::invalid_serial);

    const auto incomplete = group_q3u4_devices(std::vector<DeviceObservation>{bad_speed, sub});
    CHECK(incomplete && incomplete.value().groups[0].status == GroupStatus::invalid_observation);

    const auto only_main = group_q3u4_devices(std::vector<DeviceObservation>{main});
    const auto only_sub = group_q3u4_devices(std::vector<DeviceObservation>{sub});
    CHECK(only_main && only_main.value().groups[0].status == GroupStatus::incomplete);
    CHECK(only_sub && only_sub.value().groups[0].status == GroupStatus::incomplete);

    const DeviceObservation duplicate_sub = observation("00000000000010", 2U);
    const auto duplicate_dev2 = group_q3u4_devices(
        std::vector<DeviceObservation>{main, sub, duplicate_sub});
    CHECK(duplicate_dev2 && duplicate_dev2.value().groups[0].status == GroupStatus::duplicate);

    const auto forward = group_q3u4_devices(std::vector<DeviceObservation>{main, sub});
    const auto reverse = group_q3u4_devices(std::vector<DeviceObservation>{sub, main});
    CHECK(forward && reverse);
    CHECK(forward.value().groups.size() == reverse.value().groups.size());
    CHECK(forward.value().groups[0].base_serial == reverse.value().groups[0].base_serial);
    CHECK(forward.value().groups[0].status == reverse.value().groups[0].status);
    CHECK(forward.value().groups[0].devices[0U]->serial ==
          reverse.value().groups[0].devices[0U]->serial);
    CHECK(forward.value().groups[0].devices[1U]->serial ==
          reverse.value().groups[0].devices[1U]->serial);

    DeviceObservation extra_endpoint = main;
    extra_endpoint.topology.interfaces[0].endpoints.push_back(
        UsbEndpointObservation{0x86U, EndpointType::bulk, 512U});
    CHECK(validate_q3u4_observation(extra_endpoint) == ObservationStatus::invalid_topology);
    DeviceObservation duplicate_interface = main;
    duplicate_interface.topology.interfaces.push_back(
        duplicate_interface.topology.interfaces[0]);
    CHECK(validate_q3u4_observation(duplicate_interface) == ObservationStatus::invalid_topology);
    DeviceObservation unrelated_extras = main;
    unrelated_extras.topology.interfaces.push_back(
        UsbInterfaceObservation{1U, 0U, {{0x87U, EndpointType::bulk, 64U}}});
    unrelated_extras.topology.interfaces.push_back(
        UsbInterfaceObservation{0U, 1U, {{0x88U, EndpointType::other, 0U}}});
    CHECK(validate_q3u4_observation(unrelated_extras) == ObservationStatus::usable);

    CHECK(select_ready_q3u4_group(forward.value(), {}).value() == 0U);
    const auto two_ready = group_q3u4_devices(std::vector<DeviceObservation>{
        main, sub, other_main, observation("00000000000020", 2U)});
    CHECK(two_ready);
    CHECK(select_ready_q3u4_group(two_ready.value(), {}).error() == Error::INVALID_ARGUMENT);
    CHECK(select_ready_q3u4_group(two_ready.value(), "00000000000020").value() == 1U);
    CHECK(select_ready_q3u4_group(two_ready.value(), "00000000000099").error() == Error::NOT_FOUND);
    CHECK(select_ready_q3u4_group(two_ready.value(), "bad").error() == Error::INVALID_ARGUMENT);
    return true;
}

bool test_collision_grouping_and_usb_path_selection()
{
    const auto at = [](DeviceObservation value, std::uint8_t address,
                       std::uint8_t port) {
        value.location.has_bus = true;
        value.location.has_address = true;
        value.location.bus = 1U;
        value.location.address = address;
        value.location.port_count = 1U;
        value.location.port_path[0U] = port;
        return value;
    };
    DeviceObservation m1 = at(observation("00000000000010", 1U), 30U, 2U);
    m1.product_id = kPxM1UrProductId;
    m1.serial = "000000000000001";
    DeviceObservation s1 = at(m1, 31U, 4U);
    s1.product_id = kPxS1UrProductId;
    const auto mixed = group_q3u4_devices({m1, s1});
    CHECK(mixed && mixed.value().groups.size() == 2U);
    CHECK(mixed.value().groups[0U].model != mixed.value().groups[1U].model);
    CHECK(mixed.value().groups[0U].status == GroupStatus::ready &&
          mixed.value().groups[1U].status == GroupStatus::ready);
    CHECK(select_ready_q3u4_group(mixed.value(), m1.serial).error() ==
          Error::INVALID_ARGUMENT);
    const auto m1_by_port = select_q3u4_group_by_usb_paths(
        mixed.value(), m1.serial, {"1-2"});
    CHECK(m1_by_port && m1_by_port.value().candidate_indices[0U] == 0U);
    const auto s1_by_address = select_q3u4_group_by_usb_paths(
        mixed.value(), m1.serial, {"1:31"});
    CHECK(s1_by_address && s1_by_address.value().candidate_indices[0U] == 1U);
    CHECK(select_q3u4_group_by_usb_paths(mixed.value(), m1.serial, {"1:0"}).error() ==
          Error::INVALID_ARGUMENT);
    DeviceObservation unknown_bus = m1;
    unknown_bus.location.bus = 0U;
    const auto unknown_location = group_q3u4_devices({unknown_bus});
    CHECK(unknown_location &&
          select_q3u4_group_by_usb_paths(unknown_location.value(), m1.serial,
                                         {"1-2"}).error() == Error::INVALID_ARGUMENT);
    CHECK(select_q3u4_group_by_usb_paths(mixed.value(), m1.serial, {"1-2."}).error() ==
          Error::INVALID_ARGUMENT);
    CHECK(select_q3u4_group_by_usb_paths(mixed.value(), m1.serial,
                                         {"1:30", "1:31"}).error() ==
          Error::INVALID_ARGUMENT);

    DeviceObservation m1_other = at(m1, 32U, 6U);
    const auto same_model = group_q3u4_devices({m1, m1_other});
    CHECK(same_model && same_model.value().groups.size() == 2U);
    CHECK(select_ready_q3u4_group(same_model.value(), m1.serial).error() ==
          Error::INVALID_ARGUMENT);
    const auto second = select_q3u4_group_by_usb_paths(
        same_model.value(), m1.serial, {"1-6"});
    CHECK(second && second.value().candidate_indices[0U] == 1U);

    const DeviceObservation q1a = at(observation("00000000000010", 1U), 20U, 1U);
    const DeviceObservation q1b = at(observation("00000000000010", 1U), 21U, 3U);
    const DeviceObservation q2a = at(observation("00000000000010", 2U), 22U, 2U);
    const DeviceObservation q2b = at(observation("00000000000010", 2U), 23U, 4U);
    const auto ambiguous = group_q3u4_devices({q1a, q2a, q1b, q2b});
    CHECK(ambiguous && ambiguous.value().groups.size() == 1U &&
          ambiguous.value().groups[0U].status == GroupStatus::duplicate &&
          ambiguous.value().groups[0U].candidates.size() == 4U);
    CHECK(select_ready_q3u4_group(ambiguous.value(), "00000000000010").error() ==
          Error::INVALID_ARGUMENT);
    const auto paired = select_q3u4_group_by_usb_paths(
        ambiguous.value(), "00000000000010", {"1:23", "1-3"});
    CHECK(paired && paired.value().candidate_indices[0U] == 2U &&
          paired.value().candidate_indices[1U] == 3U);
    CHECK(select_q3u4_group_by_usb_paths(ambiguous.value(), "00000000000010",
                                         {"1:20", "1:21"}).error() ==
          Error::INVALID_ARGUMENT);
    CHECK(select_q3u4_group_by_usb_paths(ambiguous.value(), "00000000000010",
                                         {"1:20"}).error() == Error::INVALID_ARGUMENT);
    CHECK(valid_runtime_instance("m1.001_port2"));
    CHECK(!valid_runtime_instance(".") && !valid_runtime_instance("..") &&
          !valid_runtime_instance("bad/name") && !valid_runtime_instance(""));
    return true;
}

bool test_lnb_15v_profile_capability_table()
{
    const std::array<std::pair<DeviceModel, bool>, 16U> expected{{
        {DeviceModel::px_q3u4, true},
        {DeviceModel::px_w3u4, true},
        {DeviceModel::px_mlt5pe, true},
        {DeviceModel::dtv02a_5ts_p, true},
        {DeviceModel::px_w3pe4, true},
        {DeviceModel::px_w3pe5, true},
        {DeviceModel::px_q3pe4, true},
        {DeviceModel::px_q3pe5, true},
        {DeviceModel::px_mlt8pe3, true},
        {DeviceModel::px_mlt8pe5, true},
        {DeviceModel::dtv02a_4ts_p, true},
        {DeviceModel::px_m1ur, false},
        {DeviceModel::px_s1ur, false},
        {DeviceModel::dtv03a_1tu, false},
        {DeviceModel::dtv02_1t1s_u, false},
        {DeviceModel::dtv02a_1t1s_u, false},
    }};
    for (const auto& entry : expected) {
        CHECK(device_profile(entry.first).supports_lnb_15v == entry.second);
    }
    return true;
}

#if PX4_ENABLE_LIBUSB

struct FakeDevice final {
    DeviceObservation observation;
    std::uint8_t serial_index = 1U;
    int serial_result = 0;
};

struct FakeHandle final {
    FakeDevice* device = nullptr;
    std::intptr_t fd = -1;
};

struct FakeTransfer final {
    LibusbApi::TransferCallback callback = nullptr;
    void* context = nullptr;
    std::uint8_t* buffer = nullptr;
    std::uint8_t marker = 0U;
    std::uint64_t submission_order = 0U;
    bool submitted = false;
    bool callback_pending = false;
};

class FakeApi final : public LibusbApi {
public:
    ~FakeApi() noexcept override
    {
        if (lifecycle_events != nullptr) {
            lifecycle_events->emplace_back("api-destroy");
        }
    }

    std::vector<FakeDevice*> devices;
    FakeDevice* wrapped_device = nullptr;
    std::vector<FakeDevice*> wrapped_devices;
    bool complete_events = false;
    bool complete_all_events = false;
    bool complete_events_reverse = false;
    bool cancel_callbacks = true;
    bool cancel_returns_not_found = false;
    bool callback_after_not_found = false;
    TransferStatus completion_status = TransferStatus::completed;
    int completion_length = 4;
    int bulk_result = 0;
    int claim_result = 0;
    std::size_t claim_fail_after = 0U;
    int wrap_result = 0;
    std::size_t wrap_fail_after = 0U;
    int submit_result = 0;
    std::size_t submit_fail_after = 0U;
    int bulk_transferred = -1;
    std::vector<std::uint8_t> bulk_payload;
    int event_result = 0;
    int open_result = 0;
    int serial_result = 0;
    int config_result = 0;
    int describe_config_result = 0;
    bool init_no_discovery = false;
    std::size_t init_calls = 0U;
    std::size_t open_calls = 0U;
    std::size_t claim_calls = 0U;
    std::size_t close_calls = 0U;
    std::size_t release_calls = 0U;
    std::size_t config_calls = 0U;
    std::size_t free_config_calls = 0U;
    std::size_t serial_calls = 0U;
    std::size_t wrap_calls = 0U;
    std::size_t get_device_list_calls = 0U;
    std::size_t bulk_calls = 0U;
    std::size_t alloc_calls = 0U;
    std::size_t free_calls = 0U;
    std::size_t cancel_calls = 0U;
    std::size_t event_calls = 0U;
    std::size_t submit_calls = 0U;
    std::size_t callback_calls = 0U;
    std::vector<std::uint8_t> endpoints;
    std::vector<int> lengths;
    std::vector<std::string> events;
    std::vector<std::intptr_t> wrapped_fds;
    std::vector<FakeDevice*> claim_devices;
    std::vector<bool> fd_valid_at_close;
    std::vector<std::string>* lifecycle_events = nullptr;
    std::vector<bool>* fd_valid_at_close_sink = nullptr;
    std::vector<std::intptr_t>* wrapped_fd_sink = nullptr;
    std::size_t* close_count_sink = nullptr;
    std::atomic<int> api_in_flight{0};
    std::atomic<int> api_max_in_flight{0};
    std::atomic<bool> block_bulk{false};
    std::atomic<bool> bulk_entered{false};
    std::atomic<bool> release_bulk{false};
    std::atomic<bool> block_lifecycle_api{false};
    std::atomic<bool> lifecycle_api_entered{false};
    std::atomic<bool> release_lifecycle_api{false};
    bool dispatch_stream_callbacks_in_bulk = false;

    void wait_for_bulk_stream_callbacks() noexcept
    {
        std::unique_lock<std::mutex> lock(bulk_stream_mutex_);
        bulk_stream_changed_.wait(lock, [this] { return bulk_stream_callbacks_done_; });
    }

    void release_bulk_after_stream_callbacks() noexcept
    {
        {
            const std::lock_guard<std::mutex> lock(bulk_stream_mutex_);
            release_bulk_after_callbacks_ = true;
        }
        bulk_stream_changed_.notify_all();
    }

    void record_lifecycle(const char* event) noexcept
    {
        if (lifecycle_events != nullptr) {
            lifecycle_events->emplace_back(event);
        }
    }

    void enter_bulk_gate() noexcept
    {
        const int current = api_in_flight.fetch_add(1) + 1;
        int observed = api_max_in_flight.load();
        while (current > observed &&
               !api_max_in_flight.compare_exchange_weak(observed, current)) {
        }
        if (block_bulk.load()) {
            bulk_entered.store(true);
            while (!release_bulk.load()) {
                std::this_thread::yield();
            }
        }
    }

    void leave_bulk_gate() noexcept { api_in_flight.fetch_sub(1); }

    void enter_lifecycle_gate() noexcept
    {
        const int current = api_in_flight.fetch_add(1) + 1;
        int observed = api_max_in_flight.load();
        while (current > observed &&
               !api_max_in_flight.compare_exchange_weak(observed, current)) {
        }
        if (block_lifecycle_api.load()) {
            lifecycle_api_entered.store(true);
            while (!release_lifecycle_api.load()) {
                std::this_thread::yield();
            }
        }
    }

    void leave_lifecycle_gate() noexcept { api_in_flight.fetch_sub(1); }

    int init(Context* context, bool no_device_discovery) noexcept override
    {
        ++init_calls;
        init_no_discovery = no_device_discovery;
        *context = this;
        return 0;
    }

    void exit(Context) noexcept override
    {
        events.emplace_back("exit");
        record_lifecycle("exit");
    }

    int get_device_list(Context, void** list, std::size_t* count) noexcept override
    {
        ++get_device_list_calls;
        *list = &devices;
        *count = devices.size();
        return 0;
    }

    Device list_device(void* list, std::size_t index) noexcept override
    {
        return static_cast<std::vector<FakeDevice*>*>(list)->at(index);
    }

    void free_device_list(void*) noexcept override { events.emplace_back("free-list"); }

    int get_device_info(Device device, DeviceObservation* observation,
                        std::uint8_t* serial_index) noexcept override
    {
        if (device == nullptr || observation == nullptr || serial_index == nullptr) {
            return LIBUSB_ERROR_INVALID_PARAM;
        }
        const auto* fake = static_cast<FakeDevice*>(device);
        *observation = fake->observation;
        *serial_index = fake->serial_index;
        return 0;
    }

    int get_config_descriptor(Device device, unsigned int, ConfigDescriptor* descriptor) noexcept override
    {
        ++config_calls;
        if (config_result != 0) {
            *descriptor = nullptr;
            return config_result;
        }
        *descriptor = device;
        return 0;
    }

    int describe_config_descriptor(ConfigDescriptor descriptor,
                                   UsbTopologyObservation* topology) noexcept override
    {
        if (describe_config_result != 0) {
            return describe_config_result;
        }
        if (descriptor == nullptr || topology == nullptr) {
            return LIBUSB_ERROR_INVALID_PARAM;
        }
        *topology = static_cast<FakeDevice*>(descriptor)->observation.topology;
        return 0;
    }

    void free_config_descriptor(ConfigDescriptor) noexcept override
    {
        ++free_config_calls;
        events.emplace_back("free-config");
    }

    int get_device_from_handle(Handle handle, Device* device) noexcept override
    {
        if (handle == nullptr || device == nullptr) {
            return LIBUSB_ERROR_INVALID_PARAM;
        }
        *device = static_cast<FakeHandle*>(handle)->device;
        return *device == nullptr ? LIBUSB_ERROR_NO_DEVICE : 0;
    }

    int get_serial_descriptor(Handle handle, std::uint8_t, char* output,
                              std::size_t output_size, std::size_t* length) noexcept override
    {
        ++serial_calls;
        if (handle != nullptr) {
            const auto* fake = static_cast<FakeHandle*>(handle)->device;
            if (fake != nullptr && fake->serial_result != 0) {
                return fake->serial_result;
            }
        }
        if (serial_result != 0) {
            return serial_result;
        }
        if (handle == nullptr || output == nullptr || length == nullptr) {
            return LIBUSB_ERROR_INVALID_PARAM;
        }
        const std::string& serial = static_cast<FakeHandle*>(handle)->device->observation.serial;
        if (serial.size() >= output_size) {
            return LIBUSB_ERROR_OVERFLOW;
        }
        std::memcpy(output, serial.data(), serial.size());
        output[serial.size()] = '\0';
        *length = serial.size();
        return 0;
    }

    int open(Device device, Handle* handle) noexcept override
    {
        ++open_calls;
        if (open_result != 0) {
            *handle = nullptr;
            return open_result;
        }
        auto* fake = new FakeHandle{static_cast<FakeDevice*>(device), -1};
        *handle = fake;
        events.emplace_back("open");
        return 0;
    }

    int wrap_sys_device(Context, std::intptr_t fd, Handle* handle) noexcept override
    {
        ++wrap_calls;
        wrapped_fds.push_back(fd);
        if (wrapped_fd_sink != nullptr) {
            wrapped_fd_sink->push_back(fd);
        }
        if (wrap_result != 0 &&
            (wrap_fail_after == 0U || wrap_calls >= wrap_fail_after)) {
            *handle = nullptr;
            return wrap_result;
        }
        FakeDevice* device = wrapped_device;
        if (wrap_calls <= wrapped_devices.size()) {
            device = wrapped_devices[wrap_calls - 1U];
        }
        *handle = new FakeHandle{device, fd};
        events.emplace_back("wrap");
        return 0;
    }

    int claim_interface(Handle handle, int) noexcept override
    {
        ++claim_calls;
        claim_devices.push_back(static_cast<FakeHandle*>(handle)->device);
        events.emplace_back("claim");
        record_lifecycle("claim");
        if (claim_result != 0 &&
            (claim_fail_after == 0U || claim_calls >= claim_fail_after)) {
            return claim_result;
        }
        return 0;
    }

    int release_interface(Handle, int) noexcept override
    {
        ++release_calls;
        events.emplace_back("release");
        record_lifecycle("release");
        return 0;
    }

    void close(Handle handle) noexcept override
    {
        ++close_calls;
        if (close_count_sink != nullptr) {
            ++*close_count_sink;
        }
        events.emplace_back("close");
#if defined(__linux__) || defined(__ANDROID__)
        const auto* fake = static_cast<FakeHandle*>(handle);
        if (fake != nullptr && fake->fd >= 0) {
            const bool valid = ::fcntl(static_cast<int>(fake->fd), F_GETFD) >= 0;
            fd_valid_at_close.push_back(valid);
            if (fd_valid_at_close_sink != nullptr) {
                fd_valid_at_close_sink->push_back(valid);
            }
        }
#endif
        record_lifecycle("close");
        delete static_cast<FakeHandle*>(handle);
    }

    int bulk_transfer(Handle, std::uint8_t endpoint, std::uint8_t* buffer, int length,
                      int* transferred, unsigned int) noexcept override
    {
        enter_bulk_gate();
        ++bulk_calls;
        endpoints.push_back(endpoint);
        lengths.push_back(length);
        *transferred = bulk_transferred != -1 ? bulk_transferred
                                             : (bulk_result == 0 ? length : 0);
        if (length > 0 && buffer != nullptr) {
            if (bulk_payload.empty()) {
                buffer[0] = 0xa5U;
            } else {
                const std::size_t copy_size = std::min<std::size_t>(
                    bulk_payload.size(), static_cast<std::size_t>(length));
                std::memcpy(buffer, bulk_payload.data(), copy_size);
            }
        }
        if (dispatch_stream_callbacks_in_bulk) {
            // libusb synchronous transfers are implemented on top of the
            // shared async event machinery.  Model a card command reaping all
            // currently submitted stream URBs before that command completes.
            for (FakeTransfer* transfer : transfers_) {
                if (!transfer->submitted) continue;
                transfer->submitted = false;
                if (transfer->buffer != nullptr) transfer->buffer[0U] = transfer->marker;
                ++callback_calls;
                transfer->callback(static_cast<Transfer>(transfer), completion_status,
                                   completion_length, transfer->context);
            }
            std::unique_lock<std::mutex> lock(bulk_stream_mutex_);
            bulk_stream_callbacks_done_ = true;
            bulk_stream_changed_.notify_all();
            bulk_stream_changed_.wait(
                lock, [this] { return release_bulk_after_callbacks_; });
        }
        const int result = bulk_result;
        leave_bulk_gate();
        return result;
    }

    Transfer alloc_transfer() noexcept override
    {
        ++alloc_calls;
        return new FakeTransfer;
    }

    void fill_bulk_transfer(Transfer transfer, Handle, std::uint8_t, std::uint8_t* buffer, int,
                            TransferCallback callback, void* context,
                            unsigned int) noexcept override
    {
        auto* fake = static_cast<FakeTransfer*>(transfer);
        fake->callback = callback;
        fake->context = context;
        fake->buffer = buffer;
        if (std::find(transfers_.begin(), transfers_.end(), fake) == transfers_.end()) {
            fake->marker = static_cast<std::uint8_t>(transfers_.size() + 1U);
            transfers_.push_back(fake);
        }
    }

    int submit_transfer(Transfer transfer) noexcept override
    {
        enter_lifecycle_gate();
        ++submit_calls;
        if (submit_result != 0 &&
            (submit_fail_after == 0U || submit_calls >= submit_fail_after)) {
            leave_lifecycle_gate();
            return submit_result;
        }
        auto* fake = static_cast<FakeTransfer*>(transfer);
        fake->submitted = true;
        fake->submission_order = next_submission_order_++;
        leave_lifecycle_gate();
        return 0;
    }

    int cancel_transfer(Transfer transfer) noexcept override
    {
        enter_lifecycle_gate();
        ++cancel_calls;
        auto* fake = static_cast<FakeTransfer*>(transfer);
        if (cancel_returns_not_found) {
            fake->submitted = false;
            fake->callback_pending = callback_after_not_found;
            leave_lifecycle_gate();
            return LIBUSB_ERROR_NOT_FOUND;
        }
        if (!fake->submitted) {
            leave_lifecycle_gate();
            return LIBUSB_ERROR_NOT_FOUND;
        }
        fake->submitted = false;
        if (cancel_callbacks) {
            fake->callback(transfer, TransferStatus::cancelled, 0, fake->context);
        }
        leave_lifecycle_gate();
        return 0;
    }

    void free_transfer(Transfer transfer) noexcept override
    {
        ++free_calls;
        delete static_cast<FakeTransfer*>(transfer);
    }

    int handle_events(Context, unsigned int) noexcept override
    {
        enter_lifecycle_gate();
        ++event_calls;
        if (complete_events) {
            auto complete = [this](FakeTransfer* transfer) noexcept {
                if (transfer->submitted || transfer->callback_pending) {
                    transfer->submitted = false;
                    transfer->callback_pending = false;
                    if (transfer->buffer != nullptr) transfer->buffer[0U] = transfer->marker;
                    ++callback_calls;
                    transfer->callback(static_cast<Transfer>(transfer), completion_status,
                                       completion_length, transfer->context);
                    return true;
                }
                return false;
            };
            if (complete_events_reverse) {
                for (auto iterator = transfers_.rbegin(); iterator != transfers_.rend(); ++iterator) {
                    if (complete(*iterator) && !complete_all_events) break;
                }
            } else if (!complete_all_events) {
                FakeTransfer* next = nullptr;
                for (FakeTransfer* transfer : transfers_) {
                    if (!(transfer->submitted || transfer->callback_pending)) continue;
                    if (next == nullptr ||
                        transfer->submission_order < next->submission_order)
                        next = transfer;
                }
                if (next != nullptr) (void)complete(next);
            } else {
                for (FakeTransfer* transfer : transfers_) {
                    (void)complete(transfer);
                }
            }
        }
        leave_lifecycle_gate();
        return event_result;
    }

    void remember_transfer(Transfer transfer) { transfers_.push_back(static_cast<FakeTransfer*>(transfer)); }

private:
    std::mutex bulk_stream_mutex_;
    std::condition_variable bulk_stream_changed_;
    bool bulk_stream_callbacks_done_ = false;
    bool release_bulk_after_callbacks_ = false;
    std::vector<FakeTransfer*> transfers_;
    std::uint64_t next_submission_order_ = 0U;
};

class FakeFdSyscalls final : public FdSyscalls {
public:
    bool valid_result = true;
    int duplicate_result = 100;
    std::vector<int> valid_fds;
    std::vector<int> duplicated_fds;

    bool valid(int fd) noexcept override
    {
        valid_fds.push_back(fd);
        return valid_result && fd >= 0;
    }
    int duplicate(int fd) noexcept override
    {
        duplicated_fds.push_back(fd);
        return duplicate_result;
    }
};

#if defined(__linux__) || defined(__ANDROID__)
class TrackingFdSyscalls final : public FdSyscalls {
public:
    ~TrackingFdSyscalls() noexcept override
    {
        if (destroyed_sink != nullptr) {
            *destroyed_sink = true;
        }
    }

    std::size_t duplicate_fail_after = 0U;
    std::size_t duplicate_calls = 0U;
    std::vector<int> duplicates;
    std::vector<int>* duplicate_sink = nullptr;
    bool* destroyed_sink = nullptr;

    bool valid(int fd) noexcept override { return ::fcntl(fd, F_GETFD) >= 0; }

    int duplicate(int fd) noexcept override
    {
        ++duplicate_calls;
        if (duplicate_fail_after != 0U && duplicate_calls >= duplicate_fail_after) {
            return -1;
        }
        const int result = ::dup(fd);
        if (result >= 0) {
            (void)::fcntl(result, F_SETFD, FD_CLOEXEC);
            duplicates.push_back(result);
            if (duplicate_sink != nullptr) {
                duplicate_sink->push_back(result);
            }
        }
        return result;
    }
};

struct TestPipe final {
    int fds[2] = {-1, -1};

    TestPipe() noexcept
    {
        if (::pipe(fds) != 0) {
            fds[0] = -1;
            fds[1] = -1;
        }
    }
    ~TestPipe() noexcept
    {
        if (fds[0] >= 0) {
            (void)::close(fds[0]);
        }
        if (fds[1] >= 0) {
            (void)::close(fds[1]);
        }
    }
    TestPipe(const TestPipe&) = delete;
    TestPipe& operator=(const TestPipe&) = delete;
};
#endif

FakeDevice fake_device(const char* base, std::uint8_t dev_id)
{
    FakeDevice result;
    result.observation = observation(base, dev_id);
    return result;
}

bool discover_fake(FakeApi& api, FakeDevice& first, FakeDevice& second,
                   std::unique_ptr<LibusbTransport>* transport = nullptr)
{
    api.devices = {&first, &second};
    DeviceDiscovery discovery;
    NativeEnumerator enumerator(api, reinterpret_cast<void*>(static_cast<std::uintptr_t>(1U)));
    CHECK(enumerator.discover(discovery));
    CHECK(api.claim_calls == 0U);
    CHECK(discovery.candidates().size() == 2U);
    CHECK(discovery.candidates()[0].status == ObservationStatus::usable);
    if (transport != nullptr) {
        auto opened = enumerator.open_and_claim(discovery.candidates()[0]);
        CHECK(opened);
        *transport = std::move(opened.value());
        CHECK(api.claim_calls == 1U);
    }
    return true;
}

bool test_bulk_transport_and_mapping()
{
    FakeApi api;
    FakeDevice first = fake_device("00000000000010", 1U);
    FakeDevice second = fake_device("00000000000010", 2U);
    std::unique_ptr<LibusbTransport> transport;
    CHECK(discover_fake(api, first, second, &transport));

    std::unique_ptr<std::uint8_t[]> max(new std::uint8_t[kMaxCommandTransfer]);
    CHECK(transport->bulk_read(0x81U, MutableByteView{max.get(), kMaxCommandTransfer}, Timeout{1U}));
    CHECK(transport->bulk_write(0x02U, ByteView{max.get(), kMaxCommandTransfer}, Timeout{1U}));
    CHECK(api.endpoints[0] == 0x81U && api.endpoints[1] == 0x02U);
    CHECK(api.lengths[0] == 65535 && api.lengths[1] == 65535);
    CHECK(transport->bulk_read(0x02U, MutableByteView{max.get(), 1U}, Timeout{1U}).error() ==
          Error::INVALID_ARGUMENT);
    CHECK(transport->bulk_write(0x81U, ByteView{max.get(), 1U}, Timeout{1U}).error() ==
          Error::INVALID_ARGUMENT);
    CHECK(transport->bulk_read(0x81U, MutableByteView{max.get(), kMaxCommandTransfer + 1U}, Timeout{1U}).error() ==
          Error::BUFFER_TOO_SMALL);
    CHECK(transport->bulk_write(0x02U, ByteView{max.get(), kMaxCommandTransfer + 1U}, Timeout{1U}).error() ==
          Error::BUFFER_TOO_SMALL);
    CHECK(transport->bulk_read(0x81U, MutableByteView{nullptr, 1U}, Timeout{1U}).error() ==
          Error::INVALID_ARGUMENT);
    CHECK(transport->bulk_write(0x02U, ByteView{nullptr, 1U}, Timeout{1U}).error() ==
          Error::INVALID_ARGUMENT);

    const std::array<int, 7U> codes{LIBUSB_ERROR_TIMEOUT, LIBUSB_ERROR_NO_DEVICE,
                                    LIBUSB_ERROR_BUSY, LIBUSB_ERROR_NOT_FOUND,
                                    LIBUSB_ERROR_NOT_SUPPORTED, LIBUSB_ERROR_INVALID_PARAM,
                                    LIBUSB_ERROR_ACCESS};
    const std::array<Error, 7U> errors{Error::TIMEOUT, Error::DISCONNECTED, Error::BUSY,
                                       Error::NOT_FOUND, Error::UNSUPPORTED,
                                       Error::INVALID_ARGUMENT, Error::USB_IO};
    for (std::size_t index = 0U; index < codes.size(); ++index) {
        CHECK(map_libusb_error(codes[index]) == errors[index]);
    }
    api.bulk_result = LIBUSB_ERROR_NO_DEVICE;
    CHECK(transport->bulk_read(0x81U, MutableByteView{max.get(), 1U}, Timeout{1U}).error() ==
          Error::DISCONNECTED);
    api.bulk_result = LIBUSB_ERROR_TIMEOUT;
    api.bulk_transferred = 0;
    BulkReadObservation timeout_zero_observation{Error::USB_IO, 99U};
    const auto timeout_zero = transport->bulk_read(
        0x81U, MutableByteView{max.get(), 1U}, Timeout{1U}, &timeout_zero_observation);
    CHECK(!timeout_zero && timeout_zero.error() == Error::TIMEOUT);
    CHECK(timeout_zero_observation.completion_error == Error::TIMEOUT);
    CHECK(timeout_zero_observation.transferred == 0U);

    api.bulk_result = 0;
    api.bulk_transferred = 188;
    api.bulk_payload.assign(188U, 0x31U);
    std::array<std::uint8_t, 188U> normal_short{};
    BulkReadObservation normal_observation{Error::USB_IO, 99U};
    const auto normal_read = transport->bulk_read(
        0x81U, MutableByteView{normal_short.data(), normal_short.size()}, Timeout{1U},
        &normal_observation);
    CHECK(normal_read && normal_read.value() == normal_short.size());
    CHECK(normal_observation.completion_error == Error::OK);
    CHECK(normal_observation.transferred == normal_short.size());

    api.bulk_result = LIBUSB_ERROR_TIMEOUT;
    api.bulk_transferred = 512;
    api.bulk_payload.assign(512U, 0x32U);
    std::array<std::uint8_t, 512U> partial{};
    BulkReadObservation partial_observation{Error::USB_IO, 99U};
    const auto partial_read = transport->bulk_read(
        0x81U, MutableByteView{partial.data(), partial.size()}, Timeout{1U},
        &partial_observation);
    CHECK(partial_read && partial_read.value() == partial.size());
    CHECK(partial_observation.completion_error == Error::TIMEOUT);
    CHECK(partial_observation.transferred == partial.size());

    api.bulk_result = 0;
    api.bulk_transferred = -2;
    BulkReadObservation negative_observation{Error::OK, 99U};
    const auto negative = transport->bulk_read(
        0x81U, MutableByteView{max.get(), 1U}, Timeout{1U}, &negative_observation);
    CHECK(!negative && negative.error() == Error::INTERNAL);
    CHECK(negative_observation.completion_error == Error::INTERNAL);
    CHECK(negative_observation.transferred == 0U);

    api.bulk_transferred = 2;
    BulkReadObservation oversized_observation{Error::OK, 99U};
    const auto oversized = transport->bulk_read(
        0x81U, MutableByteView{max.get(), 1U}, Timeout{1U}, &oversized_observation);
    CHECK(!oversized && oversized.error() == Error::INTERNAL);
    CHECK(oversized_observation.completion_error == Error::INTERNAL);
    CHECK(oversized_observation.transferred == 0U);
    return true;
}

bool test_not_found_cancel_waits_for_callback()
{
    FakeApi api;
    FakeDevice first = fake_device("00000000000015", 1U);
    FakeDevice second = fake_device("00000000000015", 2U);
    std::unique_ptr<LibusbTransport> transport;
    CHECK(discover_fake(api, first, second, &transport));

    api.cancel_returns_not_found = true;
    api.callback_after_not_found = true;
    api.complete_events = true;
    api.completion_status = LibusbApi::TransferStatus::cancelled;
    CHECK(transport->start_stream(StreamConfig{kTsInEndpoint, 8U, 1U}));
    CHECK(api.free_calls == 0U);
    CHECK(transport->cancel_stream());
    CHECK(api.callback_calls == 1U);
    CHECK(api.event_calls == 1U);
    CHECK(api.free_calls == 1U);
    CHECK(!transport->stream_active());
    return true;
}

bool test_native_enumeration_filters_and_stream_lifecycle()
{
    FakeApi api;
    FakeDevice first = fake_device("00000000000010", 1U);
    FakeDevice second = fake_device("00000000000010", 2U);
    FakeDevice foreign = fake_device("00000000000020", 1U);
    foreign.observation.vendor_id = 0x1234U;
    api.devices = {&foreign, &second, &first};
    DeviceDiscovery discovery;
    NativeEnumerator enumerator(api, reinterpret_cast<void*>(static_cast<std::uintptr_t>(2U)));
    CHECK(enumerator.discover(discovery));
    CHECK(api.open_calls == 2U);
    CHECK(api.claim_calls == 0U);
    CHECK(api.config_calls == 2U && api.free_config_calls == 2U);
    CHECK(api.serial_calls == 2U);
    CHECK(discovery.candidates().size() == 2U);
    std::unique_ptr<LibusbTransport> transport;
    auto opened = enumerator.open_and_claim(discovery.candidates()[0]);
    CHECK(opened);
    transport = std::move(opened.value());
    CHECK(api.claim_calls == 1U);

    api.complete_events = true;
    const StreamConfig config{0x84U, 8U, 2U};
    CHECK(transport->start_stream(config));
    CHECK(transport->stream_active());
    auto event = transport->wait_stream(Timeout{20U});
    CHECK(event);
    CHECK(event.value().size == 4U);
    CHECK(event.value().kind == StreamEventKind::short_transfer);
    CHECK(transport->wait_stream(Timeout{20U}));
    CHECK(transport->stop_stream());
    CHECK(!transport->stream_active());
    CHECK(api.free_calls == 2U);

    FakeApi timeout_api;
    FakeDevice timeout_device = fake_device("00000000000030", 1U);
    FakeDevice timeout_second = fake_device("00000000000030", 2U);
    std::unique_ptr<LibusbTransport> timeout_transport;
    CHECK(discover_fake(timeout_api, timeout_device, timeout_second, &timeout_transport));
    CHECK(timeout_transport->start_stream(config));
    CHECK(timeout_transport->wait_stream(Timeout{1U}).error() == Error::TIMEOUT);
    CHECK(timeout_transport->cancel_stream());

    FakeApi disconnect_api;
    FakeDevice disconnect_device = fake_device("00000000000050", 1U);
    FakeDevice disconnect_second = fake_device("00000000000050", 2U);
    std::unique_ptr<LibusbTransport> disconnect_transport;
    CHECK(discover_fake(disconnect_api, disconnect_device, disconnect_second,
                        &disconnect_transport));
    disconnect_api.complete_events = true;
    disconnect_api.completion_status = LibusbApi::TransferStatus::no_device;
    CHECK(disconnect_transport->start_stream(config));
    CHECK(disconnect_transport->wait_stream(Timeout{1U}).error() == Error::DISCONNECTED);
    CHECK(disconnect_transport->stop_stream());

    FakeApi open_failure_api;
    FakeDevice open_failure_device = fake_device("00000000000060", 1U);
    FakeDevice open_failure_second = fake_device("00000000000060", 2U);
    open_failure_api.devices = {&open_failure_device, &open_failure_second};
    open_failure_api.open_result = LIBUSB_ERROR_IO;
    DeviceDiscovery open_failure_discovery;
    NativeEnumerator open_failure_enumerator(
        open_failure_api, reinterpret_cast<void*>(static_cast<std::uintptr_t>(7U)));
    CHECK(open_failure_enumerator.discover(open_failure_discovery));
    CHECK(open_failure_discovery.candidates()[0].status == ObservationStatus::open_failed);
    CHECK(open_failure_discovery.candidates()[0].discovery_error == Error::USB_IO);

    FakeApi read_failure_api;
    FakeDevice read_failure_device = fake_device("00000000000070", 1U);
    FakeDevice read_failure_second = fake_device("00000000000070", 2U);
    read_failure_api.devices = {&read_failure_device, &read_failure_second};
    read_failure_api.serial_result = LIBUSB_ERROR_IO;
    DeviceDiscovery read_failure_discovery;
    NativeEnumerator read_failure_enumerator(
        read_failure_api, reinterpret_cast<void*>(static_cast<std::uintptr_t>(8U)));
    CHECK(read_failure_enumerator.discover(read_failure_discovery));
    CHECK(read_failure_discovery.candidates()[0].status == ObservationStatus::invalid_serial);
    CHECK(read_failure_discovery.candidates()[0].discovery_error == Error::USB_IO);

    FakeApi missing_serial_api;
    FakeDevice missing_serial_device = fake_device("00000000000080", 1U);
    missing_serial_device.serial_index = 0U;
    FakeDevice missing_serial_second = fake_device("00000000000080", 2U);
    missing_serial_api.devices = {&missing_serial_device, &missing_serial_second};
    DeviceDiscovery missing_serial_discovery;
    NativeEnumerator missing_serial_enumerator(
        missing_serial_api, reinterpret_cast<void*>(static_cast<std::uintptr_t>(9U)));
    CHECK(missing_serial_enumerator.discover(missing_serial_discovery));
    CHECK(missing_serial_discovery.candidates()[0].status == ObservationStatus::invalid_serial);
    CHECK(missing_serial_api.serial_calls == 1U);
    return true;
}

bool test_stream_completion_submission_order()
{
    FakeApi api;
    FakeDevice first = fake_device("00000000000016", 1U);
    FakeDevice second = fake_device("00000000000016", 2U);
    std::unique_ptr<LibusbTransport> transport;
    CHECK(discover_fake(api, first, second, &transport));
    api.complete_events = true;
    api.complete_all_events = true;
    api.complete_events_reverse = true;
    api.completion_length = 4;
    CHECK(transport->start_stream(StreamConfig{kTsInEndpoint, 8U, 3U}));

    const auto first_completed = transport->wait_stream(Timeout{0U});
    CHECK(first_completed && first_completed.value().data[0U] == 1U);
    const auto second_completed = transport->wait_stream(Timeout{0U});
    CHECK(second_completed && second_completed.value().data[0U] == 2U);
    const auto third_completed = transport->wait_stream(Timeout{0U});
    CHECK(third_completed && third_completed.value().data[0U] == 3U);
    CHECK(transport->stop_stream());
    return true;
}

bool test_fail_safe_abandonment()
{
#if !defined(__linux__) && !defined(__ANDROID__)
    return true;
#else
    FakeApi api;
    FakeDevice wrapped = fake_device("00000000000090", 1U);
    api.wrapped_device = &wrapped;
    api.event_result = LIBUSB_ERROR_IO;
    api.cancel_returns_not_found = true;
    api.callback_after_not_found = false;
    int pipe_fds[2] = {-1, -1};
    CHECK(::pipe(pipe_fds) == 0);
    {
        NativeFdSyscalls syscalls;
        FdTransportFactory factory(
            api, reinterpret_cast<void*>(static_cast<std::uintptr_t>(11U)), syscalls);
        auto transport = factory.wrap_and_claim(pipe_fds[0]);
        CHECK(transport);
        CHECK(transport.value()->start_stream(StreamConfig{kTsInEndpoint, 8U, 2U}));
        CHECK(transport.value()->cancel_stream().error() == Error::USB_IO);
        CHECK(api.event_calls == 8U);
    }
    CHECK(api.free_calls == 0U);
    CHECK(api.callback_calls == 0U);
    CHECK(api.close_calls == 0U);
    CHECK(api.release_calls == 0U);
    CHECK(api.wrapped_fds.size() == 1U);
    CHECK(::fcntl(static_cast<int>(api.wrapped_fds[0]), F_GETFD) >= 0);
    CHECK(::close(static_cast<int>(api.wrapped_fds[0])) == 0);
    CHECK(::close(pipe_fds[0]) == 0);
    CHECK(::close(pipe_fds[1]) == 0);
    return true;
#endif
}

bool test_fd_ownership_and_init_mode()
{
#if !defined(__linux__) && !defined(__ANDROID__)
    return true;
#else
    FakeApi api;
    FakeDevice wrapped = fake_device("00000000000040", 1U);
    api.wrapped_device = &wrapped;
    int pipe_fds[2] = {-1, -1};
    CHECK(::pipe(pipe_fds) == 0);
    std::unique_ptr<LibusbTransport> transport;
    {
        NativeFdSyscalls syscalls;
        FdTransportFactory factory(api, reinterpret_cast<void*>(static_cast<std::uintptr_t>(3U)),
                                   syscalls);
        auto opened = factory.wrap_and_claim(pipe_fds[0]);
        CHECK(opened);
        transport = std::move(opened.value());
    }
    CHECK(::fcntl(pipe_fds[0], F_GETFD) >= 0);
    CHECK(api.wrapped_fds.size() == 1U);
    CHECK(::fcntl(static_cast<int>(api.wrapped_fds[0]), F_GETFD) >= 0);
    transport.reset();
    CHECK(api.events.size() >= 3U);
    CHECK(api.events[api.events.size() - 2U] == "release");
    CHECK(api.events[api.events.size() - 1U] == "close");
    CHECK(::fcntl(static_cast<int>(api.wrapped_fds[0]), F_GETFD) == -1);

    FakeApi failed_wrap_api;
    failed_wrap_api.wrapped_device = &wrapped;
    failed_wrap_api.wrap_result = LIBUSB_ERROR_IO;
    int failed_pipe[2] = {-1, -1};
    CHECK(::pipe(failed_pipe) == 0);
    {
        NativeFdSyscalls failed_wrap_syscalls;
        FdTransportFactory failed_wrap(
            failed_wrap_api, reinterpret_cast<void*>(static_cast<std::uintptr_t>(4U)),
            failed_wrap_syscalls);
        CHECK(failed_wrap.wrap_and_claim(failed_pipe[0]).error() == Error::USB_IO);
        CHECK(failed_wrap.wrap_and_claim(failed_pipe[0]).error() == Error::USB_IO);
    }
    CHECK(failed_wrap_api.wrapped_fds.size() == 2U);
    CHECK(::fcntl(failed_pipe[0], F_GETFD) >= 0);
    for (const std::intptr_t fd : failed_wrap_api.wrapped_fds) {
        CHECK(::fcntl(static_cast<int>(fd), F_GETFD) == -1);
    }
    CHECK(::close(failed_pipe[0]) == 0);
    CHECK(::close(failed_pipe[1]) == 0);

    FakeApi failed_duplicate_api;
    failed_duplicate_api.wrapped_device = &wrapped;
    FakeFdSyscalls failed_duplicate_syscalls;
    failed_duplicate_syscalls.duplicate_result = -1;
    FdTransportFactory failed_duplicate(
        failed_duplicate_api, reinterpret_cast<void*>(static_cast<std::uintptr_t>(6U)),
        failed_duplicate_syscalls);
    CHECK(failed_duplicate.wrap_and_claim(10).error() == Error::USB_IO);
    CHECK(failed_duplicate_api.wrap_calls == 0U);

    FakeApi failed_claim_api;
    failed_claim_api.wrapped_device = &wrapped;
    failed_claim_api.claim_result = LIBUSB_ERROR_BUSY;
    int claim_pipe[2] = {-1, -1};
    CHECK(::pipe(claim_pipe) == 0);
    {
        NativeFdSyscalls failed_claim_syscalls;
        FdTransportFactory failed_claim(
            failed_claim_api, reinterpret_cast<void*>(static_cast<std::uintptr_t>(5U)),
            failed_claim_syscalls);
        CHECK(failed_claim.wrap_and_claim(claim_pipe[0]).error() == Error::BUSY);
        CHECK(failed_claim.wrap_and_claim(claim_pipe[0]).error() == Error::BUSY);
    }
    CHECK(failed_claim_api.wrapped_fds.size() == 2U);
    CHECK(::fcntl(claim_pipe[0], F_GETFD) >= 0);
    for (const std::intptr_t fd : failed_claim_api.wrapped_fds) {
        CHECK(::fcntl(static_cast<int>(fd), F_GETFD) == -1);
    }
    CHECK(::close(claim_pipe[0]) == 0);
    CHECK(::close(claim_pipe[1]) == 0);

    FakeApi failed_access_api;
    failed_access_api.wrapped_device = &wrapped;
    failed_access_api.claim_result = LIBUSB_ERROR_ACCESS;
    int access_pipe[2] = {-1, -1};
    CHECK(::pipe(access_pipe) == 0);
    {
        NativeFdSyscalls access_syscalls;
        FdTransportFactory failed_access(
            failed_access_api, reinterpret_cast<void*>(static_cast<std::uintptr_t>(10U)),
            access_syscalls);
        CHECK(failed_access.wrap_and_claim(access_pipe[0]).error() == Error::USB_IO);
    }
    CHECK(::close(access_pipe[0]) == 0);
    CHECK(::close(access_pipe[1]) == 0);

    FakeApi partial_submit_api;
    partial_submit_api.wrapped_device = &wrapped;
    partial_submit_api.submit_result = LIBUSB_ERROR_IO;
    partial_submit_api.submit_fail_after = 2U;
    {
        NativeFdSyscalls partial_syscalls;
        FdTransportFactory partial_factory(
            partial_submit_api, reinterpret_cast<void*>(static_cast<std::uintptr_t>(12U)),
            partial_syscalls);
        auto partial = partial_factory.wrap_and_claim(pipe_fds[0]);
        CHECK(partial);
        CHECK(partial.value()->start_stream(StreamConfig{kTsInEndpoint, 8U, 3U}).error() ==
              Error::USB_IO);
        CHECK(partial_syscalls.valid(pipe_fds[0]));
        CHECK(partial_submit_api.alloc_calls == 2U);
        CHECK(partial_submit_api.free_calls == 2U);
    }

    auto session = LibusbSession::create(api, true);
    CHECK(session);
    CHECK(api.init_calls == 1U && api.init_no_discovery);
    CHECK(::close(pipe_fds[0]) == 0);
    CHECK(::close(pipe_fds[1]) == 0);
    return true;
#endif
}

bool test_fd_enclosure_batch()
{
#if !defined(__linux__) && !defined(__ANDROID__)
    return true;
#else
    TestPipe first_pipe;
    TestPipe second_pipe;
    CHECK(first_pipe.fds[0] >= 0 && second_pipe.fds[0] >= 0);
    FakeDevice first = fake_device("00000000000010", 1U);
    FakeDevice second = fake_device("00000000000010", 2U);
    FakeApi api;
    api.wrapped_devices = {&second, &first};
    TrackingFdSyscalls syscalls;
    auto session = LibusbSession::create(api, true);
    CHECK(session && api.init_no_discovery);
    FdTransportFactory factory(api, session.value()->context(), syscalls);
    auto enclosure = factory.wrap_and_claim_enclosure(
        std::vector<int>{first_pipe.fds[0], second_pipe.fds[0]});
    CHECK(enclosure);
    CHECK(enclosure.value()->base_serial == "00000000000010");
    CHECK(enclosure.value()->transports[0U] != nullptr);
    CHECK(enclosure.value()->transports[1U] != nullptr);
    CHECK(api.claim_devices.size() == 2U);
    CHECK(api.claim_devices[0U] == &first && api.claim_devices[1U] == &second);
    CHECK(api.get_device_list_calls == 0U);
    for (const int fd : syscalls.duplicates) {
        CHECK(::fcntl(fd, F_GETFD) >= 0);
    }
    enclosure.value().reset();
    CHECK(syscalls.duplicates.size() == 2U);
    for (const int fd : syscalls.duplicates) {
        CHECK(::fcntl(fd, F_GETFD) == -1);
    }
    CHECK(api.fd_valid_at_close.size() == 2U);
    CHECK(api.fd_valid_at_close[0U] && api.fd_valid_at_close[1U]);

    FakeApi duplicate_api;
    TrackingFdSyscalls duplicate_syscalls;
    FdTransportFactory duplicate_factory(
        duplicate_api, reinterpret_cast<void*>(static_cast<std::uintptr_t>(20U)),
        duplicate_syscalls);
    const auto duplicate_result = duplicate_factory.wrap_and_claim_enclosure(
        std::vector<int>{first_pipe.fds[0], first_pipe.fds[0]});
    CHECK(duplicate_result.error() == Error::INVALID_ARGUMENT);
    CHECK(duplicate_syscalls.duplicate_calls == 0U);
    CHECK(duplicate_api.wrap_calls == 0U && duplicate_api.claim_calls == 0U &&
          duplicate_api.init_calls == 0U);

    FakeApi incomplete_api;
    FakeDevice incomplete_device = fake_device("00000000000020", 1U);
    incomplete_api.wrapped_devices = {&incomplete_device};
    TrackingFdSyscalls incomplete_syscalls;
    FdTransportFactory incomplete_factory(
        incomplete_api, reinterpret_cast<void*>(static_cast<std::uintptr_t>(21U)),
        incomplete_syscalls);
    const auto incomplete_result = incomplete_factory.wrap_and_claim_enclosure(
        std::vector<int>{first_pipe.fds[0]});
    CHECK(incomplete_result.error() == Error::NOT_FOUND);
    CHECK(incomplete_api.claim_calls == 0U && incomplete_syscalls.duplicates.size() == 1U);
    CHECK(::fcntl(incomplete_syscalls.duplicates[0U], F_GETFD) == -1);

    FakeApi mismatched_api;
    FakeDevice mismatched_first = fake_device("00000000000030", 1U);
    FakeDevice mismatched_second = fake_device("00000000000031", 2U);
    mismatched_api.wrapped_devices = {&mismatched_first, &mismatched_second};
    TrackingFdSyscalls mismatched_syscalls;
    FdTransportFactory mismatched_factory(
        mismatched_api, reinterpret_cast<void*>(static_cast<std::uintptr_t>(22U)),
        mismatched_syscalls);
    const auto mismatched_result = mismatched_factory.wrap_and_claim_enclosure(
        std::vector<int>{first_pipe.fds[0], second_pipe.fds[0]});
    CHECK(mismatched_result.error() == Error::NOT_FOUND);
    CHECK(mismatched_api.claim_calls == 0U);

    std::array<TestPipe, 4U> enclosure_pipes;
    for (const TestPipe& pipe : enclosure_pipes) {
        CHECK(pipe.fds[0] >= 0);
    }
    FakeDevice enclosure_a1 = fake_device("00000000000040", 1U);
    FakeDevice enclosure_a2 = fake_device("00000000000040", 2U);
    FakeDevice enclosure_b1 = fake_device("00000000000050", 1U);
    FakeDevice enclosure_b2 = fake_device("00000000000050", 2U);
    FakeApi ambiguous_api;
    ambiguous_api.wrapped_devices = {&enclosure_a1, &enclosure_b2, &enclosure_b1, &enclosure_a2};
    TrackingFdSyscalls ambiguous_syscalls;
    FdTransportFactory ambiguous_factory(
        ambiguous_api, reinterpret_cast<void*>(static_cast<std::uintptr_t>(23U)),
        ambiguous_syscalls);
    std::vector<int> ambiguous_fds;
    for (const TestPipe& pipe : enclosure_pipes) {
        ambiguous_fds.push_back(pipe.fds[0]);
    }
    CHECK(ambiguous_factory.wrap_and_claim_enclosure(ambiguous_fds).error() ==
          Error::INVALID_ARGUMENT);
    CHECK(ambiguous_api.claim_calls == 0U);

    FakeApi selected_api;
    selected_api.wrapped_devices = {&enclosure_a2, &enclosure_b1, &enclosure_a1, &enclosure_b2};
    TrackingFdSyscalls selected_syscalls;
    FdTransportFactory selected_factory(
        selected_api, reinterpret_cast<void*>(static_cast<std::uintptr_t>(24U)),
        selected_syscalls);
    auto selected_result = selected_factory.wrap_and_claim_enclosure(
        ambiguous_fds, "00000000000050");
    CHECK(selected_result);
    CHECK(selected_api.claim_devices.size() == 2U);
    CHECK(selected_api.claim_devices[0U] == &enclosure_b1 &&
          selected_api.claim_devices[1U] == &enclosure_b2);
    selected_result.value().reset();

    FakeApi invalid_extra_api;
    FakeDevice invalid_extra = fake_device("00000000000060", 1U);
    invalid_extra.observation.vendor_id = 0x1234U;
    invalid_extra_api.wrapped_devices = {&first, &second, &invalid_extra};
    TrackingFdSyscalls invalid_extra_syscalls;
    FdTransportFactory invalid_extra_factory(
        invalid_extra_api, reinterpret_cast<void*>(static_cast<std::uintptr_t>(25U)),
        invalid_extra_syscalls);
    CHECK(invalid_extra_factory.wrap_and_claim_enclosure(
              std::vector<int>{first_pipe.fds[0], second_pipe.fds[0], enclosure_pipes[0].fds[0]})
              .error() == Error::UNSUPPORTED);
    CHECK(invalid_extra_api.claim_calls == 0U);

    FakeApi duplicate_failure_api;
    duplicate_failure_api.wrapped_devices = {&first, &second};
    TrackingFdSyscalls duplicate_failure_syscalls;
    duplicate_failure_syscalls.duplicate_fail_after = 2U;
    FdTransportFactory duplicate_failure_factory(
        duplicate_failure_api, reinterpret_cast<void*>(static_cast<std::uintptr_t>(26U)),
        duplicate_failure_syscalls);
    CHECK(duplicate_failure_factory.wrap_and_claim_enclosure(
              std::vector<int>{first_pipe.fds[0], second_pipe.fds[0]})
              .error() == Error::USB_IO);
    CHECK(duplicate_failure_api.wrap_calls == 1U && duplicate_failure_api.claim_calls == 0U);
    CHECK(duplicate_failure_syscalls.duplicates.size() == 1U);
    CHECK(::fcntl(duplicate_failure_syscalls.duplicates[0U], F_GETFD) == -1);

    FakeApi wrap_failure_api;
    wrap_failure_api.wrapped_devices = {&first, &second};
    wrap_failure_api.wrap_result = LIBUSB_ERROR_IO;
    wrap_failure_api.wrap_fail_after = 2U;
    TrackingFdSyscalls wrap_failure_syscalls;
    FdTransportFactory wrap_failure_factory(
        wrap_failure_api, reinterpret_cast<void*>(static_cast<std::uintptr_t>(27U)),
        wrap_failure_syscalls);
    CHECK(wrap_failure_factory.wrap_and_claim_enclosure(
              std::vector<int>{first_pipe.fds[0], second_pipe.fds[0]})
              .error() == Error::USB_IO);
    CHECK(wrap_failure_api.close_calls == 1U && wrap_failure_api.claim_calls == 0U);
    for (const int fd : wrap_failure_syscalls.duplicates) {
        CHECK(::fcntl(fd, F_GETFD) == -1);
    }

    FakeApi observation_failure_api;
    observation_failure_api.wrapped_devices = {&first, &second};
    observation_failure_api.serial_result = LIBUSB_ERROR_IO;
    TrackingFdSyscalls observation_failure_syscalls;
    FdTransportFactory observation_failure_factory(
        observation_failure_api, reinterpret_cast<void*>(static_cast<std::uintptr_t>(28U)),
        observation_failure_syscalls);
    CHECK(observation_failure_factory.wrap_and_claim_enclosure(
              std::vector<int>{first_pipe.fds[0], second_pipe.fds[0]})
              .error() == Error::USB_IO);
    CHECK(observation_failure_api.claim_calls == 0U && observation_failure_api.close_calls == 1U);

    FakeApi topology_failure_api;
    topology_failure_api.wrapped_devices = {&first, &second};
    topology_failure_api.config_result = LIBUSB_ERROR_IO;
    TrackingFdSyscalls topology_failure_syscalls;
    FdTransportFactory topology_failure_factory(
        topology_failure_api, reinterpret_cast<void*>(static_cast<std::uintptr_t>(29U)),
        topology_failure_syscalls);
    CHECK(topology_failure_factory.wrap_and_claim_enclosure(
              std::vector<int>{first_pipe.fds[0], second_pipe.fds[0]})
              .error() == Error::USB_IO);
    CHECK(topology_failure_api.claim_calls == 0U);

    FakeApi first_claim_api;
    first_claim_api.wrapped_devices = {&first, &second};
    first_claim_api.claim_result = LIBUSB_ERROR_BUSY;
    TrackingFdSyscalls first_claim_syscalls;
    FdTransportFactory first_claim_factory(
        first_claim_api, reinterpret_cast<void*>(static_cast<std::uintptr_t>(30U)),
        first_claim_syscalls);
    CHECK(first_claim_factory.wrap_and_claim_enclosure(
              std::vector<int>{first_pipe.fds[0], second_pipe.fds[0]})
              .error() == Error::BUSY);
    CHECK(first_claim_api.claim_calls == 1U && first_claim_api.release_calls == 0U &&
          first_claim_api.close_calls == 2U);

    FakeApi second_claim_api;
    second_claim_api.wrapped_devices = {&first, &second};
    second_claim_api.claim_result = LIBUSB_ERROR_BUSY;
    second_claim_api.claim_fail_after = 2U;
    TrackingFdSyscalls second_claim_syscalls;
    FdTransportFactory second_claim_factory(
        second_claim_api, reinterpret_cast<void*>(static_cast<std::uintptr_t>(31U)),
        second_claim_syscalls);
    CHECK(second_claim_factory.wrap_and_claim_enclosure(
              std::vector<int>{first_pipe.fds[0], second_pipe.fds[0]})
              .error() == Error::BUSY);
    CHECK(second_claim_api.claim_calls == 2U && second_claim_api.release_calls == 1U &&
          second_claim_api.close_calls == 2U);
    CHECK(second_claim_api.claim_devices[0U] == &first &&
          second_claim_api.claim_devices[1U] == &second);
    CHECK(second_claim_syscalls.duplicates.size() == 2U);
    for (const int fd : second_claim_syscalls.duplicates) {
        CHECK(::fcntl(fd, F_GETFD) == -1);
    }
    return true;
#endif
}

bool test_native_enumeration_reports_unopened_devices()
{
    // `px4d --list` shows why a supported device is missing from the
    // enclosures.  A root-owned USB node must read as open_failed, not as a
    // bad serial derived from the string that could not be read.
    auto api = std::unique_ptr<FakeApi>(new FakeApi);
    FakeDevice first = fake_device("00000000000074", 1U);
    FakeDevice second = fake_device("00000000000074", 2U);
    // As with real libusb, the serial string is never read when open fails.
    first.observation.serial.clear();
    second.observation.serial.clear();
    api->devices = {&first, &second};
    api->open_result = LIBUSB_ERROR_ACCESS;
    const auto denied = RuntimeTestAccess::enumerate_native(std::move(api));
    CHECK(denied);
    CHECK(denied.value().groups.empty());
    CHECK(denied.value().rejected.size() == 2U);
    for (const RejectedObservation& rejected : denied.value().rejected) {
        CHECK(rejected.status == ObservationStatus::open_failed);
        CHECK(rejected.observation.serial.empty());
    }

    auto readable_api = std::unique_ptr<FakeApi>(new FakeApi);
    FakeDevice readable_first = fake_device("00000000000076", 1U);
    FakeDevice readable_second = fake_device("00000000000076", 2U);
    readable_api->devices = {&readable_second, &readable_first};
    const auto readable = RuntimeTestAccess::enumerate_native(std::move(readable_api));
    CHECK(readable);
    CHECK(readable.value().rejected.empty());
    CHECK(readable.value().groups.size() == 1U);
    CHECK(readable.value().groups[0U].base_serial == "00000000000076");
    CHECK(readable.value().groups[0U].status == GroupStatus::ready);
    return true;
}

bool test_runtime_native_collision_claims_only_selected_paths()
{
    FakeDevice first = fake_device("00000000000010", 1U);
    first.observation.product_id = kPxM1UrProductId;
    first.observation.serial = "000000000000001";
    FakeDevice second = first;
    auto locate = [](FakeDevice& device, std::uint8_t address,
                     std::uint8_t port) {
        device.observation.location.has_bus = true;
        device.observation.location.has_address = true;
        device.observation.location.bus = 1U;
        device.observation.location.address = address;
        device.observation.location.port_count = 1U;
        device.observation.location.port_path[0U] = port;
    };
    locate(first, 30U, 2U);
    locate(second, 31U, 4U);

    std::vector<std::string> rejected_events;
    auto ambiguous_api = std::unique_ptr<FakeApi>(new FakeApi);
    ambiguous_api->devices = {&first, &second};
    ambiguous_api->lifecycle_events = &rejected_events;
    const auto ambiguous = RuntimeTestAccess::open_native(
        std::move(ambiguous_api), first.observation.serial);
    CHECK(!ambiguous && ambiguous.error() == Error::INVALID_ARGUMENT);
    CHECK(std::find(rejected_events.begin(), rejected_events.end(), "claim") ==
          rejected_events.end());

    auto selected_api = std::unique_ptr<FakeApi>(new FakeApi);
    FakeApi* raw = selected_api.get();
    raw->devices = {&first, &second};
    auto selected = RuntimeTestAccess::open_native(
        std::move(selected_api), first.observation.serial, {"1-2"});
    CHECK(selected && selected.value()->model() == DeviceModel::px_m1ur);
    CHECK(raw->claim_devices.size() == 1U && raw->claim_devices[0U] == &first);
    selected.value().reset();

    FakeDevice other_model = second;
    other_model.observation.product_id = kPxS1UrProductId;
    auto mixed_api = std::unique_ptr<FakeApi>(new FakeApi);
    FakeApi* mixed_raw = mixed_api.get();
    mixed_raw->devices = {&first, &other_model};
    auto mixed = RuntimeTestAccess::open_native(
        std::move(mixed_api), first.observation.serial, {"1:31"});
    CHECK(mixed && mixed.value()->model() == DeviceModel::px_s1ur);
    CHECK(mixed_raw->claim_devices.size() == 1U &&
          mixed_raw->claim_devices[0U] == &other_model);
    mixed.value().reset();

    FakeDevice q1a = fake_device("00000000000011", 1U);
    FakeDevice q1b = q1a;
    FakeDevice q2a = fake_device("00000000000011", 2U);
    FakeDevice q2b = q2a;
    locate(q1a, 20U, 1U);
    locate(q1b, 21U, 3U);
    locate(q2a, 22U, 2U);
    locate(q2b, 23U, 4U);
    std::vector<std::string> ambiguous_q3_events;
    auto ambiguous_q3_api = std::unique_ptr<FakeApi>(new FakeApi);
    ambiguous_q3_api->devices = {&q2b, &q1a, &q2a, &q1b};
    ambiguous_q3_api->lifecycle_events = &ambiguous_q3_events;
    const auto ambiguous_q3 = RuntimeTestAccess::open_native(
        std::move(ambiguous_q3_api), "00000000000011");
    CHECK(!ambiguous_q3 && ambiguous_q3.error() == Error::INVALID_ARGUMENT);
    CHECK(std::find(ambiguous_q3_events.begin(), ambiguous_q3_events.end(), "claim") ==
          ambiguous_q3_events.end());
    auto paired_api = std::unique_ptr<FakeApi>(new FakeApi);
    FakeApi* paired_raw = paired_api.get();
    paired_raw->devices = {&q2b, &q1a, &q2a, &q1b};
    auto paired = RuntimeTestAccess::open_native(
        std::move(paired_api), "00000000000011", {"1:23", "1-3"});
    CHECK(paired && paired_raw->claim_devices.size() == 2U);
    CHECK(paired_raw->claim_devices[0U] == &q1b &&
          paired_raw->claim_devices[1U] == &q2b);
    paired.value().reset();
    return true;
}

bool test_runtime_native_transaction_and_ownership()
{
    std::vector<std::string> lifecycle;
    auto api = std::unique_ptr<FakeApi>(new FakeApi);
    FakeApi* api_raw = api.get();
    api_raw->lifecycle_events = &lifecycle;
    FakeDevice first = fake_device("00000000000070", 1U);
    FakeDevice second = fake_device("00000000000070", 2U);
    api_raw->devices = {&second, &first};
    auto runtime = RuntimeTestAccess::open_native(std::move(api));
    CHECK(runtime);
    CHECK(runtime.value()->base_serial() == "00000000000070");
    CHECK(api_raw->claim_devices.size() == 2U);
    CHECK(api_raw->claim_devices[0U] == &first && api_raw->claim_devices[1U] == &second);
    runtime.value().reset();
    CHECK(lifecycle.size() >= 4U);
    const auto exit = std::find(lifecycle.begin(), lifecycle.end(), "exit");
    const auto destroyed = std::find(lifecycle.begin(), lifecycle.end(), "api-destroy");
    CHECK(exit != lifecycle.end() && destroyed != lifecycle.end());
    CHECK(exit < destroyed);
    CHECK(std::find(lifecycle.begin(), exit, "close") != exit);

    std::vector<std::string> failed_lifecycle;
    auto failed_api = std::unique_ptr<FakeApi>(new FakeApi);
    FakeApi* failed_raw = failed_api.get();
    failed_raw->lifecycle_events = &failed_lifecycle;
    failed_raw->devices = {&first, &second};
    failed_raw->claim_fail_after = 2U;
    failed_raw->claim_result = LIBUSB_ERROR_BUSY;
    const auto failed = RuntimeTestAccess::open_native(std::move(failed_api));
    CHECK(!failed && failed.error() == Error::BUSY);
    CHECK(std::count(failed_lifecycle.begin(), failed_lifecycle.end(), "claim") == 2);
    CHECK(std::count(failed_lifecycle.begin(), failed_lifecycle.end(), "release") == 1);
    CHECK(std::count(failed_lifecycle.begin(), failed_lifecycle.end(), "api-destroy") == 1);

    auto access_api = std::unique_ptr<FakeApi>(new FakeApi);
    FakeApi* access_raw = access_api.get();
    FakeDevice access_first = fake_device("00000000000071", 1U);
    FakeDevice access_second = fake_device("00000000000071", 2U);
    access_raw->devices = {&access_first, &access_second};
    access_raw->open_result = LIBUSB_ERROR_ACCESS;
    const auto access_denied = RuntimeTestAccess::open_native(
        std::move(access_api), "00000000000071");
    // Both Q3U4 candidates are visible, but opening their root-owned USB
    // nodes is denied.  This must not be reported as device absence.
    CHECK(!access_denied && access_denied.error() == Error::USB_IO);

    const auto expect_invalid_base_before_usb_discovery = [](const char* requested) {
        auto api = std::unique_ptr<FakeApi>(new FakeApi);
        FakeDevice first = fake_device("00000000000075", 1U);
        FakeDevice second = fake_device("00000000000075", 2U);
        api->devices = {&first, &second};
        api->open_result = LIBUSB_ERROR_ACCESS;
        const auto result = RuntimeTestAccess::open_native(std::move(api), requested);
        return !result && result.error() == Error::INVALID_ARGUMENT;
    };
    CHECK(expect_invalid_base_before_usb_discovery("1234567890123"));
    CHECK(expect_invalid_base_before_usb_discovery("1234567890123x"));

    auto no_serial_api = std::unique_ptr<FakeApi>(new FakeApi);
    FakeDevice no_serial_first = fake_device("00000000000072", 1U);
    FakeDevice no_serial_second = fake_device("00000000000072", 2U);
    no_serial_first.observation.serial.clear();
    no_serial_second.observation.serial.clear();
    no_serial_api->devices = {&no_serial_first, &no_serial_second};
    no_serial_api->open_result = LIBUSB_ERROR_ACCESS;
    const auto access_without_serial = RuntimeTestAccess::open_native(
        std::move(no_serial_api), "00000000000072");
    // Native descriptor enumeration has the serial index but cannot read the
    // serial string after access is denied.  Preserve the access error even
    // when grouping has no serial-bearing members to select.
    CHECK(!access_without_serial && access_without_serial.error() == Error::USB_IO);

    auto serial_both_api = std::unique_ptr<FakeApi>(new FakeApi);
    FakeApi* serial_both_raw = serial_both_api.get();
    FakeDevice serial_both_first = fake_device("00000000000073", 1U);
    FakeDevice serial_both_second = fake_device("00000000000073", 2U);
    serial_both_first.observation.serial.clear();
    serial_both_second.observation.serial.clear();
    serial_both_first.serial_result = LIBUSB_ERROR_ACCESS;
    serial_both_second.serial_result = LIBUSB_ERROR_ACCESS;
    serial_both_raw->devices = {&serial_both_first, &serial_both_second};
    const auto serial_both_denied = RuntimeTestAccess::open_native(
        std::move(serial_both_api), "00000000000073");
    // Both candidates fail while reading the descriptor.  The group has no
    // selectable serial-bearing members, but the actionable USB error must be
    // retained rather than becoming NOT_FOUND.
    CHECK(!serial_both_denied && serial_both_denied.error() == Error::USB_IO);

    auto serial_one_api = std::unique_ptr<FakeApi>(new FakeApi);
    FakeApi* serial_one_raw = serial_one_api.get();
    FakeDevice serial_one_first = fake_device("00000000000074", 1U);
    FakeDevice serial_one_second = fake_device("00000000000074", 2U);
    serial_one_first.observation.serial.clear();
    serial_one_first.serial_result = LIBUSB_ERROR_IO;
    serial_one_raw->devices = {&serial_one_first, &serial_one_second};
    const auto serial_one_denied = RuntimeTestAccess::open_native(
        std::move(serial_one_api), "00000000000074");
    // A single failed descriptor read leaves an incomplete group.  That
    // selection INVALID_ARGUMENT must not mask the candidate's USB_IO.
    CHECK(!serial_one_denied && serial_one_denied.error() == Error::USB_IO);
    return true;
}

#if defined(__linux__) || defined(__ANDROID__)
bool test_runtime_fd_ownership_and_quarantine()
{
    TestPipe first_pipe;
    TestPipe second_pipe;
    FakeDevice first = fake_device("00000000000080", 1U);
    FakeDevice second = fake_device("00000000000080", 2U);
    std::vector<std::string> lifecycle;
    auto api = std::unique_ptr<FakeApi>(new FakeApi);
    FakeApi* api_raw = api.get();
    api_raw->lifecycle_events = &lifecycle;
    std::vector<bool> fd_valid_at_close;
    api_raw->fd_valid_at_close_sink = &fd_valid_at_close;
    std::vector<intptr_t> wrapped_fds;
    api_raw->wrapped_fd_sink = &wrapped_fds;
    api_raw->wrapped_devices = {&second, &first};
    std::vector<int> retained_duplicates;
    auto syscalls = std::unique_ptr<TrackingFdSyscalls>(new TrackingFdSyscalls);
    syscalls->duplicate_sink = &retained_duplicates;
    auto runtime = RuntimeTestAccess::open_fds(
        std::move(api), std::move(syscalls), {first_pipe.fds[0], second_pipe.fds[0]});
    CHECK(runtime);
    CHECK(runtime.value()->base_serial() == "00000000000080");
    CHECK(api_raw->get_device_list_calls == 0U);
    runtime.value().reset();
    CHECK(std::find(lifecycle.begin(), lifecycle.end(), "exit") != lifecycle.end());
    CHECK(std::find(lifecycle.begin(), lifecycle.end(), "api-destroy") != lifecycle.end());
    CHECK(std::count(fd_valid_at_close.begin(), fd_valid_at_close.end(), true) == 2U);
    CHECK(::fcntl(first_pipe.fds[0], F_GETFD) >= 0);
    CHECK(::fcntl(second_pipe.fds[0], F_GETFD) >= 0);
    CHECK(retained_duplicates.size() == 2U);
    for (const int duplicate : retained_duplicates) {
        CHECK(::fcntl(duplicate, F_GETFD) == -1);
    }

    std::vector<std::string> quarantine_lifecycle;
    auto quarantine_api = std::unique_ptr<FakeApi>(new FakeApi);
    FakeApi* quarantine_raw = quarantine_api.get();
    quarantine_raw->lifecycle_events = &quarantine_lifecycle;
    quarantine_raw->wrapped_devices = {&first, &second};
    quarantine_raw->event_result = LIBUSB_ERROR_IO;
    quarantine_raw->cancel_returns_not_found = true;
    quarantine_raw->callback_after_not_found = false;
    std::size_t quarantine_close_calls = 0U;
    quarantine_raw->close_count_sink = &quarantine_close_calls;
    std::vector<int> quarantine_duplicates;
    auto quarantine_syscalls = std::unique_ptr<TrackingFdSyscalls>(new TrackingFdSyscalls);
    quarantine_syscalls->duplicate_sink = &quarantine_duplicates;
    bool quarantine_syscalls_destroyed = false;
    quarantine_syscalls->destroyed_sink = &quarantine_syscalls_destroyed;
    auto quarantined = RuntimeTestAccess::open_fds(
        std::move(quarantine_api), std::move(quarantine_syscalls),
        {first_pipe.fds[0], second_pipe.fds[0]});
    CHECK(quarantined);
    CHECK(quarantined.value()->dev1().start_stream(StreamConfig{kTsInEndpoint, 8U, 2U}));
    CHECK(quarantined.value()->dev1().cancel_stream().error() == Error::USB_IO);
    CHECK(quarantined.value()->quarantined());
    const std::size_t bulk_calls_before_quarantine_io = quarantine_raw->bulk_calls;
    const std::size_t submit_calls_before_quarantine_io = quarantine_raw->submit_calls;
    std::array<std::uint8_t, 1U> quarantine_buffer{};
    const ByteView quarantine_input{quarantine_buffer.data(), quarantine_buffer.size()};
    CHECK(quarantined.value()->dev1().bulk_read(
              kCommandInEndpoint,
              MutableByteView{quarantine_buffer.data(), quarantine_buffer.size()}, Timeout{1U})
              .error() == Error::USB_IO);
    CHECK(quarantined.value()->dev1().bulk_write(
              kCommandOutEndpoint, quarantine_input, Timeout{1U})
              .error() == Error::USB_IO);
    CHECK(quarantined.value()->dev2().bulk_read(
              kCommandInEndpoint,
              MutableByteView{quarantine_buffer.data(), quarantine_buffer.size()}, Timeout{1U})
              .error() == Error::USB_IO);
    CHECK(quarantined.value()->dev2().bulk_write(
              kCommandOutEndpoint, quarantine_input, Timeout{1U})
              .error() == Error::USB_IO);
    CHECK(quarantined.value()->dev1().start_stream(StreamConfig{kTsInEndpoint, 8U, 1U}).error() ==
          Error::USB_IO);
    CHECK(quarantined.value()->dev2().start_stream(StreamConfig{kTsInEndpoint, 8U, 1U}).error() ==
          Error::USB_IO);
    CHECK(quarantine_raw->bulk_calls == bulk_calls_before_quarantine_io);
    CHECK(quarantine_raw->submit_calls == submit_calls_before_quarantine_io);
    quarantined.value().reset();
    CHECK(std::find(quarantine_lifecycle.begin(), quarantine_lifecycle.end(), "exit") ==
          quarantine_lifecycle.end());
    CHECK(std::find(quarantine_lifecycle.begin(), quarantine_lifecycle.end(), "api-destroy") ==
          quarantine_lifecycle.end());
    CHECK(quarantine_syscalls_destroyed);
    CHECK(quarantine_close_calls == 1U);
    CHECK(quarantine_duplicates.size() == 2U);
    CHECK(::fcntl(quarantine_duplicates[0U], F_GETFD) >= 0);
    CHECK(::fcntl(quarantine_duplicates[1U], F_GETFD) == -1);
    return true;
}
#endif

bool test_runtime_context_serialization()
{
    RuntimeApiGate fair_gate;
    fair_gate.lock();
    std::array<int, 2U> acquisition_order{};
    std::atomic<std::size_t> acquisition_count{0U};
    std::thread first_waiter([&] {
        fair_gate.lock();
        const std::size_t index = acquisition_count.fetch_add(1U);
        acquisition_order[index] = 1;
        fair_gate.unlock();
    });
    fair_gate.wait_until_queued(1U);
    std::thread second_waiter([&] {
        fair_gate.lock();
        const std::size_t index = acquisition_count.fetch_add(1U);
        acquisition_order[index] = 2;
        fair_gate.unlock();
    });
    fair_gate.wait_until_queued(2U);
    fair_gate.unlock();
    first_waiter.join();
    second_waiter.join();
    CHECK(acquisition_count.load() == 2U);
    CHECK(acquisition_order[0U] == 1);
    CHECK(acquisition_order[1U] == 2);

    auto api = std::unique_ptr<FakeApi>(new FakeApi);
    FakeApi* api_raw = api.get();
    FakeDevice first = fake_device("00000000000090", 1U);
    FakeDevice second = fake_device("00000000000090", 2U);
    api_raw->devices = {&first, &second};
    auto runtime = RuntimeTestAccess::open_native(std::move(api));
    CHECK(runtime);
    api_raw->block_bulk.store(true);
    std::array<std::uint8_t, 1U> first_buffer{};
    std::array<std::uint8_t, 1U> second_buffer{};
    std::thread first_thread([&] {
        (void)runtime.value()->dev1().bulk_read(
            kCommandInEndpoint, MutableByteView{first_buffer.data(), first_buffer.size()}, Timeout{1U});
    });
    for (std::size_t attempt = 0U; attempt < 100000U && !api_raw->bulk_entered.load(); ++attempt) {
        std::this_thread::yield();
    }
    const bool entered = api_raw->bulk_entered.load();
    std::thread second_thread([&] {
        (void)runtime.value()->dev2().bulk_read(
            kCommandInEndpoint, MutableByteView{second_buffer.data(), second_buffer.size()}, Timeout{1U});
    });
    for (std::size_t attempt = 0U; attempt < 1000U; ++attempt) {
        std::this_thread::yield();
    }
    const bool serialized = api_raw->api_max_in_flight.load() == 1;
    api_raw->release_bulk.store(true);
    first_thread.join();
    second_thread.join();
    CHECK(entered);
    CHECK(serialized);
    CHECK(api_raw->api_max_in_flight.load() == 1);

    auto lifecycle_api = std::unique_ptr<FakeApi>(new FakeApi);
    FakeApi* lifecycle_raw = lifecycle_api.get();
    FakeDevice lifecycle_first = fake_device("00000000000091", 1U);
    FakeDevice lifecycle_second = fake_device("00000000000091", 2U);
    lifecycle_raw->devices = {&lifecycle_first, &lifecycle_second};
    auto lifecycle_runtime = RuntimeTestAccess::open_native(std::move(lifecycle_api));
    CHECK(lifecycle_runtime);
    lifecycle_raw->block_lifecycle_api.store(true);
    std::thread start_first([&] {
        (void)lifecycle_runtime.value()->dev1().start_stream(StreamConfig{kTsInEndpoint, 8U, 1U});
    });
    for (std::size_t attempt = 0U;
         attempt < 100000U && !lifecycle_raw->lifecycle_api_entered.load(); ++attempt) {
        std::this_thread::yield();
    }
    CHECK(lifecycle_raw->lifecycle_api_entered.load());
    std::thread start_second([&] {
        (void)lifecycle_runtime.value()->dev2().start_stream(StreamConfig{kTsInEndpoint, 8U, 1U});
    });
    for (std::size_t attempt = 0U; attempt < 1000U; ++attempt) {
        std::this_thread::yield();
    }
    CHECK(lifecycle_raw->api_max_in_flight.load() == 1);
    lifecycle_raw->release_lifecycle_api.store(true);
    start_first.join();
    start_second.join();
    CHECK(lifecycle_runtime.value()->dev1().stream_active());
    CHECK(lifecycle_runtime.value()->dev2().stream_active());

    lifecycle_raw->block_lifecycle_api.store(false);
    std::thread cancel_first([&] {
        (void)lifecycle_runtime.value()->dev1().cancel_stream();
    });
    std::thread cancel_second([&] {
        (void)lifecycle_runtime.value()->dev2().cancel_stream();
    });
    cancel_first.join();
    cancel_second.join();
    CHECK(!lifecycle_runtime.value()->dev1().stream_active());
    CHECK(!lifecycle_runtime.value()->dev2().stream_active());
    CHECK(lifecycle_raw->api_max_in_flight.load() == 1);
    return true;
}

bool test_command_event_dispatch_does_not_starve_stream_replenishment()
{
    auto api = std::unique_ptr<FakeApi>(new FakeApi);
    FakeApi* api_raw = api.get();
    FakeDevice first = fake_device("00000000000092", 1U);
    FakeDevice second = fake_device("00000000000092", 2U);
    api_raw->devices = {&first, &second};
    auto runtime = RuntimeTestAccess::open_native(std::move(api));
    CHECK(runtime);

    constexpr std::size_t kTransfers = 6U;
    api_raw->completion_length = 4;
    CHECK(runtime.value()->dev1().start_stream(
        StreamConfig{kTsInEndpoint, 8U, kTransfers}));
    CHECK(api_raw->submit_calls == kTransfers);
    api_raw->dispatch_stream_callbacks_in_bulk = true;

    std::array<std::uint8_t, 1U> command{};
    Result<std::size_t> command_result = Result<std::size_t>::failure(Error::INTERNAL);
    std::thread command_thread([&] {
        command_result = runtime.value()->dev1().bulk_read(
            kCommandInEndpoint,
            MutableByteView{command.data(), command.size()}, Timeout{100U});
    });
    api_raw->wait_for_bulk_stream_callbacks();

    bool events_ok = true;
    for (std::size_t index = 0U; index < kTransfers; ++index) {
        const auto event = runtime.value()->dev1().wait_stream(Timeout{0U});
        if (!event || event.value().size != 4U ||
            event.value().data[0U] != static_cast<std::uint8_t>(index + 1U)) {
            events_ok = false;
            break;
        }
    }
    // The next call returns the final delivered buffer before entering the
    // event API.  Thus every stream URB has been resubmitted while the
    // synchronous card-style bulk transaction remains blocked.
    const auto after_replenishment = runtime.value()->dev1().wait_stream(Timeout{0U});
    const std::size_t submit_calls_while_bulk_blocked = api_raw->submit_calls;
    const std::size_t event_calls_while_bulk_blocked = api_raw->event_calls;
    const int maximum_concurrency = api_raw->api_max_in_flight.load();

    api_raw->release_bulk_after_stream_callbacks();
    command_thread.join();
    CHECK(events_ok);
    CHECK(after_replenishment.error() == Error::TIMEOUT);
    CHECK(submit_calls_while_bulk_blocked == kTransfers * 2U);
    CHECK(event_calls_while_bulk_blocked == 0U);
    CHECK(maximum_concurrency >= 2);
    CHECK(command_result && command_result.value() == command.size());
    CHECK(runtime.value()->dev1().cancel_stream());
    return true;
}

#endif

}  // namespace

int main(int argc, char** argv)
{
#if PX4_ENABLE_LIBUSB
    if (argc == 2 && std::strcmp(argv[1], "--fail-safe-abandonment") == 0) {
        std::printf("RUN fail_safe_abandonment (explicit; ASAN detect_leaks=0)\n");
        if (!test_fail_safe_abandonment()) {
            std::fprintf(stderr, "FAIL fail_safe_abandonment\n");
            return 1;
        }
#if defined(__linux__) || defined(__ANDROID__)
        if (!test_runtime_fd_ownership_and_quarantine()) {
            std::fprintf(stderr, "FAIL runtime_fd_ownership_and_quarantine\n");
            return 1;
        }
#endif
        std::printf("PASS fail_safe_abandonment\n");
        return 0;
    }
#endif
    (void)argc;
    (void)argv;
    const struct Test final {
        const char* name;
        bool (*function)();
    } tests[] = {
        {"error_contract", test_error_contract},
        {"sha256_and_firmware_policy", test_sha256_and_firmware_policy},
        {"it930x_scatter_parser", test_it930x_scatter_parser},
        {"it930x_golden_and_typed_operations", test_it930x_golden_and_typed_operations},
        {"it930x_limits_and_failures", test_it930x_limits_and_failures},
        {"it930x_command_pacing", test_it930x_command_pacing},
        {"it930x_sequence_wrap", test_it930x_sequence_wrap},
        {"it930x_firmware_load_paths", test_it930x_firmware_load_paths},
        {"it930x_q3u4_warm_initialization", test_it930x_q3u4_warm_initialization},
        {"it930x_mlt5pe_warm_initialization", test_it930x_mlt5pe_warm_initialization},
        {"it930x_q3u4_power_state_after_initialization",
         test_it930x_q3u4_power_state_after_initialization},
        {"it930x_q3u4_lnb_gpio_authority",
         test_it930x_q3u4_lnb_gpio_authority},
        {"single_receiver_lnb_15v_rejected",
         test_single_receiver_lnb_15v_rejected},
        {"single_receiver_terrestrial_lock_settle",
         test_single_receiver_terrestrial_lock_settle},
        {"single_receiver_warm_initialization_skips_gpio11",
         test_single_receiver_warm_initialization_skips_gpio11},
        {"single_receiver_cold_initialization_skips_gpio11",
         test_single_receiver_cold_initialization_skips_gpio11},
        {"it930x_q3u4_warm_failure_and_gate_cleanup",
         test_it930x_q3u4_warm_failure_and_gate_cleanup},
        {"it930x_probe_argument_parser", test_it930x_probe_argument_parser},
        {"logging", test_logging},
        {"mock_transport", test_mock_transport},
        {"mock_failures_and_bounds", test_mock_failures_and_bounds},
        {"error_and_serial_contract", test_error_and_serial_contract},
        {"identity_grouping_and_topology", test_identity_grouping_and_topology},
        {"collision_grouping_and_usb_path_selection",
         test_collision_grouping_and_usb_path_selection},
        {"lnb_15v_profile_capability_table", test_lnb_15v_profile_capability_table},
        {"frontend_control_layer", run_frontend_tests},
        {"card_atr", run_card_tests},
        {"card_service", run_card_service_tests},
        {"it930x_card_uart", run_it930x_card_tests},
        {"ipc_wire_codec", run_ipc_tests},
        {"ipc_connection_state", run_ipc_state_tests},
        {"px4d_arguments", run_px4d_args_tests},
        {"px4d_signals", run_px4d_signal_tests},
#if PX4_ENABLE_POSIX_IPC
        {"control_integration", run_control_integration_tests},
        {"posix_ipc_transport", run_posix_ipc_tests},
        {"posix_tuner_nonce", run_posix_tuner_nonce_tests},
        {"control_workers", run_control_workers_tests},
        {"px4_ts", run_px4_ts_tests},
        {"px4ctl_format", run_px4ctl_format_tests},
        {"px4d_list_format", run_px4d_list_format_tests},
#endif
        {"q3u4_frontend_lifecycle", run_q3u4_frontend_tests},
        {"q3u4_card_backend", run_q3u4_card_backend_tests},
        {"q3u4_backend_power", run_q3u4_power_tests},
        {"q3u4_lnb_power", run_q3u4_lnb_power_tests},
        {"tuner_service", run_tuner_service_tests},
        {"r850_q3u4", run_r850_tests},
        {"rt710_q3u4", run_rt710_tests},
        {"frontend_probe", run_frontend_probe_tests},
        {"ts_probe", run_ts_probe_tests},
        {"tagged_ts_demux", run_tagged_ts_demux_tests},
        {"q3u4_stream", run_q3u4_stream_tests},
        {"mlt5pe", run_mlt5pe_tests},
#if PX4_ENABLE_LIBUSB
        {"bulk_transport_and_mapping", test_bulk_transport_and_mapping},
        {"not_found_cancel_waits_for_callback", test_not_found_cancel_waits_for_callback},
        {"native_enumeration_filters_and_stream_lifecycle",
         test_native_enumeration_filters_and_stream_lifecycle},
        {"stream_completion_submission_order", test_stream_completion_submission_order},
        {"fd_ownership_and_init_mode", test_fd_ownership_and_init_mode},
        {"fd_enclosure_batch", test_fd_enclosure_batch},
        {"native_enumeration_reports_unopened_devices",
         test_native_enumeration_reports_unopened_devices},
        {"runtime_native_collision_claims_only_selected_paths",
         test_runtime_native_collision_claims_only_selected_paths},
        {"runtime_native_transaction_and_ownership", test_runtime_native_transaction_and_ownership},
        {"runtime_context_serialization", test_runtime_context_serialization},
        {"command_event_dispatch_does_not_starve_stream_replenishment",
         test_command_event_dispatch_does_not_starve_stream_replenishment},
#endif
    };
    for (const Test& test : tests) {
        if (!test.function()) {
            std::fprintf(stderr, "FAIL %s\n", test.name);
            return 1;
        }
        std::printf("PASS %s\n", test.name);
    }
    return 0;
}
