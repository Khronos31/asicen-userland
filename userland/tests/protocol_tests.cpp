#include "asicen/channel_plan.h"
#include "asicen/device_profile.h"
#include "asicen/frontend_facts.h"
#include "asicen/loader_firmware.h"
#include "asicen/protocol.h"

#include <array>
#include <cstdint>
#include <cstdio>

namespace {

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            std::fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__,         \
                         #condition);                                                       \
            return false;                                                                   \
        }                                                                                   \
    } while (false)

bool test_protocol()
{
    CHECK(asicen::bm_request_type(asicen::Direction::Out) == 0x40);
    CHECK(asicen::bm_request_type(asicen::Direction::In) == 0xc0);
    CHECK(asicen::setup_word(0x34, 0x12) == 0x1234);

    CHECK(asicen::bulk_endpoint_for_lane(0) == 0x81);
    CHECK(asicen::bulk_endpoint_for_lane(1) == 0x82);
    CHECK(asicen::bulk_endpoint_for_lane(2) == 0);
    CHECK(asicen::kMaxUserspaceStreamRead == 188U * 1024U);

    const auto& firmware_stages = asicen::loader_firmware_stages();
    CHECK(firmware_stages.size() == 4);
    CHECK(asicen::kLoaderFirmwareStartAddress == 0x5399);
    CHECK(asicen::kLoaderFirmwareChunkSize == 0x200);
    CHECK(firmware_stages[0].blob_offset == 0x0000 && firmware_stages[0].length == 0x0c00 &&
          firmware_stages[0].final_request == 0xab);
    CHECK(firmware_stages[3].blob_offset == 0x3800 && firmware_stages[3].length == 0x0800 &&
          firmware_stages[3].final_request == 0xac);

    const auto loader_plan = asicen::build_loader_transfer_plan();
    CHECK(loader_plan.size() == 20);
    CHECK(loader_plan.front().request == 0xab && loader_plan.front().value == 0x0000 &&
          loader_plan.front().index == 0x5399 && loader_plan.front().length == 0x0200);
    CHECK(loader_plan[5].value == 0x0a00 && loader_plan[6].value == 0x2000);
    CHECK(loader_plan.back().request == 0xac && loader_plan.back().value == 0x3e00 &&
          loader_plan.back().length == 0x0200);

    CHECK(asicen::w3u3_satellite_rf_khz(asicen::SatelliteBand::Bs, 1) == 11727480U);
    CHECK(asicen::w3u3_satellite_rf_khz(asicen::SatelliteBand::Bs, 23) == 12149440U);
    CHECK(asicen::w3u3_satellite_rf_khz(asicen::SatelliteBand::Cs, 2) == 12291000U);
    CHECK(asicen::w3u3_satellite_rf_khz(asicen::SatelliteBand::Cs, 24) == 12731000U);
    CHECK(asicen::w3u3_satellite_rf_khz(asicen::SatelliteBand::Bs, 2) == 0);
    CHECK(asicen::w3u3_satellite_rf_khz(asicen::SatelliteBand::Cs, 3) == 0);

    CHECK(asicen::w3u3_system_for_local_lane(0) == asicen::BroadcastSystem::IsdbS);
    CHECK(asicen::w3u3_system_for_local_lane(1) == asicen::BroadcastSystem::IsdbT);
    CHECK(asicen::w3u3_demod_i2c_for_local_lane(0) == 0x32);
    CHECK(asicen::w3u3_demod_i2c_for_local_lane(1) == 0x30);
    CHECK(asicen::W3u3FrontendFacts::kSystemDispatchBoundaryKHz == 999999U);
    CHECK(asicen::W3u3FrontendFacts::kFc0012BandBoundaryKHz == 260999U);

    const asicen::DeviceProfile* w3u3 = asicen::find_profile(0x0b06, 0x0005);
    CHECK(w3u3 != nullptr);
    CHECK(w3u3->enclosure_receiver_count == 4);
    CHECK(w3u3->expected_runtime_functions == 2);
    CHECK(w3u3->local_lane_count == 2);

    const asicen::ControlTransfer read = asicen::make_i2c_read(0x60, 0x12, 4, 3, 250);
    CHECK(read.request == asicen::Request::I2cRead);
    CHECK(read.value == 0x1260);
    CHECK(read.index == 0x0003);
    CHECK(read.length == 5);
    CHECK(read.direction == asicen::Direction::In);
    CHECK(read.timeout_ms == 250);

    const asicen::ControlTransfer no_wait = asicen::make_i2c_read_no_wait(0x68, 8);
    CHECK(no_wait.request == asicen::Request::I2cReadNoWait);
    CHECK(no_wait.value == 0x0068);
    CHECK(no_wait.index == 0);
    CHECK(no_wait.length == 9);
    CHECK(!asicen::make_i2c_read(0x60, 0x12, 0xffffU, 3).valid);
    CHECK(!asicen::make_i2c_read_no_wait(0x68, 0xffffU).valid);
    CHECK(asicen::make_i2c_read(0x60, 0x12, 0xfffeU, 3).valid &&
          asicen::make_i2c_read(0x60, 0x12, 0xfffeU, 3).length == 0xffffU);

    const std::array<std::uint8_t, 4> response{1, 0xaa, 0xbb, 0xcc};
    std::array<std::uint8_t, 3> output{};
    CHECK(asicen::parse_status_response(response.data(), response.size(), output.data(),
                                       output.size()));
    CHECK(output[0] == 0xaa && output[1] == 0xbb && output[2] == 0xcc);

    const std::array<std::uint8_t, 2> bad_response{0, 0xff};
    CHECK(!asicen::parse_status_response(bad_response.data(), bad_response.size(), output.data(), 1));

    std::array<std::uint8_t, asicen::kCustomerInfoSize> customer{};
    customer[0] = 1;
    customer[3] = 0x06;
    customer[4] = 0x0b;
    customer[5] = 0x05;
    customer[6] = 0x00;
    customer[57] = 0xa5;
    asicen::CustomerInfo parsed{};
    CHECK(asicen::parse_customer_info(customer.data(), customer.size(), &parsed));
    CHECK(parsed.use_customer_info == 1);
    CHECK(parsed.vid[0] == 0x06 && parsed.vid[1] == 0x0b);
    CHECK(parsed.pid[0] == 0x05 && parsed.pid[1] == 0x00);
    CHECK(parsed.support_feature == 0xa5);

    return true;
}

} // namespace

int main()
{
    if (!test_protocol()) {
        return 1;
    }
    return 0;
}
