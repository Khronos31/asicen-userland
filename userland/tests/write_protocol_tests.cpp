#include "asicen/write_protocol.h"

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

bool test_write_protocol()
{
    CHECK(!asicen::make_i2c_buffer_send(0xa8, 0, true).valid);
    CHECK(!asicen::make_i2c_buffer_send(0xa8, 33, true).valid);
    CHECK(!asicen::make_i2c_buffer_send(0xa8, 0xffffU, true).valid);
    CHECK(asicen::make_i2c_buffer_send(0xa8, 32, true).valid &&
          asicen::make_i2c_buffer_send(0xa8, 32, true).length == 33);
    CHECK(!asicen::make_cf_read(2, 0x40, 5).valid);
    CHECK(!asicen::make_cf_read(1, 0x40, 0xffffU).valid);
    CHECK(!asicen::make_dsc_control(2, false).valid);
    const std::array<std::uint8_t, 1> invalid_lane_data{{0x55}};
    asicen::ControlTransfer invalid_lane_transfer{};
    CHECK(!asicen::make_cf_write(2, 0x40, invalid_lane_data.data(), invalid_lane_data.size(),
                                &invalid_lane_transfer));
    const std::array<std::uint8_t, 5> data{0x11, 0x22, 0x33, 0x44, 0x55};

    asicen::ControlTransfer t{};
    CHECK(asicen::make_i2c_write_chunk(0x60, 0x20, data.data(), 2, false, &t));
    CHECK(t.request == asicen::Request::I2cWrite);
    CHECK(t.value == 0x2060);
    CHECK(t.index == 0x2211);
    CHECK(t.length == 3 && t.direction == asicen::Direction::In);

    CHECK(asicen::make_i2c_write_chunk(0x60, 0x21, data.data(), 1, true, &t));
    CHECK(t.request == asicen::Request::I2cWriteNoStop);
    CHECK(t.index == 0x0011 && t.length == 2);
    CHECK(!asicen::make_i2c_write_chunk(0x60, 0, data.data(), 3, false, &t));

    CHECK(asicen::make_i2c_buffer_fill(3, data.data(), 3, &t));
    CHECK(t.request == asicen::Request::I2cBufferFill);
    CHECK(t.value == 0x1103 && t.index == 0x3322 && t.length == 4);

    t = asicen::make_i2c_buffer_send(0xa8, 7, true);
    CHECK(t.request == asicen::Request::I2cBufferSend);
    CHECK(t.value == 0x01a8 && t.length == 8);

    CHECK(asicen::make_sysctrl_write(0x42, data.data(), 3, &t));
    CHECK(t.request == asicen::Request::SysCtrlWrite);
    CHECK(t.value == 0x1142 && t.index == 0x3322 && t.length == 4);

    t = asicen::make_gpio_set(0x12, 0xf0);
    CHECK(t.request == asicen::Request::Gpio && t.value == 0xf012);
    t = asicen::make_gpio_ex_set(0x34, 0x0f);
    CHECK(t.request == asicen::Request::GpioExSet && t.value == 0x0f34);
    t = asicen::make_gpio_ex_get();
    CHECK(t.request == asicen::Request::GpioExGet && t.length == 1);
    CHECK(asicen::make_dsc_control(1, false).request == asicen::Request::DscStart);
    CHECK(asicen::make_dsc_control(1, true).request == asicen::Request::DscStop);
    for (std::uint8_t local : std::array<std::uint8_t, 2>{0, 1}) {
        const auto start = asicen::make_dsc_control(local, false);
        CHECK(static_cast<std::uint8_t>(start.request) == 0x06 &&
              asicen::bm_request_type(start.direction) == 0xc0 && start.value == local &&
              start.index == 0 && start.length == 1);
        const auto stop = asicen::make_dsc_control(local, true);
        CHECK(static_cast<std::uint8_t>(stop.request) == 0x07 &&
              asicen::bm_request_type(stop.direction) == 0xc0 && stop.value == local &&
              stop.index == 0 && stop.length == 1);
    }
    CHECK(asicen::make_reset_channel(2, 7).value == 0x0702);

    const auto normal = asicen::build_i2c_write_sequence(0x60, 0x20, data.data(), 5, 0);
    CHECK(normal.size() == 3);
    CHECK(normal[0].value == 0x2060 && normal[0].index == 0x2211);
    CHECK(normal[1].value == 0x2260 && normal[1].index == 0x4433);
    CHECK(normal[2].value == 0x2460 && normal[2].index == 0x0055);

    const auto odd = asicen::build_i2c_write_sequence(0x60, 0x21, data.data(), 3, 4);
    CHECK(odd.size() == 2);
    CHECK(odd[0].request == asicen::Request::I2cWriteNoStop && odd[0].index == 0x0011);
    CHECK(odd[1].value == 0x2260 && odd[1].index == 0x3322);

    std::array<std::uint8_t, 35> large{};
    for (std::size_t i = 0; i < large.size(); ++i) {
        large[i] = static_cast<std::uint8_t>(i);
    }
    const auto ext = asicen::build_i2c_write_sequence(0xa8, 0, large.data(), large.size(), 3);
    // 32-byte batch: 11 fill transfers + send. 3-byte batch: 1 fill + send.
    CHECK(ext.size() == 14);
    CHECK(ext[0].request == asicen::Request::I2cBufferFill && ext[0].value == 0x0000);
    CHECK(ext[10].request == asicen::Request::I2cBufferFill);
    CHECK(ext[11].request == asicen::Request::I2cBufferSend && ext[11].length == 33);
    CHECK(ext[12].request == asicen::Request::I2cBufferFill && ext[12].value == 0x2000);
    CHECK(ext[13].request == asicen::Request::I2cBufferSend && ext[13].length == 4);

    const auto ext_nostop = asicen::build_i2c_write_sequence(0xa8, 0, data.data(), 5, 2);
    CHECK(ext_nostop.size() == 3);
    CHECK((ext_nostop.back().value >> 8) == 0);

    return true;
}

} // namespace

int main()
{
    if (!test_write_protocol()) {
        return 1;
    }
    return 0;
}
