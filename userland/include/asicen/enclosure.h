#pragma once

#include <cstddef>
#include <cstdint>

#include "asicen/frontend_facts.h"

namespace asicen {

struct ReceiverAddress {
    std::uint8_t function_index = 0;
    std::uint8_t local_lane = 0;
    BroadcastSystem system = BroadcastSystem::IsdbS;
};

// Proven mapping for original PX-W3U3/PX-W3U2 family:
// each enclosure has two runtime USB functions and each function has
// local lane 0 = ISDB-S, lane 1 = ISDB-T.
bool w3u3_receiver_address(std::size_t receiver, ReceiverAddress* out);

}  // namespace asicen
