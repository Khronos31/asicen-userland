// SPDX-License-Identifier: GPL-2.0-only
// Injectable acquisition tests: no native libusb operation is executed.
#include "asicen/libusb_transport.h"
#include "asicen/libusb_hardware_backend.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <vector>
#if defined(__linux__) || defined(__ANDROID__)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

#define CHECK(condition, description)                                           \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, description); \
            return false;                                                       \
        }                                                                       \
    } while (false)

class FakeApi final : public asicen::LibusbAcquisitionApi {
public:
    bool valid = true;
    int init_error = 0;
    int list_error = 0;
    int open_failure = 0;
    int descriptor_failure = 0;
    int config_failure = 0;
    int claim_failure = 0;
    int release_failure = 0;
    int release_failure_device = 0;
    int alternate_error = 0;
    int wrap_error = 0;
    int wrap_calls = 0;
    int port_calls = 0;
    int bus_calls = 0;
    int address_calls = 0;
    int descriptor_calls = 0;
    int config_calls = 0;
    int kernel_calls = 0;
    int alternate_calls = 0;
    int release_calls = 0;
    int port_result = 2;
    int port_result_call = 1;
    int retained_fd = -1;
    bool empty_context = false;
    bool empty_handle = false;
    bool handle_on_failure = false;
    bool no_discovery = false;
    std::array<std::uint8_t, 2> addresses_{{2U, 3U}};
    std::array<std::uint16_t, 2> vendors_{{0x0b06U, 0x0b06U}};
    std::array<std::uint16_t, 2> products_{{0x0005U, 0x0005U}};
    std::vector<std::string> events;

    FakeApi()
    {
        static std::uintptr_t next_context = 0x1000U;
        context_ = reinterpret_cast<libusb_context*>(++next_context);
        for (std::size_t index = 0U; index < 2U; ++index) {
            devices_[index] = reinterpret_cast<libusb_device*>(index + 1U);
            endpoints_[index].bEndpointAddress = static_cast<std::uint8_t>(0x81U + index);
            endpoints_[index].bmAttributes = LIBUSB_TRANSFER_TYPE_BULK;
        }
        alternate_.bNumEndpoints = 2U;
        alternate_.endpoint = endpoints_.data();
        interface_.num_altsetting = 1;
        interface_.altsetting = &alternate_;
        config_.bNumInterfaces = 1U;
        config_.interface = &interface_;
    }

    int init(libusb_context** context, bool fd_mode) noexcept override
    {
        events.emplace_back("init");
        no_discovery = fd_mode;
        *context = init_error == 0 && !empty_context ? context_ : nullptr;
        return init_error;
    }
    void exit(libusb_context* context) noexcept override
    {
        verify(context == context_, "context identity retained until exit");
        events.emplace_back("exit");
    }
    ssize_t get_device_list(libusb_context* context, libusb_device*** list) noexcept override
    {
        verify(context == context_, "enumeration uses initialized context");
        events.emplace_back("list");
        *list = list_error == 0 ? devices_.data() : nullptr;
        return list_error == 0 ? 2 : list_error;
    }
    void free_device_list(libusb_device** list, int unref_devices) noexcept override
    {
        verify(list == devices_.data() && unref_devices == 1, "list released with device refs");
        events.emplace_back("free-list");
    }
    std::uint8_t get_bus_number(libusb_device*) noexcept override
    { ++bus_calls; return 1U; }
    std::uint8_t get_device_address(libusb_device* device) noexcept override
    { ++address_calls; return addresses_[static_cast<std::size_t>(id(device) - 1)]; }
    int open(libusb_device* device, libusb_device_handle** handle) noexcept override
    {
        const int number = id(device);
        events.push_back("open" + std::to_string(number));
        *handle = nullptr;
        if (open_failure == number) {
            if (handle_on_failure) *handle = reinterpret_cast<libusb_device_handle*>(device);
            return LIBUSB_ERROR_ACCESS;
        }
        if (!empty_handle) *handle = reinterpret_cast<libusb_device_handle*>(device);
        return 0;
    }
    int wrap_sys_device(libusb_context* context, intptr_t fd,
                        libusb_device_handle** handle) noexcept override
    {
        verify(context == context_, "wrap uses initialized context");
        retained_fd = static_cast<int>(fd);
        events.emplace_back("wrap");
        ++wrap_calls;
        *handle = (wrap_error == 0 || handle_on_failure) && !empty_handle
                      ? reinterpret_cast<libusb_device_handle*>(devices_[wrap_calls - 1]) : nullptr;
        return wrap_error;
    }
    void close(libusb_device_handle* handle) noexcept override
    { events.push_back("close" + std::to_string(id(handle))); }
    libusb_device* get_device(libusb_device_handle* handle) noexcept override
    { return reinterpret_cast<libusb_device*>(handle); }
    int get_device_descriptor(libusb_device* device,
                              libusb_device_descriptor* descriptor) noexcept override
    {
        ++descriptor_calls;
        events.push_back("descriptor" + std::to_string(id(device)));
        if (descriptor_failure == id(device)) return LIBUSB_ERROR_IO;
        descriptor->idVendor = vendors_[static_cast<std::size_t>(id(device) - 1)];
        descriptor->idProduct = products_[static_cast<std::size_t>(id(device) - 1)];
        return 0;
    }
    int get_port_numbers(libusb_device* device, std::uint8_t* ports, int capacity) noexcept override
    {
        ++port_calls;
        verify(capacity >= 2, "port output capacity");
        ports[0] = 2U;
        ports[1] = static_cast<std::uint8_t>(id(device));
        return port_calls == port_result_call ? port_result : 2;
    }
    int get_active_config_descriptor(libusb_device* device,
                                     libusb_config_descriptor** config) noexcept override
    {
        ++config_calls;
        events.push_back("config" + std::to_string(id(device)));
        *config = nullptr;
        if (config_failure == id(device)) return LIBUSB_ERROR_IO;
        *config = &config_;
        return 0;
    }
    void free_config_descriptor(libusb_config_descriptor* config) noexcept override
    {
        verify(config == &config_, "free only acquired configuration");
        events.emplace_back("free-config");
    }
    int kernel_driver_active(libusb_device_handle*, int) noexcept override
    { ++kernel_calls; return 0; }
    int claim_interface(libusb_device_handle* handle, int interface_number) noexcept override
    {
        verify(interface_number == 0, "claim only interface zero");
        events.push_back("claim" + std::to_string(id(handle)));
        return claim_failure == id(handle) ? LIBUSB_ERROR_BUSY : 0;
    }
    int release_interface(libusb_device_handle* handle, int interface_number) noexcept override
    {
        verify(interface_number == 0, "release only interface zero");
        events.push_back("release" + std::to_string(id(handle)));
        ++release_calls;
        return release_failure != 0 &&
                       (release_failure_device == 0 || release_failure_device == id(handle))
                   ? release_failure
                   : 0;
    }
    int set_interface_alt_setting(libusb_device_handle*, int, int) noexcept override
    {
        ++alternate_calls;
        events.emplace_back("alternate");
        return alternate_error;
    }
    void use_alternate_one() noexcept { alternate_.bAlternateSetting = 1U; }

    int snapshot_api_calls() const noexcept
    {
        return bus_calls + address_calls + descriptor_calls + port_calls + config_calls +
               kernel_calls + alternate_calls;
    }

private:
    void verify(bool condition, const char* description) noexcept
    {
        if (!condition) {
            std::fprintf(stderr, "FAIL fake acquisition API: %s\n", description);
            valid = false;
        }
    }
    template<class T> static int id(T* value) noexcept
    { return static_cast<int>(reinterpret_cast<std::uintptr_t>(value)); }
    libusb_context* context_ = nullptr;
    std::array<libusb_device*, 3U> devices_{};
    std::array<libusb_endpoint_descriptor, 2U> endpoints_{};
    libusb_interface_descriptor alternate_{};
    libusb_interface interface_{};
    libusb_config_descriptor config_{};
};

bool cleanup_order(const FakeApi& api, std::initializer_list<const char*> expected)
{
    CHECK(api.valid, "injected API preconditions and ownership identities");
    std::vector<std::string> actual;
    for (const auto& event : api.events) {
        if (event.rfind("release", 0U) == 0U || event.rfind("close", 0U) == 0U || event == "exit")
            actual.push_back(event);
    }
    std::vector<std::string> wanted;
    for (const char* event : expected) wanted.emplace_back(event);
    CHECK(actual == wanted, "total release-interface -> close-handle -> exit-context order");
    return true;
}

bool initialization_and_open_failures()
{
#if defined(_WIN32)
    constexpr auto access_error = px4::userland::Error::BUSY;
#else
    constexpr auto access_error = px4::userland::Error::USB_IO;
#endif
    CHECK(asicen::map_libusb_acquisition_error(LIBUSB_ERROR_ACCESS) == access_error,
          "Windows claimed-device ACCESS maps to BUSY without changing POSIX errors");
    for (bool empty : {false, true}) {
        FakeApi api;
        api.init_error = empty ? 0 : LIBUSB_ERROR_NO_MEM;
        api.empty_context = empty;
        {
            asicen::LibusbContext context(api);
            CHECK(context.initialize() == (empty ? LIBUSB_ERROR_OTHER : LIBUSB_ERROR_NO_MEM),
                  "failed/null initialization is preserved");
            CHECK(context.get() == nullptr, "failed init owns no context");
        }
        CHECK(api.events == std::vector<std::string>{"init"}, "failed init is not exited");
    }
    for (int failure : {1, 2, 3, 4}) {
        FakeApi api;
        api.list_error = failure == 1 ? LIBUSB_ERROR_IO : 0;
        api.open_failure = failure == 2 || failure == 4 ? 1 : 0;
        api.empty_handle = failure == 3;
        api.handle_on_failure = failure == 4;
        {
            asicen::LibusbContext context(api);
            CHECK(context.initialize() == 0, "initialize fake native context");
            CHECK(context.initialize() == LIBUSB_ERROR_BUSY, "cannot replace an owned context");
            asicen::LibusbDevice device(api);
            const int result = device.open(context.get(), asicen::UsbLocation{1U, 2U});
            const int expected = failure == 1 ? LIBUSB_ERROR_IO :
                                 failure == 3 ? LIBUSB_ERROR_OTHER : LIBUSB_ERROR_ACCESS;
            CHECK(result == expected && !device.is_open(), "open failure owns no handle");
            CHECK(device.claim_interface(0) == LIBUSB_ERROR_NO_DEVICE,
                  "claim cannot follow failed acquisition");
        }
        if (failure == 4) CHECK(cleanup_order(api, {"close1", "exit"}), "cleanup order");
        else CHECK(cleanup_order(api, {"exit"}), "cleanup order");
        CHECK(api.events.back() == "exit", "failure exits after list cleanup");
    }
    FakeApi api;
    api.open_failure = 2;
    {
        asicen::LibusbContext context(api);
        CHECK(context.initialize() == 0, "initialize partial-open context");
        asicen::LibusbDevice primary(api);
        asicen::LibusbDevice sibling(api);
        CHECK(primary.open(context.get(), asicen::UsbLocation{1U, 2U}) == 0, "primary opens");
        CHECK(sibling.open(context.get(), asicen::UsbLocation{1U, 3U}) == LIBUSB_ERROR_ACCESS,
              "sibling open failure preserved");
    }
    CHECK(cleanup_order(api, {"close1", "exit"}), "cleanup order");
    return true;
}

bool ownership_failure_and_success_order()
{
    // Exercise actual native snapshot + enclosure rollback, not a replacement
    // ownership implementation: descriptors/configs fail before any claim.
    for (int failure = 0; failure < 7; ++failure) {
        FakeApi api;
        api.descriptor_failure = failure == 1 ? 1 : failure == 5 ? 2 : 0;
        api.config_failure = failure == 2 ? 1 : failure == 6 ? 2 : 0;
        api.claim_failure = failure == 3 ? 1 : failure == 4 ? 2 : 0;
        {
            asicen::LibusbContext context(api);
            CHECK(context.initialize() == 0, "initialize ownership context");
            asicen::LibusbDevice primary(api);
            asicen::LibusbDevice sibling(api);
            CHECK(primary.open(context.get(), asicen::UsbLocation{1U, 2U}) == 0, "primary opens");
            CHECK(sibling.open(context.get(), asicen::UsbLocation{1U, 3U}) == 0, "sibling opens");
            asicen::LibusbFunctionClaim primary_claim(primary);
            asicen::LibusbFunctionClaim sibling_claim(sibling);
            asicen::EnclosureOwnership ownership;
            const auto result = ownership.claim_w3u3(primary_claim, sibling_claim, {2U, 1U}, {2U, 2U});
            const auto expected = failure == 0 ? asicen::OwnershipError::none :
                                  failure <= 2 || failure >= 5 ? asicen::OwnershipError::snapshot_failed :
                                  failure == 3 ? asicen::OwnershipError::primary_claim_failed :
                                                 asicen::OwnershipError::sibling_claim_failed;
            CHECK(result == expected, "descriptor/config/claim failure reaches actual ownership boundary");
            CHECK(ownership.last_transport_error() ==
                      (failure == 0 ? 0 : failure == 3 || failure == 4 ? LIBUSB_ERROR_BUSY : LIBUSB_ERROR_IO),
                  "native acquisition errors survive the ownership boundary");
            CHECK(ownership.owns_both() == (failure == 0), "partial acquisition never claims full ownership");
        }
        if (failure == 0) CHECK(cleanup_order(api, {"release2", "release1", "close2", "close1", "exit"}), "cleanup order");
        else if (failure == 4) CHECK(cleanup_order(api, {"release1", "close2", "close1", "exit"}), "cleanup order");
        else CHECK(cleanup_order(api, {"close2", "close1", "exit"}), "cleanup order");
    }
    return true;
}

std::string contents(FILE* file)
{
    std::fflush(file);
    std::rewind(file);
    std::string output;
    char buffer[512];
    for (;;) {
        const std::size_t count = std::fread(buffer, 1U, sizeof(buffer), file);
        output.append(buffer, count);
        if (count < sizeof(buffer)) break;
    }
    return output;
}

bool claim_diagnostics_are_cached_and_do_not_touch_usb()
{
    FILE* output = std::tmpfile();
    CHECK(output != nullptr, "create diagnostic output stream");
    FakeApi api;
    api.vendors_[1] = 0x1738U;
    api.products_[1] = 0x5211U;
    api.addresses_[1] = 7U;
    {
        asicen::LibusbContext context(api);
        CHECK(context.initialize() == 0, "initialize diagnostic context");
        asicen::LibusbDevice primary(api);
        asicen::LibusbDevice sibling(api);
        CHECK(primary.open(context.get(), asicen::UsbLocation{1U, 2U}) == 0 &&
                  sibling.open(context.get(), asicen::UsbLocation{1U, 7U}) == 0,
              "diagnostic functions open");
        asicen::LibusbFunctionClaim primary_claim(primary, "primary", output);
        asicen::LibusbFunctionClaim sibling_claim(sibling, "sibling", output);
        asicen::EnclosureOwnership ownership;
        CHECK(ownership.claim_w3u3(primary_claim, sibling_claim, {2U, 1U}, {2U, 2U}) ==
                  asicen::OwnershipError::none,
              "runtime primary and loader sibling claim");
        const int snapshot_calls = api.snapshot_api_calls();
        const auto before_diagnostics = api.events;
        primary_claim.log_claim_diagnostic();
        sibling_claim.log_claim_diagnostic();
        CHECK(api.events == before_diagnostics, "claim logging performs no USB calls");
        CHECK(api.snapshot_api_calls() == snapshot_calls,
              "claim logging calls no snapshot or identity getters");

        // The loader returns as runtime at a new address after ownership was
        // established. Diagnostics must retain its original loader identity.
        api.vendors_[1] = 0x0b06U;
        api.products_[1] = 0x0005U;
        api.addresses_[1] = 9U;
        api.release_failure = LIBUSB_ERROR_NO_DEVICE;
        api.release_failure_device = 2;
        const int calls_before_release = api.snapshot_api_calls();
        CHECK(ownership.release() == asicen::OwnershipError::release_failed,
              "release failure remains visible");
        api.release_failure = 0;
        CHECK(ownership.release() == asicen::OwnershipError::none,
              "release retry preserves success result");
        CHECK(api.snapshot_api_calls() == calls_before_release,
              "release diagnostics make no snapshot or identity getter calls");
        CHECK(api.events.size() >= 3U &&
                  api.events[api.events.size() - 3U] == "release2" &&
                  api.events[api.events.size() - 2U] == "release1" &&
                  api.events[api.events.size() - 1U] == "release2" &&
                  std::count(api.events.begin(), api.events.end(), "release1") == 1,
              "primary releases once and failed sibling alone is retried");
        CHECK(api.release_calls == 3, "only the failed sibling release is retried");
    }
    const std::string log = contents(output);
    std::fclose(output);
    CHECK(log.find("phase=claim role=primary bus=1 address=2 port=1-2.1 vid=0b06 "
                   "pid=0005 interface=0 rc=0 name=LIBUSB_SUCCESS") != std::string::npos,
          "primary claim records cached identity and raw result");
    CHECK(log.find("phase=claim role=sibling bus=1 address=7 port=1-2.2 vid=1738 "
                   "pid=5211 interface=0 rc=0 name=LIBUSB_SUCCESS") != std::string::npos,
          "sibling claim records loader identity and raw result");
    CHECK(log.find("phase=release role=sibling bus=1 address=7 port=1-2.2 vid=1738 "
                   "pid=5211 interface=0 rc=-4 name=LIBUSB_ERROR_NO_DEVICE") != std::string::npos,
          "failed release reports original loader identity and raw error");
    CHECK(log.find("phase=release role=sibling bus=1 address=7 port=1-2.2 vid=1738 "
                   "pid=5211 interface=0 rc=0 name=LIBUSB_SUCCESS") != std::string::npos,
          "sibling retry reports cached loader identity");
    CHECK(log.find("phase=release role=primary bus=1 address=2 port=1-2.1 vid=0b06 "
                   "pid=0005 interface=0 rc=0 name=LIBUSB_SUCCESS") != std::string::npos,
          "primary release succeeds once");
    CHECK(log.find("address=9") == std::string::npos,
          "release logs retain loader values after fake re-enumeration");
    return true;
}

bool failed_claim_logs_raw_error()
{
    FILE* output = std::tmpfile();
    CHECK(output != nullptr, "create failed-claim diagnostic stream");
    FakeApi api;
    api.claim_failure = 1;
    {
        asicen::LibusbContext context(api);
        CHECK(context.initialize() == 0, "initialize failed-claim context");
        asicen::LibusbDevice primary(api);
        asicen::LibusbDevice sibling(api);
        CHECK(primary.open(context.get(), asicen::UsbLocation{1U, 2U}) == 0 &&
                  sibling.open(context.get(), asicen::UsbLocation{1U, 3U}) == 0,
              "failed-claim functions open");
        asicen::LibusbFunctionClaim primary_claim(primary, "primary", output);
        asicen::LibusbFunctionClaim sibling_claim(sibling, "sibling", output);
        asicen::EnclosureOwnership ownership;
        CHECK(ownership.claim_w3u3(primary_claim, sibling_claim, {2U, 1U}, {2U, 2U}) ==
                  asicen::OwnershipError::primary_claim_failed,
              "primary claim failure remains visible");
        const int calls = api.snapshot_api_calls();
        primary_claim.log_claim_diagnostic();
        sibling_claim.log_claim_diagnostic();
        CHECK(api.snapshot_api_calls() == calls,
              "failed claim logging performs no snapshot or identity getter calls");
    }
    const std::string log = contents(output);
    std::fclose(output);
    CHECK(log.find("phase=claim role=primary bus=1 address=2 port=1-2.1 vid=0b06 "
                   "pid=0005 interface=0 rc=-6 name=LIBUSB_ERROR_BUSY") != std::string::npos,
          "failed claim logs raw libusb error");
    CHECK(log.find("phase=claim role=sibling") != std::string::npos &&
              log.find("attempt=no rc=unknown name=unknown") != std::string::npos,
          "sibling claim not attempted is not reported as success");
    return true;
}

bool failed_second_snapshot_invalidates_cached_diagnostics()
{
    FILE* output = std::tmpfile();
    CHECK(output != nullptr, "create failed-snapshot diagnostic stream");
    FakeApi api;
    {
        asicen::LibusbContext context(api);
        CHECK(context.initialize() == 0, "initialize failed-snapshot context");
        asicen::LibusbDevice device(api);
        CHECK(device.open(context.get(), asicen::UsbLocation{1U, 2U}) == 0,
              "failed-snapshot device opens");
        asicen::LibusbFunctionClaim claim(device, "primary", output);
        CHECK(claim.snapshot().transport_error == 0, "first diagnostic snapshot succeeds");
        CHECK(claim.claim_interface0() == 0, "claim attempt records a successful raw result");
        api.descriptor_failure = 1;
        CHECK(claim.snapshot().transport_error == LIBUSB_ERROR_IO,
              "second diagnostic snapshot fails at descriptor read");
        claim.log_claim_diagnostic();
    }
    const std::string log = contents(output);
    std::fclose(output);
    CHECK(log.find("bus=1 address=2 port=unknown vid=unknown pid=unknown") != std::string::npos,
          "failed second snapshot invalidates stale identity fields");
    CHECK(log.find("attempt=no rc=unknown name=unknown") != std::string::npos,
          "failed second snapshot resets the previous claim attempt");
    return true;
}

bool unattempted_claim_is_not_reported_as_success()
{
    FILE* output = std::tmpfile();
    CHECK(output != nullptr, "create unattempted diagnostic stream");
    FakeApi api;
    {
        asicen::LibusbContext context(api);
        CHECK(context.initialize() == 0, "initialize unattempted diagnostic context");
        asicen::LibusbDevice device(api);
        CHECK(device.open(context.get(), asicen::UsbLocation{1U, 2U}) == 0,
              "unattempted diagnostic device opens");
        asicen::LibusbFunctionClaim claim(device, "primary", output);
        const auto before_diagnostics = api.events;
        claim.log_claim_diagnostic();
        CHECK(api.events == before_diagnostics, "logging an unattempted claim makes no USB calls");
    }
    const std::string log = contents(output);
    std::fclose(output);
    CHECK(log.find("phase=claim role=primary") != std::string::npos &&
              log.find("attempt=no rc=unknown name=unknown") != std::string::npos,
          "unattempted claim has unknown result, never success");
    return true;
}

bool alternate_failure_releases_once()
{
    FakeApi api;
    api.use_alternate_one();
    api.alternate_error = LIBUSB_ERROR_IO;
    {
        asicen::LibusbContext context(api);
        CHECK(context.initialize() == 0, "initialize endpoint context");
        asicen::LibusbDevice device(api);
        CHECK(device.open(context.get(), asicen::UsbLocation{1U, 2U}) == 0, "endpoint device opens");
        CHECK(device.claim_endpoint(0x81U) == LIBUSB_ERROR_IO, "alternate failure retained");
    }
    CHECK(cleanup_order(api, {"release1", "close1", "exit"}), "cleanup order");
    return true;
}

bool granted_fd_failures_and_success()
{
#if defined(__linux__) || defined(__ANDROID__)
    for (int failure = 0; failure < 4; ++failure) {
        struct Pipe final {
            int descriptors[2] = {-1, -1};
            ~Pipe() noexcept
            {
                for (int fd : descriptors) if (fd >= 0) (void)::close(fd);
            }
        } pipe;
        auto& descriptors = pipe.descriptors;
        CHECK(::pipe(descriptors) == 0, "create ordinary test pipe");
        FakeApi api;
        api.wrap_error = failure == 1 || failure == 3 ? LIBUSB_ERROR_NO_DEVICE : 0;
        api.empty_handle = failure == 2;
        api.handle_on_failure = failure == 3;
        {
            asicen::LibusbContext context(api);
            CHECK(context.initialize(true) == 0 && api.no_discovery, "FD mode disables discovery");
            asicen::LibusbDevice device(api);
            const int result = device.open(context.get(), descriptors[0]);
            CHECK(result == (failure == 0 ? 0 : failure == 2 ? LIBUSB_ERROR_OTHER : LIBUSB_ERROR_NO_DEVICE),
                  "wrap error and null-handle result preserved");
            CHECK(api.retained_fd != descriptors[0] && api.retained_fd >= 0,
                  "only an owned duplicate reaches wrap");
            CHECK(::fcntl(descriptors[0], F_GETFD) >= 0, "caller FD remains owned by caller");
            if (failure == 0) {
                CHECK((::fcntl(api.retained_fd, F_GETFD) & FD_CLOEXEC) != 0, "retained FD is CLOEXEC");
                CHECK(device.claim_interface(0) == 0, "wrapped interface claim");
                device.close();
                device.close();
            }
            CHECK(::fcntl(api.retained_fd, F_GETFD) < 0, "private duplicate closed on every completed path");
            CHECK(::fcntl(descriptors[0], F_GETFD) >= 0, "cleanup never closes caller FD");
        }
        if (failure == 0) CHECK(cleanup_order(api, {"release1", "close1", "exit"}), "cleanup order");
        else if (failure == 3) CHECK(cleanup_order(api, {"close1", "exit"}), "cleanup order");
        else CHECK(cleanup_order(api, {"exit"}), "cleanup order");
    }
#endif
    return true;
}

bool port_accessor_preserves_errors_and_bounds()
{
    for (int result : std::initializer_list<int>{LIBUSB_ERROR_TIMEOUT, LIBUSB_ERROR_BUSY,
                                                LIBUSB_ERROR_NO_MEM, LIBUSB_ERROR_NO_DEVICE,
                                                0, 2, 9}) {
        FakeApi api;
        api.port_result = result;
        {
            asicen::LibusbContext context(api);
            CHECK(context.initialize() == 0, "initialize port query context");
            asicen::LibusbDevice device(api);
            std::uint8_t ports[8]{};
            CHECK(device.port_numbers(ports, 8) == LIBUSB_ERROR_NO_DEVICE,
                  "closed device cannot query topology");
            CHECK(device.open(context.get(), asicen::UsbLocation{1U, 2U}) == 0,
                  "port query device opens");
            CHECK(device.port_numbers(nullptr, 8) == LIBUSB_ERROR_INVALID_PARAM &&
                      device.port_numbers(ports, 0) == LIBUSB_ERROR_INVALID_PARAM,
                  "invalid port buffers do not reach the adapter");
            CHECK(api.port_calls == 0, "invalid and closed queries are local failures");
            CHECK(device.port_numbers(ports, 8) ==
                      (result > 8 ? LIBUSB_ERROR_OVERFLOW : result),
                  "port count bounds and exact transport errors are retained");
            CHECK(api.port_calls == 1, "one valid query reaches the injected adapter");
        }
        CHECK(cleanup_order(api, {"close1", "exit"}), "port accessor cleanup order");
    }
    return true;
}

bool granted_fd_topology_failures()
{
#if defined(__linux__) || defined(__ANDROID__)
    struct Pipe final {
        int descriptors[2] = {-1, -1};
        ~Pipe() noexcept
        {
            for (int fd : descriptors) {
                if (fd >= 0) {
                    (void)::close(fd);
                }
            }
        }
    } pipe;
    CHECK(::pipe(pipe.descriptors) == 0, "create topology test descriptors");
    for (int failure_call : {1, 2}) {
        for (int result : std::initializer_list<int>{LIBUSB_ERROR_TIMEOUT, LIBUSB_ERROR_BUSY,
                                                    LIBUSB_ERROR_NO_MEM, LIBUSB_ERROR_NO_DEVICE, 9}) {
            FakeApi api;
            api.port_result_call = failure_call;
            api.port_result = result;
            {
                asicen::LibusbContext context(api);
                CHECK(context.initialize(true) == 0, "initialize FD topology context");
                asicen::LibusbW3u3Hardware hardware(
                    context.get(), pipe.descriptors[0], pipe.descriptors[1],
                    asicen::find_profile(asicen::ModelId::W3u3), api);
                const auto claimed = hardware.claim();
                const int expected = result > 8 ? LIBUSB_ERROR_OVERFLOW : result;
                CHECK(!claimed && claimed.error() == asicen::map_libusb_acquisition_error(expected),
                      "both FD topology failures retain their acquisition error");
                CHECK(api.port_calls == failure_call && api.wrap_calls == failure_call,
                      "topology failure stops acquisition before later USB operations");
            }
            if (failure_call == 1) {
                CHECK(cleanup_order(api, {"close1", "exit"}), "primary topology cleanup");
            } else {
                CHECK(cleanup_order(api, {"close1", "close2", "exit"}), "sibling topology cleanup");
            }
            CHECK(::fcntl(api.retained_fd, F_GETFD) < 0, "failed topology releases retained FD");
            CHECK(::fcntl(pipe.descriptors[0], F_GETFD) >= 0 &&
                      ::fcntl(pipe.descriptors[1], F_GETFD) >= 0,
                  "failed topology preserves both caller FDs");
        }
    }
    FakeApi api;
    api.descriptor_failure = 1;
    api.port_result = LIBUSB_ERROR_TIMEOUT;
    {
        asicen::LibusbContext context(api);
        CHECK(context.initialize(true) == 0, "initialize descriptor failure context");
        asicen::LibusbW3u3Hardware hardware(
            context.get(), pipe.descriptors[0], pipe.descriptors[1], nullptr, api);
        const auto claimed = hardware.claim();
        CHECK(!claimed && claimed.error() == px4::userland::Error::USB_IO,
              "descriptor failure precedes topology failure");
        CHECK(api.port_calls == 0, "failed descriptor never queries topology");
    }
    CHECK(cleanup_order(api, {"close1", "exit"}), "descriptor failure cleanup");
#endif
    return true;
}

bool quarantine_retains_context_and_handle()
{
    FakeApi quarantined;
    {
        asicen::LibusbContext context(quarantined);
        CHECK(context.initialize() == 0, "initialize quarantine context");
        asicen::LibusbDevice device(quarantined);
        CHECK(device.open(context.get(), asicen::UsbLocation{1U, 2U}) == 0, "quarantine device opens");
        CHECK(device.claim_interface(0) == 0, "quarantine interface claimed");
        device.abandon();
        device.abandon();
        CHECK(!device.is_open(), "abandoned owner cannot access retained handle");
        context.close();
        context.close();
    }
    CHECK(cleanup_order(quarantined, {}), "cleanup order");
    FakeApi unrelated;
    {
        asicen::LibusbContext context(unrelated);
        CHECK(context.initialize() == 0, "unrelated context still initializes");
    }
    CHECK(cleanup_order(unrelated, {"exit"}), "cleanup order");
    return true;
}

} // namespace

int main()
{
    if (!initialization_and_open_failures() || !ownership_failure_and_success_order() ||
        !claim_diagnostics_are_cached_and_do_not_touch_usb() ||
        !failed_claim_logs_raw_error() ||
        !failed_second_snapshot_invalidates_cached_diagnostics() ||
        !unattempted_claim_is_not_reported_as_success() ||
        !alternate_failure_releases_once() || !granted_fd_failures_and_success() ||
        !port_accessor_preserves_errors_and_bounds() || !granted_fd_topology_failures() ||
        !quarantine_retains_context_and_handle()) {
        return 1;
    }
    std::puts("libusb acquisition/lifetime tests passed (all USB calls injected)");
    return 0;
}
