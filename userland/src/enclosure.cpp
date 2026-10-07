#include "asicen/enclosure.h"

namespace asicen {

bool w3u3_receiver_address(std::size_t receiver, ReceiverAddress* out) {
    if (out == nullptr || receiver >= 4) {
        return false;
    }

    // Public receiver numbering is grouped by runtime function so the mapping
    // remains stable even before physical connector labels are confirmed.
    out->function_index = static_cast<std::uint8_t>(receiver / 2U);
    out->local_lane = static_cast<std::uint8_t>(receiver % 2U);
    out->system = w3u3_system_for_local_lane(out->local_lane);
    return true;
}

}  // namespace asicen
