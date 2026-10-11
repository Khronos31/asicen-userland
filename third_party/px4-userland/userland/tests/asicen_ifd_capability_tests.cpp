// SPDX-License-Identifier: GPL-2.0-only
// Product capability and spelling checks supplement the unmodified IFD suite.
#include "px4/pcsc_ifd_adapter.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <memory>

namespace {

using namespace px4::userland;
using namespace px4::userland::pcsc;

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            std::fprintf(stderr, "check failed at %s:%d: %s\n",             \
                         __FILE__, __LINE__, #condition);                    \
            return false;                                                   \
        }                                                                   \
    } while (false)

class OfflineClient final : public IfdCardClient {
public:
    Result<IfdCardStatus> status() noexcept override
    {
        IfdCardStatus value{};
        value.present = true;
        value.initialized = true;
        value.atr = {0x3bU, 0x10U, 0x13U};
        value.atr_length = 3U;
        return Result<IfdCardStatus>::success(value);
    }
    Result<IfdCardConnectResult> connect_shared() noexcept override
    {
        IfdCardConnectResult value{};
        value.handle = 1U;
        value.atr = {0x3bU, 0x10U, 0x13U};
        value.atr_length = 3U;
        return Result<IfdCardConnectResult>::success(value);
    }
    Result<void> disconnect(std::uint64_t) noexcept override
    { return Result<void>::success(); }
    Result<IfdCardConnectResult> reset(std::uint64_t) noexcept override
    { return connect_shared(); }
    Result<std::size_t> transmit(std::uint64_t, ByteView apdu,
                                 MutableByteView response) noexcept override
    {
        if (apdu.size == 0U || response.size < 2U)
            return Result<std::size_t>::failure(Error::BUFFER_TOO_SMALL);
        response.data[0] = 0x90U;
        response.data[1] = 0x00U;
        return Result<std::size_t>::success(2U);
    }
    void close() noexcept override {}
};

class OfflineFactory final : public IfdCardClientFactory {
public:
    Result<std::unique_ptr<IfdCardClient>> connect(const IfdEndpoint&) noexcept override
    {
        return Result<std::unique_ptr<IfdCardClient>>::success(
            std::unique_ptr<IfdCardClient>(new OfflineClient()));
    }
};

bool test_product_ifd_capabilities()
{
    constexpr const char* name = "asicen-userland:instance=usb1-2.1:access=user";
    const auto parsed = parse_ifd_device_name(name);
    CHECK(parsed && parsed.value().device_instance == "usb1-2.1");
    CHECK(!parse_ifd_device_name("px4-userland:instance=usb1-2.1"));
    CHECK(!parse_ifd_device_name("asicen-userland:instance=../usb"));
    CHECK(!parse_ifd_device_name("asicen-userland:instance=usb:instance=other"));
    CHECK(!parse_ifd_device_name("asicen-userland:instance=usb:access=other"));
    OfflineFactory factory;
    IfdAdapter adapter(factory);
    CHECK(adapter.create_channel_by_name(1U, name) == IfdResult::no_such_device);
    CHECK(adapter.create_channel_by_name(0U, name) == IfdResult::success);
    std::array<std::uint8_t, 128U> output{};
    std::size_t length = output.size();
    CHECK(adapter.get_capability(0U, kAttrMaxDataRate,
        MutableByteView{output.data(), output.size()}, length) == IfdResult::success);
    CHECK(length == 4U && output[0] == 0x00U && output[1] == 0x4bU &&
          output[2] == 0U && output[3] == 0U);
    length = 2U;
    CHECK(adapter.get_capability(0U, kAttrMaxDataRate,
        MutableByteView{output.data(), output.size()}, length) == IfdResult::insufficient_buffer);
    CHECK(length == 4U);
    length = output.size();
    CHECK(adapter.get_capability(0U, kAttrVendorIfdType,
        MutableByteView{output.data(), output.size()}, length) == IfdResult::success);
    constexpr char type[] = "ASICEN card reader via asicend";
    CHECK(length == sizeof(type) - 1U &&
          std::memcmp(output.data(), type, sizeof(type) - 1U) == 0);
    length = output.size();
    CHECK(adapter.power(0U, IfdPowerAction::power_up,
        MutableByteView{output.data(), output.size()}, length) == IfdResult::success);
    CHECK(length == 3U && output[0] == 0x3bU && output[1] == 0x10U && output[2] == 0x13U);
    CHECK(adapter.set_protocol(0U, kIfdSetProtocolT1, kIfdNegotiatePts1,
                               0x11U, 0U, 0U) == IfdResult::error_pts_failure);
    CHECK(adapter.set_protocol(0U, kIfdSetProtocolT1, kIfdNegotiatePts1,
                               0x13U, 0U, 0U) == IfdResult::success);
    const std::array<std::uint8_t, 5U> apdu{0x90U, 0x30U, 0U, 0U, 0U};
    length = output.size();
    CHECK(adapter.transmit(0U, kIfdTransmitProtocolT1, ByteView{apdu.data(), apdu.size()},
        MutableByteView{output.data(), output.size()}, length) == IfdResult::success);
    CHECK(length == 2U && output[0] == 0x90U && output[1] == 0x00U);
    CHECK(map_ifd_error(Error::TIMEOUT, IfdOperation::transmit) == IfdResult::response_timeout);
    CHECK(adapter.close_channel(0U) == IfdResult::success);
    return true;
}

}  // namespace

int main()
{
    if (!test_product_ifd_capabilities()) {
        std::fprintf(stderr, "FAIL product_ifd_capabilities\n");
        return 1;
    }
    std::printf("PASS product_ifd_capabilities\n");
    return 0;
}
