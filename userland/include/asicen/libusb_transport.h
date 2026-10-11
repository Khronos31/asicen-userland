// SPDX-License-Identifier: GPL-2.0-only
#ifndef ASICEN_USERLAND_LIBUSB_TRANSPORT_H
#define ASICEN_USERLAND_LIBUSB_TRANSPORT_H

#include <libusb.h>

#include <cstdint>
#include <vector>

#include "asicen/protocol.h"
#include "asicen/hardware_ownership.h"
#include "asicen/usb_error.h"

namespace asicen {

// Acquisition/lifetime surface of the pinned LibusbApi. The default adapter
// calls libusb; injected implementations exercise failures without USB access.
// An injected adapter must outlive every context and device that refers to it.
class LibusbAcquisitionApi {
public:
    virtual ~LibusbAcquisitionApi() noexcept = default;
    virtual int init(libusb_context** context, bool no_device_discovery) noexcept = 0;
    virtual void exit(libusb_context* context) noexcept = 0;
    virtual ssize_t get_device_list(libusb_context* context, libusb_device*** list) noexcept = 0;
    virtual void free_device_list(libusb_device** list, int unref_devices) noexcept = 0;
    virtual std::uint8_t get_bus_number(libusb_device* device) noexcept = 0;
    virtual std::uint8_t get_device_address(libusb_device* device) noexcept = 0;
    virtual int open(libusb_device* device, libusb_device_handle** handle) noexcept = 0;
    virtual int wrap_sys_device(libusb_context* context, intptr_t fd,
                                libusb_device_handle** handle) noexcept = 0;
    virtual void close(libusb_device_handle* handle) noexcept = 0;
    virtual libusb_device* get_device(libusb_device_handle* handle) noexcept = 0;
    virtual int get_device_descriptor(libusb_device* device,
                                      libusb_device_descriptor* descriptor) noexcept = 0;
    virtual int get_port_numbers(libusb_device* device, std::uint8_t* ports,
                                 int capacity) noexcept = 0;
    virtual int get_active_config_descriptor(libusb_device* device,
                                             libusb_config_descriptor** config) noexcept = 0;
    virtual void free_config_descriptor(libusb_config_descriptor* config) noexcept = 0;
    virtual int kernel_driver_active(libusb_device_handle* handle, int interface_number) noexcept = 0;
    virtual int claim_interface(libusb_device_handle* handle, int interface_number) noexcept = 0;
    virtual int release_interface(libusb_device_handle* handle, int interface_number) noexcept = 0;
    virtual int set_interface_alt_setting(libusb_device_handle* handle, int interface_number,
                                          int alternate_setting) noexcept = 0;
};

LibusbAcquisitionApi& native_libusb_acquisition_api() noexcept;

px4::userland::Error map_libusb_acquisition_error(int error) noexcept;
int initialize_libusb_context(libusb_context** context, bool fd_mode = false) noexcept;
void release_libusb_context(libusb_context* context) noexcept;
void quarantine_libusb_context(libusb_context* context) noexcept;

// Declare before its devices: handles/claims unwind before context exit. A
// quarantined context is retained rather than exited with pending callbacks.
class LibusbContext final {
public:
    explicit LibusbContext(LibusbAcquisitionApi& api = native_libusb_acquisition_api()) noexcept
        : api_(api) {}
    ~LibusbContext() noexcept;
    LibusbContext(const LibusbContext&) = delete;
    LibusbContext& operator=(const LibusbContext&) = delete;
    int initialize(bool fd_mode = false) noexcept;
    libusb_context* get() const noexcept { return context_; }
    void close() noexcept;
private:
    LibusbAcquisitionApi& api_;
    libusb_context* context_ = nullptr;
};

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

// POSIX descriptor duplication with FD_CLOEXEC. The caller's fd is never
// closed. Returns the duplicate or a negative value. USB wrapping remains a
// separate Linux/Android-only operation.
int duplicate_fd_cloexec(int fd) noexcept;

class LibusbDevice final {
  public:
    explicit LibusbDevice(LibusbAcquisitionApi& api = native_libusb_acquisition_api()) noexcept
        : api_(api) {}
    ~LibusbDevice();

    LibusbDevice(const LibusbDevice&) = delete;
    LibusbDevice& operator=(const LibusbDevice&) = delete;

    int open(libusb_context* context, UsbLocation location);

    // Opens an already-granted USB fd (Termux/Android). fd is validated and
    // duplicated; the duplicate backs the wrapped handle and is closed by
    // close(). The caller's fd is never closed on any path.
    int open(libusb_context* context, int fd);
    void close();
    // Retain handle, claims and any wrapped fd until process exit when libusb
    // cannot prove callback quiescence. No USB cleanup is safe in this state.
    void abandon() noexcept;

    bool is_open() const;
    libusb_device_handle* handle() const;
    libusb_device* device() const;
    int descriptor(libusb_device_descriptor* output) const noexcept;
    int port_numbers(std::uint8_t* ports, int capacity) const noexcept;

    int control(const ControlTransfer& transfer, unsigned char* data);

    // Vendor host-to-device control transfer with an explicit setup. Returns the
    // transferred byte count or a negative libusb error code.
    int vendor_out(std::uint8_t request, std::uint16_t value, std::uint16_t index,
                   const unsigned char* data, std::uint16_t length, unsigned int timeout_ms);

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

    int bulk_read(std::uint8_t endpoint_address, unsigned char* data, int length, int* transferred,
                  unsigned int timeout_ms);

  private:
    friend class LibusbFunctionClaim;
    bool interface_claimed(int interface_number) const;

    LibusbAcquisitionApi& api_;
    libusb_context* context_ = nullptr;
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

} // namespace asicen

#endif // ASICEN_USERLAND_LIBUSB_TRANSPORT_H
