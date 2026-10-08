#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace asicen {

enum class Direction : std::uint16_t {
    Out = 0,
    In = 1,
};

enum class Request : std::uint8_t {
    ReadIr = 0x00,
    SetIrMode = 0x01,
    I2cRead = 0x02,
    I2cWrite = 0x03,
    ChannelFilterRead = 0x04,
    ChannelFilterWrite = 0x05,
    DscStart = 0x06,
    DscStop = 0x07,
    Gpio = 0x08,
    ResetChannel = 0x09,
    GetHighSpeed = 0x0a,
    CustomerInfo = 0x0c,
    I2cBufferFill = 0x0d,
    I2cBufferSend = 0x0e,
    GpioExSet = 0x10,
    GpioExGet = 0x11,
    ResetEncryptionChip = 0x12,
    WriteEncryptionRegister = 0x13,
    I2cWriteNoStop = 0x14,
    SysCtrlRead = 0x17,
    SysCtrlWrite = 0x18,
    I2cReadNoWait = 0x19,
    GetDeviceRandomKey = 0x1a,
};

struct ControlTransfer {
    std::uint8_t tuner_num;
    Request request;
    std::uint16_t value;
    std::uint16_t index;
    std::uint16_t length;
    Direction direction;
    std::uint16_t timeout_ms;
};

constexpr std::uint8_t kVendorOut = 0x40;
constexpr std::uint8_t kVendorIn = 0xc0;

// Reference Linux driver ioctl ABI recovered from the PLEX 1.0 userspace library.
constexpr unsigned long kIoctlControl = 0x100;
constexpr unsigned long kIoctlBulkControl = 0x101;
constexpr unsigned long kIoctlStreamLength = 0x102;
constexpr unsigned long kIoctlStreamClean = 0x103;
constexpr unsigned long kIoctlStreamRead = 0x104;

// Recovered stream geometry. One ASICEN runtime USB function exposes two lanes.
constexpr std::uint8_t kStreamLaneCount = 2;
constexpr std::uint8_t kBulkEndpointLane0 = 0x81;
constexpr std::uint8_t kBulkEndpointLane1 = 0x82;
constexpr std::size_t kStreamUnitBytes = 512;
constexpr std::size_t kTsPacketBytes = 188;
constexpr std::size_t kMaxUserspaceStreamRead = kTsPacketBytes * 1024;
constexpr std::uint64_t kDefaultStreamRingUnits = 0x24b8;  // 9400 * 512 bytes
constexpr std::uint64_t kDefaultBulkTransferCount = 4;

constexpr std::size_t kCustomerInfoSize = 58;

struct CustomerInfo {
    std::uint8_t use_customer_info = 0;
    std::array<std::uint8_t, 2> info_id{};
    std::array<std::uint8_t, 2> vid{};
    std::array<std::uint8_t, 2> pid{};
    std::array<std::uint8_t, 10> manufacturer{};
    std::array<std::uint8_t, 16> product{};
    std::array<std::uint8_t, 15> hid{};
    std::uint8_t remote_control_number = 0;
    std::array<std::uint8_t, 8> customer_defined{};
    std::uint8_t support_feature = 0;
};

std::uint8_t bm_request_type(Direction direction);
std::uint8_t bulk_endpoint_for_lane(std::uint8_t lane);
std::uint16_t setup_word(std::uint8_t low, std::uint8_t high);

ControlTransfer make_i2c_read(std::uint8_t slave,
                              std::uint8_t reg,
                              std::uint16_t data_length,
                              std::uint8_t mode,
                              std::uint16_t timeout_ms = 1000);

ControlTransfer make_i2c_read_no_wait(std::uint8_t slave,
                                      std::uint16_t data_length,
                                      std::uint16_t timeout_ms = 1000);

bool parse_status_response(const std::uint8_t* response,
                           std::size_t response_size,
                           std::uint8_t* output,
                           std::size_t output_size);

bool parse_customer_info(const std::uint8_t* data, std::size_t size, CustomerInfo* out);

}  // namespace asicen
