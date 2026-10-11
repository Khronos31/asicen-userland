// SPDX-License-Identifier: GPL-2.0-only
// Portable, hardware-free tests of the Windows RAW_IO policy; ASICEN TS framing has its own unchanged suites.
#include "windows/libusb_stream_policy.h"
#include "px4/transport.h"

#include <libusb.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

namespace {

using namespace asicen;
using px4::userland::kMaxStreamTransfer;

int failures = 0;

#define CHECK(condition)                                                         \
    do {                                                                         \
        if (!(condition)) {                                                      \
            std::fprintf(stderr, "RAW_IO check failed at %s:%d: %s\n",          \
                         __FILE__, __LINE__, #condition);                        \
            ++failures;                                                          \
        }                                                                        \
    } while (false)

class FakeRawIoApi final : public WindowsRawIoApi {
public:
    int support = 1;
    int packet = 512;
    int maximum = static_cast<int>(kMaxStreamTransfer);
    int enable_result = 0;
    std::vector<int> calls;

    int supports_raw_io() noexcept override
    { calls.push_back(1); return support; }
    int maximum_packet_size() noexcept override
    { calls.push_back(2); return packet; }
    int maximum_transfer_size() noexcept override
    { calls.push_back(3); return maximum; }
    int enable_raw_io() noexcept override
    { calls.push_back(4); return enable_result; }
};

void check_policy(FakeRawIoApi api, std::size_t requested, int expected_result,
                  std::size_t expected_size, bool expected_enabled,
                  const std::vector<int>& expected_calls)
{
    std::size_t size = requested;
    // Even an unsupported or failing call must clear a stale output flag.
    bool enabled = true;
    CHECK(prepare_windows_stream(api, &size, &enabled) == expected_result);
    CHECK(size == expected_size);
    CHECK(enabled == expected_enabled);
    CHECK(api.calls == expected_calls);
    if (expected_result != 0) {
        CHECK(size == requested);
        CHECK(!enabled);
    }
}

void test_alignment_and_fallback()
{
    FakeRawIoApi api;
    check_policy(api, 188U * 816U, 0, 153600U, true, {1, 2, 3, 4});
    check_policy(api, 153600U, 0, 153600U, true, {1, 2, 3, 4});
    check_policy(api, 1U, 0, 512U, true, {1, 2, 3, 4});
    api.support = 0;
    check_policy(api, 188U * 816U, 0, 188U * 816U, false, {1});
}

void test_api_errors()
{
    constexpr std::size_t requested = 188U * 816U;
    FakeRawIoApi api;
    api.support = LIBUSB_ERROR_NO_DEVICE;
    check_policy(api, requested, LIBUSB_ERROR_NO_DEVICE, requested, false, {1});
    api = FakeRawIoApi{};
    api.packet = LIBUSB_ERROR_NOT_FOUND;
    check_policy(api, requested, LIBUSB_ERROR_NOT_FOUND, requested, false, {1, 2});
    api.packet = 0;
    check_policy(api, requested, LIBUSB_ERROR_IO, requested, false, {1, 2});
    api = FakeRawIoApi{};
    api.maximum = LIBUSB_ERROR_NOT_SUPPORTED;
    check_policy(api, requested, LIBUSB_ERROR_NOT_SUPPORTED, requested, false,
                 {1, 2, 3});
    api.maximum = 0;
    check_policy(api, requested, LIBUSB_ERROR_IO, requested, false, {1, 2, 3});
    api = FakeRawIoApi{};
    api.enable_result = LIBUSB_ERROR_NO_MEM;
    check_policy(api, requested, LIBUSB_ERROR_NO_MEM, requested, false, {1, 2, 3, 4});
}

void test_size_limits()
{
    FakeRawIoApi api;
    check_policy(api, 0U, LIBUSB_ERROR_INVALID_PARAM, 0U, false, {});
    check_policy(api, kMaxStreamTransfer, 0, kMaxStreamTransfer, true, {1, 2, 3, 4});
    check_policy(api, kMaxStreamTransfer - 1U, 0, kMaxStreamTransfer, true,
                 {1, 2, 3, 4});
    check_policy(api, kMaxStreamTransfer + 1U, LIBUSB_ERROR_INVALID_PARAM,
                 kMaxStreamTransfer + 1U, false, {});
    const auto largest = std::numeric_limits<std::size_t>::max();
    check_policy(api, largest, LIBUSB_ERROR_INVALID_PARAM, largest, false, {});

    api.maximum = 153600;
    check_policy(api, 153408U, 0, 153600U, true, {1, 2, 3, 4});
    api.maximum = 153599;
    check_policy(api, 153408U, LIBUSB_ERROR_OVERFLOW, 153408U, false, {1, 2, 3});
    api.maximum = 153407;
    check_policy(api, 153408U, LIBUSB_ERROR_OVERFLOW, 153408U, false, {1, 2, 3});
    api.maximum = std::numeric_limits<int>::max();
    api.packet = 3;
    check_policy(api, kMaxStreamTransfer, LIBUSB_ERROR_OVERFLOW,
                 kMaxStreamTransfer, false, {1, 2, 3});
    api.packet = std::numeric_limits<int>::max();
    check_policy(api, 1U, LIBUSB_ERROR_OVERFLOW, 1U, false, {1, 2, 3});
}

}  // namespace

int main()
{
    test_alignment_and_fallback();
    test_api_errors();
    test_size_limits();

    if (failures != 0) return 1;
    std::puts("Windows libusb stream policy tests passed");
    return 0;
}
