#ifndef ASICEN_USERLAND_ENCLOSURE_H
#define ASICEN_USERLAND_ENCLOSURE_H

#include <cstddef>
#include <cstdint>

#include "asicen/frontend_facts.h"
#include "asicen/device_profile.h"
#include "px4/error.h"

namespace asicen {

struct ReceiverAddress {
    std::uint8_t function_index = 0;
    std::uint8_t local_lane = 0;
    BroadcastSystem system = BroadcastSystem::IsdbS;
    std::uint8_t rf_function_index = 0;
    std::uint8_t frontend_source = 0;
};

// Proven mapping for original PX-W3U3/PX-W3U2 family:
// each enclosure has two runtime USB functions and each function has
// local lane 0 = ISDB-S, lane 1 = ISDB-T.
px4::userland::Result<ReceiverAddress> receiver_address(const DeviceProfile& profile,
                                                        std::size_t receiver) noexcept;

}  // namespace asicen

#endif  // ASICEN_USERLAND_ENCLOSURE_H
