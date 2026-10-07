#pragma once

#include <cstdint>

namespace asicen {

enum class BroadcastSystem : std::uint8_t {
    IsdbS,
    IsdbT,
};

struct W3u3FrontendFacts {
    static constexpr std::uint8_t kLocalLaneCount = 2;
    static constexpr std::uint8_t kSatelliteLane = 0;
    static constexpr std::uint8_t kTerrestrialLane = 1;

    // 8-bit I2C address form used by the recovered historical library.
    static constexpr std::uint8_t kSatelliteDemodI2c = 0x32;
    static constexpr std::uint8_t kTerrestrialDemodI2c = 0x30;

    // TC_SetFrequency's recovered dispatch boundary.
    static constexpr std::uint64_t kSystemFrequencyBoundary = 999999;
};

BroadcastSystem w3u3_system_for_local_lane(std::uint8_t lane);
std::uint8_t w3u3_demod_i2c_for_local_lane(std::uint8_t lane);

}  // namespace asicen
