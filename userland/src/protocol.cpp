#include "asicen/protocol.h"

#include <algorithm>
#include <limits>

namespace asicen {

std::uint8_t bm_request_type(Direction direction)
{
    return direction == Direction::In ? kVendorIn : kVendorOut;
}

std::uint8_t bulk_endpoint_for_lane(std::uint8_t lane)
{
    if (lane == 0) {
        return kBulkEndpointLane0;
    }
    if (lane == 1) {
        return kBulkEndpointLane1;
    }
    return 0;
}

std::uint16_t setup_word(std::uint8_t low, std::uint8_t high)
{
    return static_cast<std::uint16_t>(low) | (static_cast<std::uint16_t>(high) << 8U);
}

ControlTransfer make_i2c_read(std::uint8_t slave, std::uint8_t reg, std::uint16_t data_length,
                              std::uint8_t mode, std::uint16_t timeout_ms)
{
    const std::uint16_t max_data_length =
        static_cast<std::uint16_t>(std::numeric_limits<std::uint16_t>::max() - 1U);
    if (data_length > max_data_length) {
        ControlTransfer invalid{};
        invalid.valid = false;
        return invalid;
    }

    return ControlTransfer{
        0,
        Request::I2cRead,
        setup_word(slave, reg),
        setup_word(mode, 0),
        static_cast<std::uint16_t>(data_length + 1U),
        Direction::In,
        timeout_ms,
    };
}

ControlTransfer make_i2c_read_no_wait(std::uint8_t slave, std::uint16_t data_length,
                                      std::uint16_t timeout_ms)
{
    const std::uint16_t max_data_length =
        static_cast<std::uint16_t>(std::numeric_limits<std::uint16_t>::max() - 1U);
    if (data_length > max_data_length) {
        ControlTransfer invalid{};
        invalid.valid = false;
        return invalid;
    }

    return ControlTransfer{
        0,
        Request::I2cReadNoWait,
        setup_word(slave, 0),
        0,
        static_cast<std::uint16_t>(data_length + 1U),
        Direction::In,
        timeout_ms,
    };
}

bool parse_status_response(const std::uint8_t* response, std::size_t response_size,
                           std::uint8_t* output, std::size_t output_size)
{
    if (response == nullptr || response_size == 0 || response[0] != 1) {
        return false;
    }
    if (output_size > response_size - 1U) {
        return false;
    }
    if (output_size != 0 && output == nullptr) {
        return false;
    }
    if (output_size != 0) {
        std::copy_n(response + 1, output_size, output);
    }
    return true;
}

bool parse_customer_info(const std::uint8_t* data, std::size_t size, CustomerInfo* out)
{
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

} // namespace asicen
