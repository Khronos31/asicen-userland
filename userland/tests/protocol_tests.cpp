#include "asicen/device_profile.h"
#include "asicen/frontend_facts.h"
#include "asicen/protocol.h"

#include <array>
#include <cstdint>
#include <iostream>

namespace {

int failures = 0;

void check(bool value, const char* name) {
    if (!value) {
        std::cerr << "FAIL: " << name << '\n';
        ++failures;
    }
}

}  // namespace

int main() {
    check(asicen::bm_request_type(asicen::Direction::Out) == 0x40, "vendor out");
    check(asicen::bm_request_type(asicen::Direction::In) == 0xc0, "vendor in");
    check(asicen::setup_word(0x34, 0x12) == 0x1234, "setup word");

    check(asicen::bulk_endpoint_for_lane(0) == 0x81, "stream 0 endpoint");
    check(asicen::bulk_endpoint_for_lane(1) == 0x82, "stream 1 endpoint");
    check(asicen::bulk_endpoint_for_lane(2) == 0, "invalid endpoint");
    check(asicen::kMaxUserspaceStreamRead == 188U * 1024U, "stream read cap");

    check(asicen::w3u3_system_for_local_lane(0) == asicen::BroadcastSystem::IsdbS,
          "W3U3 local lane 0 is satellite");
    check(asicen::w3u3_system_for_local_lane(1) == asicen::BroadcastSystem::IsdbT,
          "W3U3 local lane 1 is terrestrial");
    check(asicen::w3u3_demod_i2c_for_local_lane(0) == 0x32,
          "W3U3 satellite demod address");
    check(asicen::w3u3_demod_i2c_for_local_lane(1) == 0x30,
          "W3U3 terrestrial demod address");

    const asicen::DeviceProfile* w3u3 = asicen::find_profile(0x0b06, 0x0005);
    check(w3u3 != nullptr, "W3U3 profile");
    if (w3u3 != nullptr) {
        check(w3u3->enclosure_receiver_count == 4, "W3U3 enclosure receiver count");
        check(w3u3->expected_runtime_functions == 2, "W3U3 runtime function count");
        check(w3u3->local_lane_count == 2, "W3U3 local lane count");
    }

    const asicen::ControlTransfer read =
        asicen::make_i2c_read(0x60, 0x12, 4, 3, 250);
    check(read.request == asicen::Request::I2cRead, "i2c read request");
    check(read.value == 0x1260, "i2c read value");
    check(read.index == 0x0003, "i2c read index");
    check(read.length == 5, "i2c read response length");
    check(read.direction == asicen::Direction::In, "i2c read direction");
    check(read.timeout_ms == 250, "i2c read timeout");

    const asicen::ControlTransfer no_wait =
        asicen::make_i2c_read_no_wait(0x68, 8);
    check(no_wait.request == asicen::Request::I2cReadNoWait, "i2c no-wait request");
    check(no_wait.value == 0x0068, "i2c no-wait value");
    check(no_wait.index == 0, "i2c no-wait index");
    check(no_wait.length == 9, "i2c no-wait response length");

    const std::array<std::uint8_t, 4> response{1, 0xaa, 0xbb, 0xcc};
    std::array<std::uint8_t, 3> output{};
    check(asicen::parse_status_response(response.data(), response.size(),
                                        output.data(), output.size()),
          "status response parse");
    check(output[0] == 0xaa && output[1] == 0xbb && output[2] == 0xcc,
          "status response payload");

    const std::array<std::uint8_t, 2> bad_response{0, 0xff};
    check(!asicen::parse_status_response(bad_response.data(), bad_response.size(),
                                         output.data(), 1),
          "status response failure");

    std::array<std::uint8_t, asicen::kCustomerInfoSize> customer{};
    customer[0] = 1;
    customer[3] = 0x06;
    customer[4] = 0x0b;
    customer[5] = 0x05;
    customer[6] = 0x00;
    customer[57] = 0xa5;
    asicen::CustomerInfo parsed{};
    check(asicen::parse_customer_info(customer.data(), customer.size(), &parsed),
          "customer info parse");
    check(parsed.use_customer_info == 1, "customer info flag");
    check(parsed.vid[0] == 0x06 && parsed.vid[1] == 0x0b, "customer info vid");
    check(parsed.pid[0] == 0x05 && parsed.pid[1] == 0x00, "customer info pid");
    check(parsed.support_feature == 0xa5, "customer info feature");

    return failures == 0 ? 0 : 1;
}
