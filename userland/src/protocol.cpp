#include "asicen/protocol.h"

#include <algorithm>

namespace asicen {

std::uint8_t bm_request_type(Direction direction) {
    return direction == Direction::In ? kVendorIn : kVendorOut;
}

bool parse_customer_info(const std::uint8_t* data, std::size_t size, CustomerInfo* out) {
    if (data == nullptr || out == nullptr || size < kCustomerInfoSize) {
        return false;
    }

    std::size_t offset = 0;
    out->use_customer_info = data[offset++];

    const auto copy = [&](auto& dst) {
        std::copy_n(data + offset, dst.size(), dst.begin());
        offset += dst.size();
    };

    copy(out->info_id);
    copy(out->vid);
    copy(out->pid);
    copy(out->manufacturer);
    copy(out->product);
    copy(out->hid);
    out->remote_control_number = data[offset++];
    copy(out->customer_defined);
    out->support_feature = data[offset++];

    return offset == kCustomerInfoSize;
}

}  // namespace asicen
