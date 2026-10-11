// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/libusb_hardware_backend.h"
#include "asicen/write_protocol.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

namespace asicen {
struct LibusbW3u3HardwareTestPeer {
    using Hooks = LibusbW3u3Hardware::CaptureUsbHooks;
    static void configure(LibusbW3u3Hardware& hw, const Hooks* hooks)
    {
        hw.capture_usb_hooks_ = hooks;
        hw.claimed_ = hw.initialized_ = true;
        hw.v2_roles_verified_ = hw.v2_identity_verified_ = true;
        (void)hw.active_receiver_.reserve(0U);
    }
    static void snapshot(LibusbW3u3Hardware& hw, std::uint8_t gpio)
    {
        hw.gpio_snapshot_ = gpio;
        hw.gpio_snapshot_valid_ = true;
    }
    static void failed_capture(LibusbW3u3Hardware& hw)
    {
        hw.source_prepared_ = true;
        hw.mark_cleanup_failed(px4::userland::Error::USB_IO);
    }
    static void clear_capture_fixture(LibusbW3u3Hardware& hw) { hw.source_prepared_ = false; }
    static bool quarantined(const LibusbW3u3Hardware& hw) { return hw.cleanup_failed_.load(); }
    static int generic_gpio(LibusbW3u3Hardware& hw, std::uint8_t value, std::uint8_t mask)
    {
        std::uint8_t response = 0U;
        return hw.control(make_gpio_set(value, mask), &response);
    }
    static void stop_monitor(LibusbW3u3Hardware& hw) { hw.stop_lnb_monitor(); }
    static void hold_gate(LibusbW3u3Hardware& hw) { hw.control_gate_.lock(); }
    static void release_gate(LibusbW3u3Hardware& hw) { hw.control_gate_.unlock(); }
    static void gated_delay(LibusbW3u3Hardware& hw, unsigned ms)
    {
        std::lock_guard<std::recursive_timed_mutex> gate(hw.control_gate_);
        hw.delay_ms(ms);
    }
};
}  // namespace asicen

namespace {
using namespace asicen;
using px4::userland::Error;
using px4::userland::ipc::System;
using Hooks = LibusbW3u3HardwareTestPeer::Hooks;
struct FakeUsb {
    struct Event {
        Request request;
        std::uint16_t value;
        std::uint8_t function;
    };
    mutable std::mutex mutex;
    std::vector<Event> events;
    std::uint8_t gpio = 0xa5U;
    std::uint8_t controller_type = 0x1eU;
    std::uint8_t controller_output = 0U;
    unsigned fail_writes = 0U;
    unsigned short_writes = 0U;
    unsigned fail_reads = 0U;
    unsigned mismatch_reads = 0U;
    bool disconnect_on_write = false;
    bool cancel_on_write = false;
    LibusbW3u3Hardware* hardware = nullptr;
    static int control(void* context, std::uint8_t function, const ControlTransfer& transfer,
                       unsigned char* data)
    {
        auto& fake = *static_cast<FakeUsb*>(context);
        std::lock_guard<std::mutex> lock(fake.mutex);
        fake.events.push_back({transfer.request, transfer.value, function});
        if (data == nullptr) {
            return LIBUSB_ERROR_IO;
        }
        if (transfer.request == Request::I2cRead) {
            data[0] = 1U;
            for (unsigned i = 1U; i < transfer.length; ++i) {
                data[i] = 0U;
            }
            if (transfer.value == 0x094aU) {
                data[1] = fake.controller_type;
            }
            if (transfer.value == 0x054aU) {
                data[1] = fake.controller_output;
            }
            return transfer.length;
        }
        if (transfer.request == Request::Gpio || transfer.request == Request::GpioRead) {
            const auto mask = transfer.request == Request::GpioRead
                                  ? 0U
                                  : static_cast<unsigned>(transfer.value >> 8U);
            if (mask != 0U) {
                fake.gpio =
                    static_cast<std::uint8_t>((fake.gpio & ~mask) | (transfer.value & mask));
                if (fake.disconnect_on_write) {
                    fake.disconnect_on_write = false;
                    return LIBUSB_ERROR_NO_DEVICE;
                }
                if (fake.cancel_on_write) {
                    fake.cancel_on_write = false;
                    fake.hardware->request_stop();
                }
                if (fake.fail_writes != 0U) {
                    --fake.fail_writes;
                    return LIBUSB_ERROR_IO;
                }
                if (fake.short_writes != 0U) {
                    --fake.short_writes;
                    return 0;
                }
            } else {
                if (fake.fail_reads != 0U) {
                    --fake.fail_reads;
                    return LIBUSB_ERROR_IO;
                }
                if (fake.mismatch_reads != 0U) {
                    --fake.mismatch_reads;
                    data[0] = static_cast<std::uint8_t>(fake.gpio ^ 0x20U);
                    return transfer.length;
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
    bool on(ModelId model) const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return model == ModelId::W3u3V2 ? (gpio & 0x20U) != 0U : (gpio & 0x20U) == 0U;
    }
    void feedback_low()
    {
        std::lock_guard<std::mutex> lock(mutex);
        gpio &= 0x7fU;
    }
    std::size_t count() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return events.size();
    }
    bool only_primary() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& event : events) {
            if (event.function != 0U) {
                return false;
            }
        }
        return true;
    }
};
struct Fixture {
    // Hooks must outlive hardware, including its monitor and final cleanup.
    FakeUsb usb;
    Hooks hooks;
    LibusbW3u3Hardware hw;
    explicit Fixture(ModelId model = ModelId::W3u3)
        : hooks(usb.hooks()), hw(nullptr, {}, {}, {}, {}, find_profile(model))
    {
        usb.hardware = &hw;
        if (model == ModelId::W3u3V2) {
            usb.gpio &= 0xdfU;
        }
        LibusbW3u3HardwareTestPeer::configure(hw, &hooks);
    }
    ~Fixture() { (void)hw.shutdown(); }
};
bool check(bool ok, const char* expression, int line)
{
    if (!ok) {
        std::fprintf(stderr, "line %d: CHECK failed: %s\n", line, expression);
    }
    return ok;
}
#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!check(static_cast<bool>(expr), #expr, __LINE__))                                      \
            return false;                                                                          \
    } while (false)

bool test_lnb_monitor_launch_failure_restores_power()
{
    Fixture fixture(ModelId::W3u3V2);
    fixture.hooks.launch_thread = [](pthread_t*, const pthread_attr_t*, NativeThread::Entry,
                                     void*) { return 11; };
    CHECK(fixture.hw.begin_tune_power(0U, System::ISDB_S, 15U).error() == Error::INTERNAL);
    CHECK(!fixture.usb.on(ModelId::W3u3V2));
    return true;
}

bool test_model_polarity_and_transactions()
{
    for (const auto model : {ModelId::W3u2, ModelId::W3u3, ModelId::W3u3V2}) {
        Fixture f(model);
        CHECK(f.hw.begin_tune_power(0U, System::ISDB_S, 0U));
        CHECK(f.hw.commit_tune_power(0U));
        CHECK(!f.usb.on(model));
        CHECK(f.hw.begin_tune_power(0U, System::ISDB_S, 15U));
        CHECK(f.usb.on(model));
        CHECK(f.hw.commit_tune_power(0U));
        CHECK(f.usb.on(model));
        CHECK(f.hw.stop());  // Ordinary stream stop retains the lease's feed.
        CHECK(f.usb.on(model));
        // A pending zero-voltage request is applied before acquisition.
        CHECK(f.hw.begin_tune_power(0U, System::ISDB_S, 0U));
        CHECK(!f.usb.on(model));
        CHECK(f.hw.rollback_tune_power(0U));
        CHECK(f.usb.on(model));
        CHECK(f.hw.begin_tune_power(0U, System::ISDB_S, 0U));
        CHECK(!f.usb.on(model));
        CHECK(f.hw.commit_tune_power(0U));
        CHECK(!f.usb.on(model));
        CHECK(f.hw.begin_tune_power(0U, System::ISDB_S, 15U));
        CHECK(f.hw.rollback_tune_power(0U));
        CHECK(!f.usb.on(model));
        CHECK(f.usb.only_primary());
    }
    return true;
}

bool test_unsupported_and_invalid_requests_do_no_io()
{
    for (const auto model : {ModelId::S3u, ModelId::S3u2}) {
        Fixture f(model);
        CHECK(f.hw.begin_tune_power(0U, System::ISDB_S, 15U).error() == Error::UNSUPPORTED);
        CHECK(f.usb.count() == 0U);
        CHECK(f.hw.begin_tune_power(0U, System::ISDB_S, 0U));
        CHECK(f.hw.commit_tune_power(0U));
        CHECK(f.hw.close_receiver(0U));
        CHECK(f.usb.count() == 0U);
    }
    Fixture f;
    CHECK(!f.hw.begin_tune_power(0U, System::ISDB_S, 18U));
    CHECK(!f.hw.begin_tune_power(0U, System::ISDB_T, 15U));
    CHECK(!f.hw.begin_tune_power(1U, System::ISDB_S, 15U));
    CHECK(f.usb.count() == 0U);
    return true;
}

bool test_controller_guard_and_generic_mask()
{
    for (const auto model : {ModelId::W3u2, ModelId::W3u3, ModelId::W3u3V2}) {
        Fixture f(model);
        f.usb.controller_output = 0xa0U;
        CHECK(f.hw.begin_tune_power(0U, System::ISDB_S, 15U).error() == Error::BUSY);
        CHECK(!f.usb.on(model));
        f.usb.controller_output = 0U;
        f.usb.controller_type = 0U;
        CHECK(f.hw.begin_tune_power(0U, System::ISDB_S, 15U).error() == Error::UNSUPPORTED);
        const auto before = f.usb.count();
        CHECK(LibusbW3u3HardwareTestPeer::generic_gpio(f.hw, model == ModelId::W3u3V2 ? 0x20U : 0U,
                                                       0x20U) == 1);
        CHECK(f.usb.count() == before);
        CHECK(!f.usb.on(model));
        CHECK(LibusbW3u3HardwareTestPeer::generic_gpio(f.hw, 0xa7U, 0xfbU) == 1);
        CHECK(!f.usb.on(model));
    }
    return true;
}

bool test_begin_failures_restore_off_and_quarantine_uncertainty()
{
    for (unsigned failure = 0U; failure < 4U; ++failure) {
        Fixture f;
        if (failure == 0U) {
            f.usb.fail_writes = 1U;
        }
        if (failure == 1U) {
            f.usb.short_writes = 1U;
        }
        if (failure == 2U) {
            f.usb.fail_reads = 1U;
        }
        if (failure == 3U) {
            f.usb.mismatch_reads = 1U;
        }
        CHECK(!f.hw.begin_tune_power(0U, System::ISDB_S, 15U));
        CHECK(!f.usb.on(ModelId::W3u3));
        CHECK(!LibusbW3u3HardwareTestPeer::quarantined(f.hw));
    }
    Fixture f;
    f.usb.fail_writes = 2U;
    CHECK(!f.hw.begin_tune_power(0U, System::ISDB_S, 15U));
    CHECK(LibusbW3u3HardwareTestPeer::quarantined(f.hw));
    const auto before = f.usb.count();
    CHECK(!f.hw.begin_tune_power(0U, System::ISDB_S, 15U));
    CHECK(f.usb.count() == before);
    CHECK(!f.hw.shutdown());  // Preserve the first cleanup error after retrying OFF.
    CHECK(!f.usb.on(ModelId::W3u3));
    return true;
}

bool test_close_shutdown_release_and_snapshot_are_off()
{
    for (const auto model : {ModelId::W3u2, ModelId::W3u3, ModelId::W3u3V2}) {
        Fixture f(model);
        CHECK(f.hw.begin_tune_power(0U, System::ISDB_S, 15U));
        CHECK(f.hw.commit_tune_power(0U));
        CHECK(f.hw.close_receiver(0U));
        CHECK(!f.usb.on(model));
    }
    {
        Fixture f;
        LibusbW3u3HardwareTestPeer::snapshot(f.hw, 0x85U);  // Historical ON must not return.
        CHECK(f.hw.begin_tune_power(0U, System::ISDB_S, 15U));
        CHECK(f.hw.commit_tune_power(0U));
        CHECK(f.hw.shutdown());
        CHECK(!f.usb.on(ModelId::W3u3));
        CHECK((f.usb.gpio & 0xdfU) == 0x85U);
    }
    {
        Fixture f;
        CHECK(f.hw.begin_tune_power(0U, System::ISDB_S, 15U));
        CHECK(f.hw.commit_tune_power(0U));
        CHECK(f.hw.release());
        CHECK(!f.usb.on(ModelId::W3u3));
    }
    {
        Fixture f;
        CHECK(f.hw.begin_tune_power(0U, System::ISDB_S, 15U));
        CHECK(f.hw.commit_tune_power(0U));
        LibusbW3u3HardwareTestPeer::failed_capture(f.hw);
        CHECK(!f.hw.close_receiver(0U));
        CHECK(!f.usb.on(ModelId::W3u3));
        LibusbW3u3HardwareTestPeer::clear_capture_fixture(f.hw);
    }
    return true;
}

bool test_cancel_and_disconnect_do_not_leave_new_power_owned()
{
    {
        Fixture f;
        f.usb.cancel_on_write = true;
        CHECK(f.hw.begin_tune_power(0U, System::ISDB_S, 15U).error() == Error::NOT_READY);
        CHECK(!f.usb.on(ModelId::W3u3));
    }
    {
        Fixture f;
        f.usb.disconnect_on_write = true;
        CHECK(f.hw.begin_tune_power(0U, System::ISDB_S, 15U).error() == Error::DISCONNECTED);
        const auto before = f.usb.count();
        CHECK(f.hw.rollback_tune_power(0U));
        CHECK(f.hw.shutdown());
        CHECK(f.usb.count() == before);
    }
    return true;
}

bool test_v2_feedback_cutoff_is_terminal()
{
    {
        Fixture f(ModelId::W3u3V2);
        f.usb.feedback_low();
        CHECK(f.hw.begin_tune_power(0U, System::ISDB_S, 15U).error() == Error::PROTOCOL_ERROR);
        CHECK(!f.usb.on(ModelId::W3u3V2));
        CHECK(LibusbW3u3HardwareTestPeer::quarantined(f.hw));
    }
    {
        Fixture f(ModelId::W3u3V2);
        CHECK(f.hw.begin_tune_power(0U, System::ISDB_S, 15U));
        CHECK(f.hw.commit_tune_power(0U));
        CHECK(f.hw.begin_tune_power(0U, System::ISDB_S, 15U));
        f.usb.feedback_low();
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!LibusbW3u3HardwareTestPeer::quarantined(f.hw) &&
               std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(LibusbW3u3HardwareTestPeer::quarantined(f.hw));
        CHECK(!f.usb.on(ModelId::W3u3V2));
        CHECK(!f.hw.commit_tune_power(0U));
        CHECK(f.hw.rollback_tune_power(0U));
        CHECK(!f.usb.on(ModelId::W3u3V2));
    }
    return true;
}

bool test_v2_restoration_failure_forces_off()
{
    for (const bool fail_commit : {false, true}) {
        Fixture f(ModelId::W3u3V2);
        CHECK(f.hw.begin_tune_power(0U, System::ISDB_S, 15U));
        CHECK(f.hw.commit_tune_power(0U));
        LibusbW3u3HardwareTestPeer::hold_gate(f.hw);
        if (fail_commit) {
            CHECK(f.hw.begin_tune_power(0U, System::ISDB_S, 0U));
        }
        f.usb.fail_writes = 1U;
        f.usb.feedback_low();
        if (fail_commit) {
            // OFF was already applied at begin_tune; restoration belongs to
            // rollback, so exercise its failed ON write directly.
            CHECK(!f.hw.rollback_tune_power(0U));
        } else {
            CHECK(!f.hw.begin_tune_power(0U, System::ISDB_S, 15U));
        }
        CHECK(!f.usb.on(ModelId::W3u3V2));
        CHECK(LibusbW3u3HardwareTestPeer::quarantined(f.hw));
        LibusbW3u3HardwareTestPeer::release_gate(f.hw);
    }
    return true;
}

bool test_v2_cooperative_feedback_during_gate_owned_delay()
{
    Fixture f(ModelId::W3u3V2);
    CHECK(f.hw.begin_tune_power(0U, System::ISDB_S, 15U));
    CHECK(f.hw.commit_tune_power(0U));
    f.usb.feedback_low();
    const auto start = std::chrono::steady_clock::now();
    LibusbW3u3HardwareTestPeer::gated_delay(f.hw, 1500U);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(elapsed < std::chrono::milliseconds(500));
    CHECK(LibusbW3u3HardwareTestPeer::quarantined(f.hw));
    CHECK(!f.usb.on(ModelId::W3u3V2));
    return true;
}

bool test_gate_waiter_rechecks_cancellation()
{
    Fixture f;
    LibusbW3u3HardwareTestPeer::hold_gate(f.hw);
    std::atomic<bool> entered{false};
    Error result = Error::OK;
    std::thread waiter([&] {
        entered.store(true);
        result = f.hw.begin_tune_power(0U, System::ISDB_S, 15U).error();
    });
    while (!entered.load()) {
        std::this_thread::yield();
    }
    f.hw.request_stop();
    LibusbW3u3HardwareTestPeer::release_gate(f.hw);
    waiter.join();
    CHECK(result == Error::NOT_READY);
    CHECK(f.usb.count() == 0U);
    return true;
}
}  // namespace
int main()
{
    if (!test_lnb_monitor_launch_failure_restores_power() ||
        !test_model_polarity_and_transactions() ||
        !test_unsupported_and_invalid_requests_do_no_io() ||
        !test_controller_guard_and_generic_mask() ||
        !test_begin_failures_restore_off_and_quarantine_uncertainty() ||
        !test_close_shutdown_release_and_snapshot_are_off() ||
        !test_cancel_and_disconnect_do_not_leave_new_power_owned() ||
        !test_v2_feedback_cutoff_is_terminal() || !test_v2_restoration_failure_forces_off() ||
        !test_v2_cooperative_feedback_during_gate_owned_delay() ||
        !test_gate_waiter_rechecks_cancellation()) {
        return 1;
    }
    return 0;
}
