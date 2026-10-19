// SPDX-License-Identifier: GPL-2.0-only
// Cold-startup observation/re-acquisition through the injectable libusb seam.
// Each fake context keeps the device snapshot it saw at init, modelling a
// pre-upload context that does not show the re-enumerated runtime. No native
// libusb operation is executed.
#include "asicend_fresh_usb.h"
#include "asicend_identity.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            std::fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__,         \
                         #condition);                                                       \
            return false;                                                                   \
        }                                                                                   \
    } while (false)

using Error = px4::userland::Error;

struct FakeDevice {
    std::size_t context = 0U;
    std::uint8_t bus = 0U;
    std::uint8_t address = 0U;
    std::uint16_t vid = 0U;
    std::uint16_t pid = 0U;
    std::vector<std::uint8_t> ports;
};

class FakeApi final : public asicen::LibusbAcquisitionApi {
public:
    struct Context {
        std::vector<std::unique_ptr<FakeDevice>> cache;
        std::vector<libusb_device*> list;
        bool exited = false;
        int open_handles = 0;
    };

    std::vector<FakeDevice> physical;
    std::vector<std::unique_ptr<Context>> contexts;
    std::vector<std::string> events;
    bool violation = false;
    int init_error = 0;
    int list_error = 0;

    std::size_t index(libusb_context* context) const
    {
        return static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(context));
    }
    static FakeDevice* fake(libusb_device* device) { return reinterpret_cast<FakeDevice*>(device); }
    std::size_t live_contexts() const
    {
        std::size_t live = 0U;
        for (const auto& context : contexts) live += context->exited ? 0U : 1U;
        return live;
    }

    int init(libusb_context** context, bool) noexcept override
    {
        if (init_error != 0) {
            events.emplace_back("init-failed");
            return init_error;
        }
        auto created = std::make_unique<Context>();
        const std::size_t id = contexts.size() + 1U;
        for (const FakeDevice& device : physical) {
            created->cache.push_back(std::make_unique<FakeDevice>(device));
            created->cache.back()->context = id;
        }
        contexts.push_back(std::move(created));
        *context = reinterpret_cast<libusb_context*>(static_cast<std::uintptr_t>(id));
        events.push_back("init" + std::to_string(id));
        return 0;
    }
    void exit(libusb_context* context) noexcept override
    {
        Context& state = *contexts[index(context) - 1U];
        if (state.exited || state.open_handles != 0 || !state.list.empty()) violation = true;
        state.exited = true;
        // Anything still pointing at this context's devices now reads garbage.
        for (auto& device : state.cache) {
            device->bus = 0xffU;
            device->address = 0xffU;
            device->vid = 0xffffU;
            device->pid = 0xffffU;
            device->ports.assign(1U, 0xffU);
        }
        events.push_back("exit" + std::to_string(index(context)));
    }
    ssize_t get_device_list(libusb_context* context, libusb_device*** list) noexcept override
    {
        Context& state = *contexts[index(context) - 1U];
        if (state.exited) violation = true;
        events.push_back("list" + std::to_string(index(context)));
        if (list_error != 0) return list_error;
        state.list.clear();
        for (auto& device : state.cache)
            state.list.push_back(reinterpret_cast<libusb_device*>(device.get()));
        *list = state.list.data();
        return static_cast<ssize_t>(state.list.size());
    }
    void free_device_list(libusb_device** list, int unref_devices) noexcept override
    {
        if (unref_devices != 1) violation = true;
        for (std::size_t i = 0U; i < contexts.size(); ++i) {
            if (!contexts[i]->list.empty() && contexts[i]->list.data() == list) {
                contexts[i]->list.clear();
                events.push_back("free" + std::to_string(i + 1U));
                return;
            }
        }
        violation = true;
    }
    std::uint8_t get_bus_number(libusb_device* device) noexcept override { return fake(device)->bus; }
    std::uint8_t get_device_address(libusb_device* device) noexcept override
    { return fake(device)->address; }
    int open(libusb_device* device, libusb_device_handle** handle) noexcept override
    {
        Context& state = *contexts[fake(device)->context - 1U];
        if (state.exited) violation = true;
        ++state.open_handles;
        *handle = reinterpret_cast<libusb_device_handle*>(device);
        events.push_back("open" + std::to_string(fake(device)->context) + ":" +
                         std::to_string(fake(device)->address));
        return 0;
    }
    int wrap_sys_device(libusb_context*, intptr_t, libusb_device_handle**) noexcept override
    { return LIBUSB_ERROR_NOT_SUPPORTED; }
    void close(libusb_device_handle* handle) noexcept override
    {
        FakeDevice* device = reinterpret_cast<FakeDevice*>(handle);
        Context& state = *contexts[device->context - 1U];
        if (state.exited || state.open_handles == 0) violation = true;
        --state.open_handles;
        events.push_back("close" + std::to_string(device->context));
    }
    libusb_device* get_device(libusb_device_handle* handle) noexcept override
    { return reinterpret_cast<libusb_device*>(handle); }
    int get_device_descriptor(libusb_device* device,
                              libusb_device_descriptor* descriptor) noexcept override
    {
        descriptor->idVendor = fake(device)->vid;
        descriptor->idProduct = fake(device)->pid;
        return 0;
    }
    int get_port_numbers(libusb_device* device, std::uint8_t* ports, int capacity) noexcept override
    {
        const auto& path = fake(device)->ports;
        if (static_cast<int>(path.size()) > capacity) return LIBUSB_ERROR_OVERFLOW;
        for (std::size_t i = 0U; i < path.size(); ++i) ports[i] = path[i];
        return static_cast<int>(path.size());
    }
    int get_active_config_descriptor(libusb_device*, libusb_config_descriptor**) noexcept override
    { return LIBUSB_ERROR_NOT_SUPPORTED; }
    void free_config_descriptor(libusb_config_descriptor*) noexcept override {}
    int kernel_driver_active(libusb_device_handle*, int) noexcept override { return 0; }
    int claim_interface(libusb_device_handle*, int) noexcept override { return 0; }
    int release_interface(libusb_device_handle*, int) noexcept override { return 0; }
    int set_interface_alt_setting(libusb_device_handle*, int, int) noexcept override { return 0; }
};

const std::vector<std::uint8_t> kPrimary{2U, 1U};

std::vector<FakeDevice> cold_pair()
{
    return {{0U, 1U, 5U, 0x1738U, 0x5211U, {2U, 1U}},
            {0U, 1U, 6U, 0x1738U, 0x5211U, {2U, 2U}},
            {0U, 1U, 3U, 0x0b06U, 0x0005U, {3U, 1U}}};
}

// Primary loader at 1-2.1 re-enumerates as a W3U3 runtime at a new address.
void reenumerate_primary(FakeApi& api)
{
    api.physical[0].vid = 0x0b06U;
    api.physical[0].pid = 0x0005U;
    api.physical[0].address = 9U;
}

bool pre_upload_context_keeps_old_snapshot_fresh_scan_sees_runtime()
{
    FakeApi api;
    api.physical = cold_pair();
    asicen::LibusbContext pre_upload(api);
    CHECK(pre_upload.initialize() == 0);
    reenumerate_primary(api);

    // The pre-upload context still lists the loader at its old address.
    libusb_device** list = nullptr;
    const ssize_t count = api.get_device_list(pre_upload.get(), &list);
    CHECK(count == 3 && api.get_device_address(list[0]) == 5U);
    api.free_device_list(list, 1);
    asicen::LibusbDevice stale(api);
    CHECK(stale.open(pre_upload.get(), asicen::UsbLocation{1U, 9U}) == LIBUSB_ERROR_NO_DEVICE);

    std::vector<asicen::UsbFunctionObservation> observations;
    api.events.clear();
    CHECK(asicen::cli::observe_bus_fresh(api, 1U, &observations) == Error::OK);
    CHECK((api.events == std::vector<std::string>{"init2", "list2", "free2", "exit2"}));
    CHECK(api.contexts[1]->exited && !api.contexts[0]->exited);
    // Copied metadata stays valid after the scan context exited and poisoned
    // its devices; other positions are still not taken as the primary.
    const auto found = asicen::cli::find_runtime_at_position(observations, 1U, kPrimary);
    CHECK(found.error == Error::OK && observations[found.index].address == 9U &&
          found.profile == asicen::find_profile(asicen::ModelId::W3u3));
    CHECK(observations.size() == 3U && observations[2].port_path ==
          std::vector<std::uint8_t>({3U, 1U}));
    CHECK(!api.violation);
    return true;
}

bool reacquisition_uses_a_context_created_after_reenumeration()
{
    FakeApi api;
    api.physical = cold_pair();
    asicen::LibusbContext pre_upload(api);
    CHECK(pre_upload.initialize() == 0);
    reenumerate_primary(api);
    {
        asicen::LibusbContext runtime_context(api);
        asicen::LibusbDevice primary_runtime(api);
        api.events.clear();
        CHECK(asicen::cli::refresh_context(runtime_context, &primary_runtime) == 0);
        CHECK(primary_runtime.open(runtime_context.get(), asicen::UsbLocation{1U, 9U}) == 0);
        CHECK((api.events == std::vector<std::string>{"init2", "list2", "open2:9", "free2"}));
        // A second refresh closes the handle before its context exits.
        api.events.clear();
        CHECK(asicen::cli::refresh_context(runtime_context, &primary_runtime) == 0);
        CHECK((api.events == std::vector<std::string>{"close2", "exit2", "init3"}));
        CHECK(primary_runtime.open(runtime_context.get(), asicen::UsbLocation{1U, 9U}) == 0);
        api.events.clear();
    }
    // Declaration order unwinds the handle first, then its context.
    CHECK((api.events == std::vector<std::string>{"close3", "exit3"}));
    CHECK(!api.contexts[0]->exited);

    // The daemon's claim context is replaced the same way; no device remains on it.
    api.events.clear();
    CHECK(asicen::cli::refresh_context(pre_upload, nullptr) == 0);
    CHECK((api.events == std::vector<std::string>{"exit1", "init4"}));
    asicen::LibusbDevice claimed(api);
    CHECK(claimed.open(pre_upload.get(), asicen::UsbLocation{1U, 9U}) == 0);
    claimed.close();
    CHECK(!api.violation);
    return true;
}

bool sibling_attempts_each_get_a_new_context()
{
    FakeApi api;
    api.physical = cold_pair();
    reenumerate_primary(api);
    // After the GPIO restore the sibling first disappears, then re-enumerates.
    api.physical.erase(api.physical.begin() + 1);
    asicen::LibusbContext refreshed(api);
    asicen::LibusbDevice sibling(api);
    CHECK(asicen::cli::refresh_context(refreshed, &sibling) == 0);
    CHECK(sibling.open(refreshed.get(), asicen::UsbLocation{1U, 10U}) == LIBUSB_ERROR_NO_DEVICE);
    api.physical.push_back({0U, 1U, 10U, 0x1738U, 0x5211U, {2U, 2U}});
    CHECK(asicen::cli::refresh_context(refreshed, &sibling) == 0);
    CHECK(sibling.open(refreshed.get(), asicen::UsbLocation{1U, 10U}) == 0);
    CHECK(api.live_contexts() == 1U && api.contexts[0]->exited);
    sibling.close();
    CHECK(!api.violation);
    return true;
}

bool observation_failures_release_the_scan_context()
{
    FakeApi api;
    api.physical = cold_pair();
    std::vector<asicen::UsbFunctionObservation> observations(1U);
    api.init_error = LIBUSB_ERROR_NO_MEM;
    CHECK(asicen::cli::observe_bus_fresh(api, 1U, &observations) != Error::OK &&
          observations.empty() && api.contexts.empty());
    api.init_error = 0;
    api.list_error = LIBUSB_ERROR_IO;
    CHECK(asicen::cli::observe_bus_fresh(api, 1U, &observations) != Error::OK);
    CHECK(api.contexts.size() == 1U && api.contexts[0]->exited && api.live_contexts() == 0U);
    CHECK(asicen::cli::observe_bus_fresh(api, 1U, nullptr) == Error::INVALID_ARGUMENT);
    CHECK(!api.violation);
    return true;
}

}  // namespace

int main()
{
    if (!pre_upload_context_keeps_old_snapshot_fresh_scan_sees_runtime() ||
        !reacquisition_uses_a_context_created_after_reenumeration() ||
        !sibling_attempts_each_get_a_new_context() ||
        !observation_failures_release_the_scan_context()) {
        return 1;
    }
    std::puts("cold fresh USB checks passed (no hardware access)");
    return 0;
}
