// SPDX-License-Identifier: GPL-2.0-only
#include "asicen/libusb_transport.h"

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <mutex>
#include <new>

namespace asicen {
namespace {

class NativeLibusbAcquisitionApi final : public LibusbAcquisitionApi {
public:
    int init(libusb_context** context, bool no_device_discovery) noexcept override;
    void exit(libusb_context* context) noexcept override
    {
#if defined(__APPLE__)
        (void)context;
#else
        libusb_exit(context);
#endif
    }
    ssize_t get_device_list(libusb_context* context, libusb_device*** list) noexcept override
    { return libusb_get_device_list(context, list); }
    void free_device_list(libusb_device** list, int unref_devices) noexcept override
    { libusb_free_device_list(list, unref_devices); }
    std::uint8_t get_bus_number(libusb_device* device) noexcept override
    { return libusb_get_bus_number(device); }
    std::uint8_t get_device_address(libusb_device* device) noexcept override
    { return libusb_get_device_address(device); }
    int open(libusb_device* device, libusb_device_handle** handle) noexcept override
    { return libusb_open(device, handle); }
    int wrap_sys_device(libusb_context* context, intptr_t fd,
                        libusb_device_handle** handle) noexcept override
    {
#if defined(__linux__) || defined(__ANDROID__)
        return libusb_wrap_sys_device(context, fd, handle);
#else
        (void)context;
        (void)fd;
        (void)handle;
        return LIBUSB_ERROR_NOT_SUPPORTED;
#endif
    }
    void close(libusb_device_handle* handle) noexcept override { libusb_close(handle); }
    libusb_device* get_device(libusb_device_handle* handle) noexcept override
    { return libusb_get_device(handle); }
    int get_device_descriptor(libusb_device* device,
                              libusb_device_descriptor* descriptor) noexcept override
    { return libusb_get_device_descriptor(device, descriptor); }
    int get_port_numbers(libusb_device* device, std::uint8_t* ports, int capacity) noexcept override
    { return libusb_get_port_numbers(device, ports, capacity); }
    int get_active_config_descriptor(libusb_device* device,
                                     libusb_config_descriptor** config) noexcept override
    { return libusb_get_active_config_descriptor(device, config); }
    void free_config_descriptor(libusb_config_descriptor* config) noexcept override
    { libusb_free_config_descriptor(config); }
    int kernel_driver_active(libusb_device_handle* handle, int interface_number) noexcept override
    { return libusb_kernel_driver_active(handle, interface_number); }
    int claim_interface(libusb_device_handle* handle, int interface_number) noexcept override
    { return libusb_claim_interface(handle, interface_number); }
    int release_interface(libusb_device_handle* handle, int interface_number) noexcept override
    { return libusb_release_interface(handle, interface_number); }
    int set_interface_alt_setting(libusb_device_handle* handle, int interface_number,
                                  int alternate_setting) noexcept override
    { return libusb_set_interface_alt_setting(handle, interface_number, alternate_setting); }
};

std::mutex quarantined_context_mutex;
struct QuarantinedContext {
    libusb_context* context;
    QuarantinedContext* next;
};
QuarantinedContext* quarantined_contexts = nullptr;
bool quarantine_all_contexts = false;

bool context_is_quarantined(libusb_context* context) noexcept
{
    for (auto* node = quarantined_contexts; node != nullptr; node = node->next) {
        if (node->context == context) {
            return true;
        }
    }
    return quarantine_all_contexts;
}
} // namespace

void quarantine_libusb_context(libusb_context* context) noexcept
{
    if (context == nullptr) {
        return;
    }
    const std::lock_guard<std::mutex> lock(quarantined_context_mutex);
    if (!context_is_quarantined(context)) {
        auto* node = new (std::nothrow) QuarantinedContext{context, quarantined_contexts};
        if (node == nullptr) {
            // Allocation failure must not destroy a context with pending URBs.
            quarantine_all_contexts = true;
        } else {
            quarantined_contexts = node;
        }
    }
}

// Generic error and context lifecycle contract adapted from PX4 userland
// libusb_transport.cpp at 1a1485d0c3e972e0a47be907edb67949564aa9a7.
static_assert(LIBUSB_ERROR_TIMEOUT == -7 && LIBUSB_ERROR_NO_DEVICE == -4 &&
                  LIBUSB_ERROR_BUSY == -6 && LIBUSB_ERROR_NOT_FOUND == -5 &&
                  LIBUSB_ERROR_NOT_SUPPORTED == -12 && LIBUSB_ERROR_INVALID_PARAM == -2 &&
                  LIBUSB_ERROR_NO_MEM == -11,
              "libusb error contract changed");

px4::userland::Error map_libusb_acquisition_error(int error) noexcept
{
#if defined(_WIN32)
    if (error == LIBUSB_ERROR_ACCESS) {
        return px4::userland::Error::BUSY;
    }
#endif
    return map_libusb_error(error);
}

int NativeLibusbAcquisitionApi::init(libusb_context** context, bool fd_mode) noexcept
{
    if (context == nullptr) {
        return LIBUSB_ERROR_INVALID_PARAM;
    }
#if defined(__APPLE__)
    (void)fd_mode;
    // Match PX4's process-lifetime Darwin context to avoid the libusb <=1.0.30
    // detach/shutdown deadlock. Handles still have ordinary scoped ownership.
    static std::mutex context_mutex;
    static libusb_context* process_context = nullptr;
    const std::lock_guard<std::mutex> lock(context_mutex);
    if (process_context != nullptr) {
        *context = process_context;
        return 0;
    }
    libusb_context* created_context = nullptr;
    const int result = libusb_init(&created_context);
    if (result == 0) process_context = created_context;
    *context = created_context;
    return result;
#elif defined(__FreeBSD__)
    if (fd_mode) {
        return LIBUSB_ERROR_NOT_SUPPORTED;
    }
    return libusb_init(context);
#else
    if (!fd_mode) {
        return libusb_init(context);
    }
#if LIBUSB_API_VERSION >= 0x0100010A
    libusb_init_option option{};
    option.option = LIBUSB_OPTION_NO_DEVICE_DISCOVERY;
    option.value.ival = 1;
    return libusb_init_context(context, &option, 1);
#else
    const int result = libusb_set_option(nullptr, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
    if (result != 0) {
        return result;
    }
    return libusb_init(context);
#endif
#endif
}

LibusbAcquisitionApi& native_libusb_acquisition_api() noexcept
{
    static NativeLibusbAcquisitionApi api;
    return api;
}

int initialize_libusb_context(libusb_context** context, bool fd_mode) noexcept
{
    return native_libusb_acquisition_api().init(context, fd_mode);
}

namespace {
void release_context(libusb_context* context, LibusbAcquisitionApi& api) noexcept
{
    if (context == nullptr) {
        return;
    }
    const std::lock_guard<std::mutex> lock(quarantined_context_mutex);
    if (!context_is_quarantined(context)) {
        api.exit(context);
    }
}
} // namespace

void release_libusb_context(libusb_context* context) noexcept
{
    release_context(context, native_libusb_acquisition_api());
}

LibusbContext::~LibusbContext() noexcept
{
    close();
}

int LibusbContext::initialize(bool fd_mode) noexcept
{
    if (context_ != nullptr) return LIBUSB_ERROR_BUSY;
    libusb_context* created_context = nullptr;
    const int result = api_.init(&created_context, fd_mode);
    if (result != 0) return result;
    if (created_context == nullptr) return LIBUSB_ERROR_OTHER;
    context_ = created_context;
    return 0;
}

void LibusbContext::close() noexcept
{
    release_context(context_, api_);
    context_ = nullptr;
}

KernelDriverState classify_kernel_driver_state(int query_result) noexcept
{
    KernelDriverState state;
    if (query_result > 0) {
        state.known = true;
        state.active = true;
    } else if (query_result == 0 || query_result == LIBUSB_ERROR_NOT_SUPPORTED) {
        state.known = true;
        state.active = false;
    }
    return state;
}

int duplicate_fd_cloexec(int fd) noexcept
{
#if defined(_WIN32)
    (void)fd;
    return -1;
#else
#if defined(F_DUPFD_CLOEXEC)
    const int cloexec_fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (cloexec_fd >= 0) {
        return cloexec_fd;
    }
#endif
    const int duplicate_fd = ::dup(fd);
    if (duplicate_fd < 0) {
        return -1;
    }
#if defined(FD_CLOEXEC)
    const int flags = fcntl(duplicate_fd, F_GETFD);
    if (flags >= 0) {
        (void)fcntl(duplicate_fd, F_SETFD, flags | FD_CLOEXEC);
    }
#endif
    return duplicate_fd;
#endif
}

LibusbDevice::~LibusbDevice()
{
    close();
}

int LibusbDevice::open(libusb_context* context, UsbLocation location)
{
    close();
    if (context == nullptr) return LIBUSB_ERROR_INVALID_PARAM;

    libusb_device** list = nullptr;
    const ssize_t count = api_.get_device_list(context, &list);
    if (count < 0) {
        return static_cast<int>(count);
    }

    int result = LIBUSB_ERROR_NO_DEVICE;
    for (ssize_t i = 0; i < count; ++i) {
        libusb_device* candidate = list[i];
        if (api_.get_bus_number(candidate) != location.bus ||
            api_.get_device_address(candidate) != location.address) {
            continue;
        }

        libusb_device_handle* opened = nullptr;
        result = api_.open(candidate, &opened);
        if (result == 0) {
            if (opened == nullptr) {
                result = LIBUSB_ERROR_OTHER;
            } else {
                handle_ = opened;
                context_ = context;
            }
        } else if (opened != nullptr) {
            api_.close(opened);
        }
        break;
    }

    api_.free_device_list(list, 1);
    return result;
}

int LibusbDevice::open(libusb_context* context, int fd)
{
    close();
#if !defined(__linux__) && !defined(__ANDROID__)
    (void)context;
    (void)fd;
    return LIBUSB_ERROR_NOT_SUPPORTED;
#else
    if (context == nullptr || fd < 0 || fcntl(fd, F_GETFD) < 0) {
        return LIBUSB_ERROR_INVALID_PARAM;
    }
    const int retained = duplicate_fd_cloexec(fd);
    if (retained < 0) {
        return LIBUSB_ERROR_IO;
    }

    libusb_device_handle* handle = nullptr;
    const int wrap = api_.wrap_sys_device(context, static_cast<intptr_t>(retained), &handle);
    if (wrap != 0 || handle == nullptr) {
        if (handle != nullptr) {
            api_.close(handle);
        }
        ::close(retained);
        return wrap != 0 ? wrap : LIBUSB_ERROR_OTHER;
    }

    handle_ = handle;
    context_ = context;
    retained_fd_ = retained;
    return 0;
#endif
}

void LibusbDevice::close()
{
    if (handle_ != nullptr) {
        for (auto it = claimed_interfaces_.rbegin(); it != claimed_interfaces_.rend(); ++it) {
            api_.release_interface(handle_, *it);
        }
        claimed_interfaces_.clear();
        api_.close(handle_);
        handle_ = nullptr;
    } else {
        claimed_interfaces_.clear();
    }
    if (retained_fd_ >= 0) {
#if defined(__linux__) || defined(__ANDROID__)
        ::close(retained_fd_);
#endif
        retained_fd_ = -1;
    }
    context_ = nullptr;
}

void LibusbDevice::abandon() noexcept
{
    quarantine_libusb_context(context_);
    context_ = nullptr;
    handle_ = nullptr;
    retained_fd_ = -1;
    claimed_interfaces_.clear();
}

bool LibusbDevice::is_open() const
{
    return handle_ != nullptr;
}

libusb_device_handle* LibusbDevice::handle() const
{
    return handle_;
}

libusb_device* LibusbDevice::device() const
{
    return handle_ == nullptr ? nullptr : api_.get_device(handle_);
}

int LibusbDevice::descriptor(libusb_device_descriptor* output) const noexcept
{
    if (output == nullptr) return LIBUSB_ERROR_INVALID_PARAM;
    if (handle_ == nullptr) return LIBUSB_ERROR_NO_DEVICE;
    return api_.get_device_descriptor(device(), output);
}

int LibusbDevice::port_numbers(std::uint8_t* ports, int capacity) const noexcept
{
    if (ports == nullptr || capacity <= 0) {
        return LIBUSB_ERROR_INVALID_PARAM;
    }
    libusb_device* dev = device();
    if (dev == nullptr) {
        return LIBUSB_ERROR_NO_DEVICE;
    }
    const int count = api_.get_port_numbers(dev, ports, capacity);
    return count > capacity ? LIBUSB_ERROR_OVERFLOW : count;
}

int LibusbDevice::control(const ControlTransfer& transfer, unsigned char* data)
{
    if (!transfer.valid || (transfer.length != 0U && data == nullptr)) {
        return LIBUSB_ERROR_INVALID_PARAM;
    }
    if (handle_ == nullptr) {
        return LIBUSB_ERROR_NO_DEVICE;
    }

    return libusb_control_transfer(handle_, bm_request_type(transfer.direction),
                                   static_cast<std::uint8_t>(transfer.request), transfer.value,
                                   transfer.index, data, transfer.length, transfer.timeout_ms);
}

int LibusbDevice::vendor_out(std::uint8_t request, std::uint16_t value, std::uint16_t index,
                             const unsigned char* data, std::uint16_t length,
                             unsigned int timeout_ms)
{
    if (length != 0U && data == nullptr) {
        return LIBUSB_ERROR_INVALID_PARAM;
    }
    if (handle_ == nullptr) {
        return LIBUSB_ERROR_NO_DEVICE;
    }
    return libusb_control_transfer(handle_, kVendorOut, request, value, index,
                                   const_cast<unsigned char*>(data), length, timeout_ms);
}

int LibusbDevice::kernel_driver_active(int interface_number) const
{
    if (handle_ == nullptr) {
        return LIBUSB_ERROR_NO_DEVICE;
    }
    return api_.kernel_driver_active(handle_, interface_number);
}

int LibusbDevice::claim_interface(int interface_number)
{
    if (handle_ == nullptr) {
        return LIBUSB_ERROR_NO_DEVICE;
    }
    if (interface_claimed(interface_number)) {
        return interface_number;
    }

    const int claim_rc = api_.claim_interface(handle_, interface_number);
    if (claim_rc != 0) {
        return claim_rc;
    }

    claimed_interfaces_.push_back(interface_number);
    return interface_number;
}

int LibusbDevice::release_interface(int interface_number) noexcept
{
    if (handle_ == nullptr) {
        return LIBUSB_ERROR_NO_DEVICE;
    }
    const auto it =
        std::find(claimed_interfaces_.begin(), claimed_interfaces_.end(), interface_number);
    if (it == claimed_interfaces_.end()) {
        return 0;
    }
    const int rc = api_.release_interface(handle_, interface_number);
    if (rc == 0) {
        claimed_interfaces_.erase(it);
    }
    return rc;
}

bool LibusbDevice::interface_claimed(int interface_number) const
{
    return std::find(claimed_interfaces_.begin(), claimed_interfaces_.end(), interface_number) !=
           claimed_interfaces_.end();
}

int LibusbDevice::claim_endpoint(std::uint8_t endpoint_address)
{
    if (handle_ == nullptr) {
        return LIBUSB_ERROR_NO_DEVICE;
    }

    libusb_config_descriptor* config = nullptr;
    const int config_rc = api_.get_active_config_descriptor(device(), &config);
    if (config_rc != 0 || config == nullptr) {
        return config_rc != 0 ? config_rc : LIBUSB_ERROR_NOT_FOUND;
    }

    int interface_number = -1;
    int alternate_setting = 0;
    for (int i = 0; i < config->bNumInterfaces && interface_number < 0; ++i) {
        const libusb_interface& iface = config->interface[i];
        for (int a = 0; a < iface.num_altsetting && interface_number < 0; ++a) {
            const libusb_interface_descriptor& alt = iface.altsetting[a];
            for (std::uint8_t e = 0; e < alt.bNumEndpoints; ++e) {
                if (alt.endpoint[e].bEndpointAddress == endpoint_address) {
                    interface_number = alt.bInterfaceNumber;
                    alternate_setting = alt.bAlternateSetting;
                    break;
                }
            }
        }
    }
    api_.free_config_descriptor(config);

    if (interface_number < 0) {
        return LIBUSB_ERROR_NOT_FOUND;
    }
    if (interface_claimed(interface_number)) {
        return interface_number;
    }

    const int claim_rc = api_.claim_interface(handle_, interface_number);
    if (claim_rc != 0) {
        return claim_rc;
    }

    if (alternate_setting != 0) {
        const int alt_rc =
            api_.set_interface_alt_setting(handle_, interface_number, alternate_setting);
        if (alt_rc != 0) {
            api_.release_interface(handle_, interface_number);
            return alt_rc;
        }
    }

    claimed_interfaces_.push_back(interface_number);
    return interface_number;
}

int LibusbDevice::bulk_read(std::uint8_t endpoint_address, unsigned char* data, int length,
                            int* transferred, unsigned int timeout_ms)
{
    if (handle_ == nullptr) {
        return LIBUSB_ERROR_NO_DEVICE;
    }
    if (data == nullptr || transferred == nullptr || length <= 0 ||
        (endpoint_address & LIBUSB_ENDPOINT_DIR_MASK) != LIBUSB_ENDPOINT_IN) {
        return LIBUSB_ERROR_INVALID_PARAM;
    }
    *transferred = 0;
    const int result =
        libusb_bulk_transfer(handle_, endpoint_address, data, length, transferred, timeout_ms);
    if (*transferred < 0 || *transferred > length) {
        *transferred = 0;
        return LIBUSB_ERROR_OVERFLOW;
    }
    return result;
}

UsbFunctionSnapshot LibusbFunctionClaim::snapshot() const
{
    UsbFunctionSnapshot result{};
    libusb_device* dev = device_.device();
    if (dev == nullptr) {
        result.transport_error = LIBUSB_ERROR_NO_DEVICE;
        return result;
    }
    result.bus = device_.api_.get_bus_number(dev);
    result.address = device_.api_.get_device_address(dev);
    libusb_device_descriptor descriptor{};
    result.transport_error = device_.api_.get_device_descriptor(dev, &descriptor);
    if (result.transport_error != 0) {
        return result;
    }
    result.vendor_id = descriptor.idVendor;
    result.product_id = descriptor.idProduct;

    std::uint8_t ports[8]{};
    const int port_count = device_.api_.get_port_numbers(dev, ports, static_cast<int>(sizeof(ports)));
    if (port_count < 0 || port_count > static_cast<int>(sizeof(ports))) {
        result.transport_error = port_count < 0 ? port_count : LIBUSB_ERROR_OVERFLOW;
        return result;
    }
    if (port_count > 0 && port_count <= static_cast<int>(sizeof(ports))) {
        result.port_path.assign(ports, ports + port_count);
    }

    libusb_config_descriptor* config = nullptr;
    result.transport_error = device_.api_.get_active_config_descriptor(dev, &config);
    if (result.transport_error != 0 || config == nullptr) {
        if (result.transport_error == 0) result.transport_error = LIBUSB_ERROR_NOT_FOUND;
        return result;
    }
    const libusb_interface_descriptor* interface0 = nullptr;
    bool has_only_alt0 = true;
    for (std::uint8_t i = 0; i < config->bNumInterfaces; ++i) {
        const libusb_interface& iface = config->interface[i];
        for (int a = 0; a < iface.num_altsetting; ++a) {
            const auto& alt = iface.altsetting[a];
            if (alt.bInterfaceNumber != 0) {
                continue;
            }
            if (alt.bAlternateSetting == 0) {
                interface0 = &alt;
            } else {
                has_only_alt0 = false;
            }
        }
    }
    if (interface0 != nullptr) {
        result.interface0_present = true;
        // With exactly one alternate setting (alt 0), the claimed interface
        // cannot be using a nonzero alternate without changing configuration.
        result.active_alt0 = has_only_alt0 ? 0 : -1;
        for (std::uint8_t e = 0; e < interface0->bNumEndpoints; ++e) {
            if (interface0->endpoint[e].bEndpointAddress == 0x81U) {
                result.endpoint81_in_alt0 = true;
                result.endpoint81_bulk_in_alt0 =
                    (interface0->endpoint[e].bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) ==
                    LIBUSB_TRANSFER_TYPE_BULK;
            } else if (interface0->endpoint[e].bEndpointAddress == 0x82U) {
                result.endpoint82_in_alt0 = true;
                result.endpoint82_bulk_in_alt0 =
                    (interface0->endpoint[e].bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) ==
                    LIBUSB_TRANSFER_TYPE_BULK;
            }
        }
    }
    device_.api_.free_config_descriptor(config);
    const int kernel_result = device_.kernel_driver_active(0);
    if (kernel_result < 0 && kernel_result != LIBUSB_ERROR_NOT_SUPPORTED) {
        result.transport_error = kernel_result;
    }
    const KernelDriverState kernel = classify_kernel_driver_state(kernel_result);
    result.kernel_driver_state_known = kernel.known;
    result.interface0_kernel_driver = kernel.active;
    return result;
}

int LibusbFunctionClaim::claim_interface0()
{
    return device_.claim_interface(0);
}

int LibusbFunctionClaim::release_interface0() noexcept
{
    return device_.release_interface(0);
}

} // namespace asicen
