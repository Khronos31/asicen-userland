#pragma once

#include <cstdint>

namespace asicen {

enum class Direction : std::uint16_t {
    Out = 0,
    In = 1,
};

struct ControlTransfer {
    std::uint8_t tuner_num;
    std::uint8_t request;
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
constexpr unsigned long kIoctlStreamAux = 0x103;
constexpr unsigned long kIoctlStreamRead = 0x104;

std::uint8_t bm_request_type(Direction direction);

}  // namespace asicen
