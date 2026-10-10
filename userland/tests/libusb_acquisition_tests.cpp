// SPDX-License-Identifier: GPL-2.0-only
// Injectable acquisition tests: no native libusb operation is executed.
#include "asicen/libusb_transport.h"
#include "asicen/libusb_hardware_backend.h"

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
    int alternate_error = 0;
    int wrap_error = 0;
    int wrap_calls = 0;
    int port_calls = 0;
    int port_result = 2;
    int port_result_call = 1;
    int retained_fd = -1;
    bool empty_context = false;
    bool empty_handle = false;
    bool handle_on_failure = false;
    bool no_discovery = false;
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
    std::uint8_t get_bus_number(libusb_device*) noexcept override { return 1U; }
    std::uint8_t get_device_address(libusb_device* device) noexcept override
    { return static_cast<std::uint8_t>(id(device) + 1); }
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
        events.push_back("descriptor" + std::to_string(id(device)));
        if (descriptor_failure == id(device)) return LIBUSB_ERROR_IO;
        descriptor->idVendor = 0x0b06U;
        descriptor->idProduct = 0x0005U;
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
    int kernel_driver_active(libusb_device_handle*, int) noexcept override { return 0; }
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
        return 0;
    }
    int set_interface_alt_setting(libusb_device_handle*, int, int) noexcept override
    {
        events.emplace_back("alternate");
        return alternate_error;
    }
    void use_alternate_one() noexcept { alternate_.bAlternateSetting = 1U; }

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
        !alternate_failure_releases_once() || !granted_fd_failures_and_success() ||
        !port_accessor_preserves_errors_and_bounds() || !granted_fd_topology_failures() ||
        !quarantine_retains_context_and_handle()) {
        return 1;
    }
    std::puts("libusb acquisition/lifetime tests passed (all USB calls injected)");
    return 0;
}
