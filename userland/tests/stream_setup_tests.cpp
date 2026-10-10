#include "asicen/frontend_sequence.h"
#include "asicen/write_protocol.h"

#include <cstdint>
#include <iostream>
#include <vector>

namespace {

bool check(bool value, const char* name)
{
    if (!value) {
        std::cerr << "FAIL: " << name << '\n';
        return false;
    }
    return true;
}

#define CHECK(...)                                                                                 \
    do {                                                                                           \
        if (!check(__VA_ARGS__)) {                                                                 \
            return false;                                                                          \
        }                                                                                          \
    } while (false)

class RecordingTransport final : public asicen::FrontendTransport {
public:
    int control(const asicen::ControlTransfer& transfer, unsigned char* data) override
    {
        transfers.push_back(transfer);
        for (std::uint16_t i = 0; i < transfer.length; ++i) {
            data[i] = 0;
        }
        if (transfer.length > 0) {
            data[0] = 1;
        }
        return transfer.length;
    }
    void delay_ms(unsigned) override {}

    std::vector<asicen::ControlTransfer> transfers;
};

}  // namespace

bool run_tests()
{
    // Increment A: safe startup subset (DTV_Start 27/fb without sibling 40).
    const asicen::FrontendPlan subset = asicen::plan_startup_subset();
    CHECK(subset.size() == 1, "startup subset single op");
    CHECK(subset[0].kind == asicen::FrontendOpKind::Control &&
              subset[0].transfer.request == asicen::Request::Gpio &&
              subset[0].transfer.value == 0xbb27,
          "startup subset gpio value bb27");
    {
        const std::uint8_t value = static_cast<std::uint8_t>(subset[0].transfer.value & 0xffU);
        const std::uint8_t mask =
            static_cast<std::uint8_t>((subset[0].transfer.value >> 8U) & 0xffU);
        CHECK(value == 0x27 && mask == 0xbb, "startup subset value/mask");
        CHECK((mask & 0x40U) == 0, "startup subset never touches sibling 0x40");
        CHECK((value & 0x20U) != 0 && (mask & 0x20U) != 0, "startup subset sets LNB 0x20");
    }

    // Increment B: CF request packing.
    {
        const asicen::ControlTransfer read = asicen::make_cf_read(0, 0x40, 1);
        CHECK(read.request == asicen::Request::ChannelFilterRead && read.value == 0x0040 &&
                  read.index == 0 && read.length == 2,
              "cf read packing");
        const asicen::ControlTransfer read_lane1 = asicen::make_cf_read(1, 0x40, 1);
        CHECK(read_lane1.value == 0x00c0, "cf read lane1 or80");

        asicen::ControlTransfer write{};
        const std::uint8_t boundary[2] = {0x1f, 0xff};
        CHECK(asicen::make_cf_write(0, 0x41, boundary, 2, &write), "cf write build");
        CHECK(write.request == asicen::Request::ChannelFilterWrite && write.value == 0x1f41 &&
                  write.index == 0x00ff && write.length == 3,
              "cf write packing");
        CHECK(asicen::make_cf_write(1, 0x41, boundary, 2, &write), "cf write lane1 build");
        CHECK(write.value == 0x1fc1 && write.index == 0x00ff, "cf write lane1 or80");
        CHECK(!asicen::make_cf_write(0, 0x41, boundary, 4, &write), "cf write rejects >3");
    }

    // plan_stream_setup = filter reset + PID boundary 0x41/0x43.
    const asicen::FrontendPlan setup = asicen::plan_stream_setup(1);
    CHECK(setup.size() == 3, "stream setup op count");
    CHECK(setup[0].kind == asicen::FrontendOpKind::FilterReset && setup[0].local == 1 &&
              setup[0].flag == 1,
          "stream setup filter reset");
    CHECK(setup[1].transfer.request == asicen::Request::ChannelFilterWrite &&
              setup[1].transfer.value == 0x1fc1 && setup[1].transfer.index == 0x00ff,
          "stream setup pid 41");
    CHECK(setup[2].transfer.value == 0x1fc3, "stream setup pid 43");
    CHECK(asicen::plan_stream_setup(2).empty(), "stream setup rejects local2");
    const asicen::FrontendPlan primary_setup = asicen::plan_stream_setup(0U, 1U);
    CHECK(primary_setup.size() == 3U && primary_setup[0].local == 0U &&
              primary_setup[1].transfer.value == 0x1f41 &&
              primary_setup[2].transfer.value == 0x1f43,
          "primary satellite capture setup addresses local0 filter registers");

    // Filter reset execution: 3 CF reads at subcmd 0, ResetChannel, one CF write
    // (all-zero 3-byte chunks skipped, so only the chunk carrying byte 0x40).
    {
        RecordingTransport transport;
        asicen::FrontendPlan reset_only;
        reset_only.push_back(setup[0]);
        const asicen::FrontendRunResult result = asicen::run_frontend_plan(reset_only, &transport);
        CHECK(result == asicen::FrontendRunResult::Completed, "filter reset completes");
        CHECK(transport.transfers.size() == 5, "filter reset transfer count");
        if (transport.transfers.size() == 5) {
            CHECK(transport.transfers[0].request == asicen::Request::ChannelFilterRead &&
                      transport.transfers[0].value == 0x0080 &&
                      transport.transfers[0].length == 0x21,
                  "filter reset read chunk 1");
            CHECK(transport.transfers[1].length == 0x21, "filter reset read chunk 2");
            CHECK(transport.transfers[2].length == 0x06, "filter reset read chunk 3");
            CHECK(transport.transfers[3].request == asicen::Request::ResetChannel &&
                      transport.transfers[3].value == 0x0101 && transport.transfers[3].length == 1,
                  "filter reset reset-channel");
            CHECK(transport.transfers[4].request == asicen::Request::ChannelFilterWrite &&
                      transport.transfers[4].index == 0x0004 && transport.transfers[4].length == 4,
                  "filter reset write carries bit2");
        }
    }

    return true;
}

int main()
{
    return run_tests() ? 0 : 1;
}
