#ifndef ASICEN_USERLAND_CHANNEL_PLAN_H
#define ASICEN_USERLAND_CHANNEL_PLAN_H

#include <cstdint>

namespace asicen {

enum class SatelliteBand : std::uint8_t {
    Bs,
    Cs,
};

// Returns the RF frequency expected by the recovered W3U3 ASICEN API.
// BS uses odd transponder numbers 1..23.
// CS uses even transponder numbers 2..24.
// Returns 0 for an invalid transponder number.
std::uint32_t w3u3_satellite_rf_khz(SatelliteBand band, std::uint8_t transponder);

bool w3u3_is_valid_bs_transponder(std::uint8_t transponder);
bool w3u3_is_valid_cs_transponder(std::uint8_t transponder);

}  // namespace asicen

#endif  // ASICEN_USERLAND_CHANNEL_PLAN_H
