// SPDX-License-Identifier: GPL-2.0-only
#include "libusb_stream_policy.h"

#include "px4/transport.h"

#include <libusb.h>

#include <algorithm>
#include <limits>

namespace asicen {

int prepare_windows_stream(WindowsRawIoApi& api, std::size_t* transfer_size,
                           bool* raw_io_enabled) noexcept
{
    if (transfer_size == nullptr || raw_io_enabled == nullptr)
        return LIBUSB_ERROR_INVALID_PARAM;
    *raw_io_enabled = false;
    if (*transfer_size == 0U || *transfer_size > px4::userland::kMaxStreamTransfer)
        return LIBUSB_ERROR_INVALID_PARAM;
    const int supported = api.supports_raw_io();
    if (supported <= 0) return supported;

    const int packet_size = api.maximum_packet_size();
    if (packet_size < 0) return packet_size;
    if (packet_size == 0) return LIBUSB_ERROR_IO;
    const int maximum_size = api.maximum_transfer_size();
    if (maximum_size < 0) return maximum_size;
    if (maximum_size == 0) return LIBUSB_ERROR_IO;

    // RAW_IO bypasses WinUSB's software queue, but requires packet-aligned
    // reads no larger than MAXIMUM_TRANSFER_SIZE. This is USB packet size,
    // not TS size: the demux already retains partial 188-byte TS packets.
    const std::size_t packet = static_cast<std::size_t>(packet_size);
    const std::size_t remainder = *transfer_size % packet;
    const std::size_t padding = remainder == 0U ? 0U : packet - remainder;
    const std::size_t limit = std::min({px4::userland::kMaxStreamTransfer,
        static_cast<std::size_t>(std::numeric_limits<int>::max()),
        static_cast<std::size_t>(maximum_size)});
    if (*transfer_size > limit || padding > limit - *transfer_size)
        return LIBUSB_ERROR_OVERFLOW;
    const std::size_t aligned = *transfer_size + padding;

    const int result = api.enable_raw_io();
    if (result != 0) return result;
    *transfer_size = aligned;
    *raw_io_enabled = true;
    return 0;
}

#if defined(_WIN32)
#if LIBUSB_API_VERSION >= 0x0100010C
namespace {

class NativeWindowsRawIoApi final : public WindowsRawIoApi {
public:
    NativeWindowsRawIoApi(libusb_device_handle* handle, std::uint8_t endpoint) noexcept
        : handle_(handle), endpoint_(endpoint) {}
    int supports_raw_io() noexcept override
    { return libusb_endpoint_supports_raw_io(handle_, endpoint_); }
    int maximum_packet_size() noexcept override
    { return libusb_get_max_packet_size(libusb_get_device(handle_), endpoint_); }
    int maximum_transfer_size() noexcept override
    { return libusb_get_max_raw_io_transfer_size(handle_, endpoint_); }
    int enable_raw_io() noexcept override
    { return libusb_endpoint_set_raw_io(handle_, endpoint_, 1); }

private:
    libusb_device_handle* handle_;
    std::uint8_t endpoint_;
};

}  // namespace
#endif

int prepare_libusb_stream(libusb_device_handle* handle, std::uint8_t endpoint,
                                    std::size_t* transfer_size,
                                    bool* raw_io_enabled) noexcept
{
#if LIBUSB_API_VERSION >= 0x0100010C
    NativeWindowsRawIoApi api(static_cast<libusb_device_handle*>(handle), endpoint);
    return prepare_windows_stream(api, transfer_size, raw_io_enabled);
#else
    // Older locally supplied libusb builds retain their existing behavior.
    // Official Windows artifacts pin libusb 1.0.30 and use the path above.
    (void)handle;
    (void)endpoint;
    if (transfer_size == nullptr || raw_io_enabled == nullptr)
        return LIBUSB_ERROR_INVALID_PARAM;
    *raw_io_enabled = false;
    return 0;
#endif
}

int finish_libusb_stream(libusb_device_handle* handle, std::uint8_t endpoint) noexcept
{
#if LIBUSB_API_VERSION >= 0x0100010C
    return libusb_endpoint_set_raw_io(static_cast<libusb_device_handle*>(handle), endpoint, 0);
#else
    (void)handle;
    (void)endpoint;
    return 0;
#endif
}
#endif

}  // namespace asicen
