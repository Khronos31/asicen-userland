#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace asicen {

constexpr std::uint32_t kIpcMagic = 0x43495341U;  // "ASIC" in little endian
constexpr std::uint16_t kIpcVersion = 1;
constexpr std::size_t kIpcMessageSize = 24;

enum class IpcCommand : std::uint16_t {
    Status = 1,
    Stream = 2,
};

enum class IpcStatus : std::uint16_t {
    Ok = 0,
    Invalid = 1,
    Busy = 2,
    Internal = 3,
};

struct IpcRequest {
    IpcCommand command = IpcCommand::Status;
    std::uint32_t receiver = 0;
    std::uint32_t packet_count = 0;
};

struct IpcResponse {
    IpcStatus status = IpcStatus::Internal;
    std::uint64_t lease_id = 0;
    std::uint32_t receiver_count = 0;
};

std::array<std::uint8_t, kIpcMessageSize> encode_request(const IpcRequest& request);
bool decode_request(const std::uint8_t* data, std::size_t size, IpcRequest* request);

std::array<std::uint8_t, kIpcMessageSize> encode_response(const IpcResponse& response);
bool decode_response(const std::uint8_t* data, std::size_t size, IpcResponse* response);

}  // namespace asicen
