#include "asicen/ipc.h"

namespace asicen {
namespace {

void put16(std::uint8_t* p, std::uint16_t value) {
    p[0] = static_cast<std::uint8_t>(value);
    p[1] = static_cast<std::uint8_t>(value >> 8U);
}

void put32(std::uint8_t* p, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) {
        p[i] = static_cast<std::uint8_t>(value >> (i * 8U));
    }
}

void put64(std::uint8_t* p, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) {
        p[i] = static_cast<std::uint8_t>(value >> (i * 8U));
    }
}

std::uint16_t get16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0]) |
           (static_cast<std::uint16_t>(p[1]) << 8U);
}

std::uint32_t get32(const std::uint8_t* p) {
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(p[i]) << (i * 8U);
    }
    return value;
}

std::uint64_t get64(const std::uint8_t* p) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(p[i]) << (i * 8U);
    }
    return value;
}

bool valid_header(const std::uint8_t* data, std::size_t size) {
    return data != nullptr && size == kIpcMessageSize &&
           get32(data) == kIpcMagic && get16(data + 4) == kIpcVersion;
}

}  // namespace

std::array<std::uint8_t, kIpcMessageSize> encode_request(const IpcRequest& request) {
    std::array<std::uint8_t, kIpcMessageSize> data{};
    put32(data.data(), kIpcMagic);
    put16(data.data() + 4, kIpcVersion);
    put16(data.data() + 6, static_cast<std::uint16_t>(request.command));
    put32(data.data() + 8, request.receiver);
    put32(data.data() + 12, request.packet_count);
    return data;
}

bool decode_request(const std::uint8_t* data, std::size_t size, IpcRequest* request) {
    if (request == nullptr || !valid_header(data, size)) {
        return false;
    }
    const auto command = static_cast<IpcCommand>(get16(data + 6));
    if (command != IpcCommand::Status && command != IpcCommand::Stream) {
        return false;
    }
    request->command = command;
    request->receiver = get32(data + 8);
    request->packet_count = get32(data + 12);
    return true;
}

std::array<std::uint8_t, kIpcMessageSize> encode_response(const IpcResponse& response) {
    std::array<std::uint8_t, kIpcMessageSize> data{};
    put32(data.data(), kIpcMagic);
    put16(data.data() + 4, kIpcVersion);
    put16(data.data() + 6, static_cast<std::uint16_t>(response.status));
    put64(data.data() + 8, response.lease_id);
    put32(data.data() + 16, response.receiver_count);
    return data;
}

bool decode_response(const std::uint8_t* data, std::size_t size, IpcResponse* response) {
    if (response == nullptr || !valid_header(data, size)) {
        return false;
    }
    const auto status = static_cast<IpcStatus>(get16(data + 6));
    if (status != IpcStatus::Ok && status != IpcStatus::Invalid &&
        status != IpcStatus::Busy && status != IpcStatus::Internal) {
        return false;
    }
    response->status = status;
    response->lease_id = get64(data + 8);
    response->receiver_count = get32(data + 16);
    return true;
}

}  // namespace asicen
