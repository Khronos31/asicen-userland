#pragma once

#include <libusb.h>

#include <cstdint>
#include <vector>

#include "asicen/protocol.h"
#include "asicen/hardware_ownership.h"

namespace asicen {

struct UsbLocation {
    std::uint8_t bus = 0;
    std::uint8_t address = 0;
};

// Result of observing libusb_kernel_driver_active(). Android's libusb returns
// LIBUSB_ERROR_NOT_SUPPORTED, which means the state is known and no kernel
// driver is bound. Other negative results leave the state unknown.
struct KernelDriverState {
    bool known = false;
    bool active = false;
};

KernelDriverState classify_kernel_driver_state(int query_result) noexcept;

// Duplicates fd with FD_CLOEXEC for libusb_wrap_sys_device retention. The
// caller's fd is never closed. Returns the duplicate or a negative value.
int duplicate_fd_cloexec(int fd) noexcept;

class LibusbDevice final {
public:
    LibusbDevice() = default;
    ~LibusbDevice();

    LibusbDevice(const LibusbDevice&) = delete;
    LibusbDevice& operator=(const LibusbDevice&) = delete;

    int open(libusb_context* context, UsbLocation location);

    // Opens an already-granted USB fd (Termux/Android). fd is validated and
    // duplicated; the duplicate backs the wrapped handle and is closed by
    // close(). The caller's fd is never closed on any path.
    int open(libusb_context* context, int fd);
    void close();

    bool is_open() const;
    libusb_device_handle* handle() const;
    libusb_device* device() const;

    int control(const ControlTransfer& transfer, unsigned char* data);

    // Vendor host-to-device control transfer with an explicit setup. Returns the
    // transferred byte count or a negative libusb error code.
    int vendor_out(std::uint8_t request,
                   std::uint16_t value,
                   std::uint16_t index,
                   const unsigned char* data,
                   std::uint16_t length,
                   unsigned int timeout_ms);

    // Returns 1 when a kernel driver is bound to interface_number, 0 when the
    // interface is free, or a negative libusb error code.
    int kernel_driver_active(int interface_number) const;

    // Claims interface_number as-is. Released by close(). Repeated claims are
    // ignored; kernel drivers are never detached.
    int claim_interface(int interface_number);
    int release_interface(int interface_number) noexcept;

    // Finds the active alternate setting that owns endpoint_address, claims
    // its interface, and selects the alternate setting when needed.
    // Kernel drivers are never detached implicitly.
    int claim_endpoint(std::uint8_t endpoint_address);

    int bulk_read(std::uint8_t endpoint_address,
                  unsigned char* data,
                  int length,
                  int* transferred,
                  unsigned int timeout_ms);

private:
    bool interface_claimed(int interface_number) const;

    libusb_device_handle* handle_ = nullptr;
    int retained_fd_ = -1;
    std::vector<int> claimed_interfaces_;
};

class LibusbFunctionClaim final : public UsbFunctionClaim {
public:
    explicit LibusbFunctionClaim(LibusbDevice& device) : device_(device) {}
    UsbFunctionSnapshot snapshot() const override;
    int claim_interface0() override;
    int release_interface0() noexcept override;

private:
    LibusbDevice& device_;
};

}  // namespace asicen
