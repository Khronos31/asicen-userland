#include "asicen/channel_plan.h"

namespace asicen {

bool w3u3_is_valid_bs_transponder(std::uint8_t transponder) {
    return transponder >= 1 && transponder <= 23 && (transponder & 1U) != 0;
}

bool w3u3_is_valid_cs_transponder(std::uint8_t transponder) {
    return transponder >= 2 && transponder <= 24 && (transponder & 1U) == 0;
}

std::uint32_t w3u3_satellite_rf_khz(SatelliteBand band, std::uint8_t transponder) {
    if (band == SatelliteBand::Bs) {
        if (!w3u3_is_valid_bs_transponder(transponder)) {
            return 0;
        }
        const std::uint32_t index = (transponder - 1U) / 2U;
        return 11727480U + index * 38360U;
    }

    if (!w3u3_is_valid_cs_transponder(transponder)) {
        return 0;
    }
    const std::uint32_t index = (transponder - 2U) / 2U;
    return 12291000U + index * 40000U;
}

}  // namespace asicen
