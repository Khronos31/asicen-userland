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

    // Frequencies passed to the recovered frontend API are in kHz.
    static constexpr std::uint32_t kSystemDispatchBoundaryKHz = 999999;

    // One FC0012 terrestrial tuning branch changes behavior at 260999 kHz.
    static constexpr std::uint32_t kFc0012BandBoundaryKHz = 260999;
};

BroadcastSystem w3u3_system_for_local_lane(std::uint8_t lane);
std::uint8_t w3u3_demod_i2c_for_local_lane(std::uint8_t lane);

}  // namespace asicen
