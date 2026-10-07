#include "asicen/frontend_facts.h"

namespace asicen {

BroadcastSystem w3u3_system_for_local_lane(std::uint8_t lane) {
    return lane == W3u3FrontendFacts::kTerrestrialLane
        ? BroadcastSystem::IsdbT
        : BroadcastSystem::IsdbS;
}

std::uint8_t w3u3_demod_i2c_for_local_lane(std::uint8_t lane) {
    if (lane == W3u3FrontendFacts::kSatelliteLane) {
        return W3u3FrontendFacts::kSatelliteDemodI2c;
    }
    if (lane == W3u3FrontendFacts::kTerrestrialLane) {
        return W3u3FrontendFacts::kTerrestrialDemodI2c;
    }
    return 0;
}

}  // namespace asicen
