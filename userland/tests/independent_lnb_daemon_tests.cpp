// SPDX-License-Identifier: GPL-2.0-or-later
// Device-free check of the production wiring for the independent LNB request:
// asicenctl -> Core control server -> authority/worker -> TunerService ->
// HardwareStreamService -> LibusbW3u3Hardware -> (fake) USB GPIO.
#include "asicen/card_only_service.h"
#include "asicen/libusb_hardware_backend.h"
#include "dtv/userland/control_client.h"
#include "dtv/userland/host_runtime/control_socket.h"
#include "host_ctl.h"
#include "test_temp_directory.h"

#include <array>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

namespace asicen {
struct LibusbW3u3HardwareTestPeer {
    using Hooks = LibusbW3u3Hardware::CaptureUsbHooks;
    static void configure(LibusbW3u3Hardware& hw, const Hooks* hooks)
    {
        hw.capture_usb_hooks_ = hooks;
        hw.claimed_ = hw.initialized_ = true;
    }
};
}  // namespace asicen

namespace {
using namespace asicen;
namespace w = dtv::userland::wire;
using dtv::userland::host_runtime::EndpointConfig;
using dtv::userland::host_runtime::connect_control;
using dtv::userland::Timeout;
using Hooks = LibusbW3u3HardwareTestPeer::Hooks;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::fprintf(stderr, "check failed %s:%d: %s\n", __FILE__, __LINE__, #condition);      \
            return false;                                                                          \
        }                                                                                          \
    } while (false)

constexpr const char* kInstance = "lnb-e2e";

struct FakeUsb {
    mutable std::mutex mutex;
    std::uint8_t gpio = 0xa5U;
    unsigned gpio_writes = 0U;
    bool fail_next_write = false;
    static int control(void* context, std::uint8_t, const ControlTransfer& transfer,
                       unsigned char* data)
    {
        auto& fake = *static_cast<FakeUsb*>(context);
        std::lock_guard<std::mutex> lock(fake.mutex);
        if (data == nullptr) return LIBUSB_ERROR_IO;
        if (transfer.request == Request::I2cRead) {
            data[0] = 1U;
            for (unsigned i = 1U; i < transfer.length; ++i) data[i] = 0U;
            if (transfer.value == 0x094aU) data[1] = 0x1eU;
            return transfer.length;
        }
        if (transfer.request == Request::Gpio || transfer.request == Request::GpioRead) {
            const auto mask = transfer.request == Request::GpioRead
                                  ? 0U
                                  : static_cast<unsigned>(transfer.value >> 8U);
            if (mask != 0U) {
                fake.gpio =
                    static_cast<std::uint8_t>((fake.gpio & ~mask) | (transfer.value & mask));
                ++fake.gpio_writes;
                if (fake.fail_next_write) {
                    fake.fail_next_write = false;
                    return LIBUSB_ERROR_IO;
                }
            }
            data[0] = fake.gpio;
            return transfer.length;
        }
        data[0] = 1U;
        return transfer.length;
    }
    Hooks hooks()
    {
        Hooks h{};
        h.context = this;
        h.control_function = control;
        return h;
    }
    bool on() const  // PX-W3U3: active-low GPIO20.
    {
        std::lock_guard<std::mutex> lock(mutex);
        return (gpio & 0x20U) == 0U;
    }
    unsigned writes() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return gpio_writes;
    }
    void fail_write()
    {
        std::lock_guard<std::mutex> lock(mutex);
        fail_next_write = true;
    }
};

// Runs the production daemon loop on the fake hardware until stopped.
class Daemon final {
public:
    Daemon(LibusbW3u3Hardware& hardware, const std::string& root)
        : endpoint_{root, "asicen-userland", kInstance}
    {
        thread_ = std::thread([this, &hardware, root] {
            result_ = run_live_card_stream_server(hardware, root.c_str(), kInstance, false, &stop_,
                                                  nullptr);
        });
    }
    ~Daemon() { (void)stop(); }
    bool ready()
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < until) {
            auto connected = connect_control(endpoint_, Timeout{200U});
            if (connected) {
                const auto status = w::fetch_status(*connected.value(), Timeout{2000U});
                if (status) return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return false;
    }
    bool status(w::StatusFetchResult& out)
    {
        auto connected = connect_control(endpoint_, Timeout{2000U});
        if (!connected) return false;
        out = w::fetch_status(*connected.value(), Timeout{5000U});
        return static_cast<bool>(out);
    }
    bool stop()
    {
        stop_ = 1;
        if (thread_.joinable()) thread_.join();
        return result_ == 0;
    }
    int ctl(const std::string& root, const char* command) const
    {
        const dtv::userland::host_ctl::Profile profile{
            "asicenctl", "asicen-userland", "test", dtv::userland::identity::Product::asicen,
            true, false, nullptr, nullptr, true, false};
        const std::array<const char*, 6> argv{"asicenctl", "--runtime-dir", root.c_str(),
                                               "--instance", kInstance, command};
        return dtv::userland::host_ctl::run_control_cli(static_cast<int>(argv.size()),
                                                        argv.data(), profile);
    }

private:
    EndpointConfig endpoint_;
    volatile std::sig_atomic_t stop_ = 0;
    int result_ = -1;
    std::thread thread_;
};

bool lnb_state(Daemon& daemon, bool known, bool value = false)
{
    w::StatusFetchResult status;
    CHECK(daemon.status(status));
    const auto& lnb = status.snapshot.status.lnb_15v_enabled;
    return lnb.present == known && (!known || lnb.value == value);
}

bool advertises_lnb(Daemon& daemon)
{
    w::StatusFetchResult status;
    CHECK(daemon.status(status));
    const auto& receivers = status.snapshot.list.receivers;
    CHECK(receivers.present);
    bool satellite_with_lnb = false;
    for (std::size_t i = 0U; i < receivers.value.count; ++i) {
        const auto& item = receivers.value.items[i];
        const bool lnb = (item.capabilities.controls & w::kControlBitLnb) != 0U;
        CHECK(lnb == ((item.systems.mask & 0x02U) != 0U));
        if (lnb) {
            CHECK(item.capabilities.lnb.values.count == 2U &&
                  item.capabilities.lnb.values.items[0U] == 0U &&
                  item.capabilities.lnb.values.items[1U] == 15U);
            satellite_with_lnb = true;
        }
    }
    return satellite_with_lnb;
}

struct TempRoot {
    std::string path;
    bool ok = false;
    TempRoot()
    {
        std::string pattern = px4::userland::test::temporary_directory_template("asicen-lnb-e2e-");
        char* made = ::mkdtemp(pattern.data());
        if (made == nullptr) return;
        path = made;
        ok = ::chmod(path.c_str(), 0700) == 0;
    }
    ~TempRoot()
    {
        std::error_code ignored;
        if (!path.empty()) std::filesystem::remove_all(path, ignored);
    }
};

bool test_ctl_lnb_on_off_reaches_the_gpio()
{
    TempRoot root;
    CHECK(root.ok);
    FakeUsb usb;
    Hooks hooks = usb.hooks();
    LibusbW3u3Hardware hardware(nullptr, {}, {}, {}, {}, find_profile(ModelId::W3u3));
    LibusbW3u3HardwareTestPeer::configure(hardware, &hooks);
    {
        Daemon daemon(hardware, root.path);
        CHECK(daemon.ready());
        CHECK(advertises_lnb(daemon));
        CHECK(lnb_state(daemon, false));  // Nothing verified yet: null, never a guessed false.
        CHECK(daemon.ctl(root.path, "lnb-on") == 0);
        CHECK(usb.on());
        CHECK(lnb_state(daemon, true, true));
        CHECK(daemon.ctl(root.path, "lnb-off") == 0);
        CHECK(!usb.on());
        CHECK(lnb_state(daemon, true, false));
        CHECK(daemon.ctl(root.path, "lnb-on") == 0);
        CHECK(usb.on());
        usb.fail_write();
        CHECK(daemon.ctl(root.path, "lnb-off") == 7);
        CHECK(lnb_state(daemon, false));  // The write may have applied, but was not verified.
        CHECK(daemon.ctl(root.path, "lnb-off") == 0);
        CHECK(lnb_state(daemon, true, false));
        CHECK(daemon.ctl(root.path, "lnb-on") == 0);
        CHECK(daemon.stop());
    }
    // The server loop leaves the feed to the daemon's hardware shutdown.
    CHECK(usb.on());
    CHECK(hardware.shutdown());
    CHECK(!usb.on());
    return true;
}

bool test_ctl_lnb_on_is_refused_without_allow_lnb_power()
{
    TempRoot root;
    CHECK(root.ok);
    FakeUsb usb;
    Hooks hooks = usb.hooks();
    LibusbW3u3Hardware hardware(nullptr, {}, {}, {}, {}, find_profile(ModelId::W3u3));
    hardware.set_allow_lnb_power(false);
    LibusbW3u3HardwareTestPeer::configure(hardware, &hooks);
    Daemon daemon(hardware, root.path);
    CHECK(daemon.ready());
    CHECK(daemon.ctl(root.path, "lnb-on") == 3);  // UNSUPPORTED
    CHECK(usb.writes() == 0U && !usb.on());
    CHECK(lnb_state(daemon, false));
    CHECK(daemon.ctl(root.path, "lnb-off") == 0);  // OFF stays available and is verified.
    CHECK(lnb_state(daemon, true, false));
    CHECK(daemon.stop());
    return true;
}
}  // namespace

int main()
{
    if (!test_ctl_lnb_on_off_reaches_the_gpio() ||
        !test_ctl_lnb_on_is_refused_without_allow_lnb_power()) {
        return 1;
    }
    return 0;
}
