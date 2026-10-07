#include "asicen/protocol.h"

namespace asicen {

std::uint8_t bm_request_type(Direction direction) {
    return direction == Direction::In ? kVendorIn : kVendorOut;
}

}  // namespace asicen
