// SPDX-License-Identifier: GPL-2.0-only
// Offline API-boundary regression. No real USB context or libusb scheduler runs.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#define main baseline_lifecycle_main
#include "libusb_capture_lifecycle_tests.cpp"
#undef main

namespace {
enum class Case { ready, cancelled, invalid, status_error, unplug, out_of_order,
                  spurious, handler_departure, handling_not_ok, api_error, drain,
                  api_no_device, api_interrupted, control_disconnect, control_disconnect_ready };
const char* case_name(Case test) {
    switch (test) {
    case Case::ready: return "expected-ready";
    case Case::cancelled: return "cancelled";
    case Case::invalid: return "invalid-length";
    case Case::status_error: return "status-error";
    case Case::unplug: return "no-device-callback";
    case Case::out_of_order: return "nonexpected-ready";
    case Case::spurious: return "spurious-no-target";
    case Case::handler_departure: return "handler-departure";
    case Case::handling_not_ok: return "handling-not-ok";
    case Case::api_error: return "event-api-error";
    case Case::drain: return "cancel-deferred-drain";
    case Case::api_no_device: return "event-api-no-device";
    case Case::api_interrupted: return "event-api-interrupted";
    case Case::control_disconnect: return "control-disconnected-no-target";
    case Case::control_disconnect_ready: return "control-disconnected-ready";
    }
    return "unknown";
}
struct Boundary {
    FakeUsb* fake;
    std::atomic<bool>* cancelled;
    Case test;
    bool contender;
    std::mutex mutex;
    std::condition_variable changed;
    std::recursive_mutex events;
    std::mutex waiters;
    bool entered = false, released = false;
    int polls = 0, waits = 0, tries = 0, waiter_locks = 0;
    int event_locks = 0, waiter_depth = 0, drain_polls = 0;
    std::vector<libusb_transfer*> cancelling;
    Boundary(FakeUsb* f, std::atomic<bool>* c, Case t, bool contender_)
        : fake(f), cancelled(c), test(t), contender(contender_) {}
    void boundary() {
        std::unique_lock<std::mutex> lock(mutex);
        if (entered) return;
        entered = true;
        changed.notify_all();
        assert(changed.wait_for(lock, std::chrono::seconds(3), [&] { return released; }));
    }
    void complete(unsigned index, libusb_transfer_status status, int length) {
        // libusb io.c dispatches with events -> event_waiters -> AsyncState.
        std::lock_guard<std::recursive_mutex> event_lock(events);
        std::lock_guard<std::mutex> waiter_lock(waiters);
        auto* transfer = fake->submitted[index];
        transfer->status = status;
        transfer->actual_length = length;
        transfer->callback(transfer);
        fake->callback_total.fetch_add(1U);
    }
    int poll() {
        ++polls;
        assert(polls < 10);
        if (test == Case::out_of_order) complete(0, LIBUSB_TRANSFER_COMPLETED, 0);
        if (test == Case::spurious && polls == 2) complete(0, LIBUSB_TRANSFER_ERROR, 0);
        if (test == Case::handler_departure || test == Case::handling_not_ok)
            complete(0, LIBUSB_TRANSFER_ERROR, 0);
        if (test == Case::api_error) return LIBUSB_ERROR_IO;
        if (test == Case::api_no_device) return LIBUSB_ERROR_NO_DEVICE;
        if (test == Case::control_disconnect) return LIBUSB_ERROR_NO_DEVICE;
        if (test == Case::api_interrupted) return LIBUSB_ERROR_INTERRUPTED;
        return 0;
    }
};
Boundary* active = nullptr;
void no_interrupt(void*) {}
int delayed_cancel(void* context, libusb_transfer* transfer) {
    ++static_cast<FakeUsb*>(context)->cancel_count;
    active->cancelling.push_back(transfer);
    return 0;
}
}

extern "C" {
ssize_t __wrap_libusb_get_device_list(libusb_context*, libusb_device***) {
    return LIBUSB_ERROR_NO_DEVICE; // Constructor must never touch a real context.
}
int __wrap_libusb_try_lock_events(libusb_context*) {
    auto& b = *active;
    ++b.tries;
    if (!b.contender) b.boundary();
    if (b.contender && !(b.test == Case::handler_departure && b.tries > 1)) return 1;
    b.events.lock();
    ++b.event_locks;
    assert(b.event_locks == 1 && b.waiter_depth == 0);
    return 0;
}
void __wrap_libusb_unlock_events(libusb_context*) {
    assert(active->event_locks-- == 1);
    active->events.unlock();
}
int __wrap_libusb_event_handling_ok(libusb_context*) {
    return !(active->test == Case::handling_not_ok && active->tries == 1);
}
void __wrap_libusb_lock_event_waiters(libusb_context*) {
    auto& b = *active;
    ++b.waiter_locks;
    b.boundary();
    b.waiters.lock();
    assert(++b.waiter_depth == 1 && b.event_locks == 0);
}
void __wrap_libusb_unlock_event_waiters(libusb_context*) {
    assert(active->waiter_depth-- == 1);
    active->waiters.unlock();
}
int __wrap_libusb_event_handler_active(libusb_context*) {
    return active->test != Case::handler_departure;
}
int __wrap_libusb_wait_for_event(libusb_context*, timeval* timeout) {
    assert(active->waiter_depth == 1);
    assert(timeout->tv_sec == 0 && timeout->tv_usec <= 100000);
    ++active->waits;
    // Model condition wait releasing the waiter lock while another handler runs.
    active->waiters.unlock();
    const int result = active->poll();
    active->waiters.lock();
    return result;
}
int __wrap_libusb_handle_events_locked(libusb_context*, timeval* timeout) {
    assert(active->event_locks == 1);
    assert(timeout->tv_sec == 0 && timeout->tv_usec <= 100000);
    return active->poll();
}
int __wrap_libusb_handle_events_timeout_completed(libusb_context*, timeval*, int*) {
    auto& b = *active;
    if (!b.cancelling.empty()) {
        ++b.drain_polls;
        std::lock_guard<std::recursive_mutex> event_lock(b.events);
        std::lock_guard<std::mutex> waiter_lock(b.waiters);
        for (auto* transfer : b.cancelling) {
            transfer->status = LIBUSB_TRANSFER_CANCELLED;
            transfer->actual_length = 0;
            transfer->callback(transfer);
            b.fake->callback_total.fetch_add(1U);
        }
        b.cancelling.clear();
        return 0;
    }
    assert(false && "Unexpected legacy event-pump entry");
    return LIBUSB_ERROR_OTHER;
}
}

static bool run_case(Case test, bool contender) {
    FakeUsb fake;
    Hooks hooks{};
    // The opaque pointer is never dereferenced: all event APIs are wrapped.
    LibusbW3u3Hardware hardware(reinterpret_cast<libusb_context*>(0x1), {}, {}, {}, {});
    configure(hardware, fake, hooks, 1U);
    std::atomic<bool> cancelled{false};
    CHECK(hardware.prepare(1U, System::ISDB_T, cancelled).has_value());
    Boundary b{&fake, &cancelled, test, contender};
    active = &b;
    hooks.pump_events = nullptr;
    hooks.interrupt_events = no_interrupt;
    if (test == Case::drain) hooks.cancel = delayed_cancel;
    fake.fail_submit_number =
        (test == Case::ready || test == Case::control_disconnect_ready) ? 5 : 0;
    std::thread callback([&] {
        std::unique_lock<std::mutex> lock(b.mutex);
        assert(b.changed.wait_for(lock, std::chrono::seconds(3), [&] { return b.entered; }));
        // Happens after run's empty-ready scan, before event API proceeds.
        if (test == Case::cancelled || test == Case::drain) cancelled.store(true);
        else if (test == Case::ready) b.complete(0, LIBUSB_TRANSFER_COMPLETED, 0);
        else if (test == Case::invalid) b.complete(0, LIBUSB_TRANSFER_COMPLETED, -1);
        else if (test == Case::status_error) b.complete(0, LIBUSB_TRANSFER_ERROR, 0);
        else if (test == Case::unplug) b.complete(0, LIBUSB_TRANSFER_NO_DEVICE, 0);
        else if (test == Case::out_of_order) b.complete(1, LIBUSB_TRANSFER_ERROR, 0);
        else if (test == Case::control_disconnect || test == Case::control_disconnect_ready) {
            assert(LibusbW3u3HardwareTestPeer::begin_card_operation(hardware));
            fake.fail_control_once = LIBUSB_ERROR_NO_DEVICE;
            std::array<unsigned char, 2> response{};
            const ControlTransfer read{1U, Request::I2cRead, 0x004aU, 0U, 2U, Direction::In, 500U};
            assert(LibusbW3u3HardwareTestPeer::card_control(hardware, read, response.data()) ==
                   LIBUSB_ERROR_NO_DEVICE);
            LibusbW3u3HardwareTestPeer::end_card_operation(hardware);
            if (test == Case::control_disconnect_ready) b.complete(0, LIBUSB_TRANSFER_COMPLETED, 0);
        }
        b.released = true;
        b.changed.notify_all();
    });
    auto result = hardware.run(cancelled, discard_packets, nullptr);
    callback.join();
    CHECK(result == ((test == Case::cancelled || test == Case::drain) ? CaptureRunResult::cancelled :
                     (test == Case::unplug || test == Case::api_no_device ||
                      test == Case::control_disconnect || test == Case::control_disconnect_ready)
                         ? CaptureRunResult::disconnected : CaptureRunResult::usb_error));
    const bool immediate = test == Case::ready || test == Case::cancelled || test == Case::invalid ||
                           test == Case::status_error || test == Case::unplug || test == Case::drain ||
                           test == Case::control_disconnect_ready;
    const int expected_polls = immediate ? 0 : test == Case::spurious ? 2 : 1;
    CHECK(b.polls == expected_polls);
    {
        CHECK(b.tries > 0);
        CHECK((b.waiter_locks > 0) == contender);
        if (test == Case::handler_departure || test == Case::handling_not_ok)
            CHECK(b.tries == 2);
        if (contender && !immediate && test != Case::handler_departure)
            CHECK(b.waits == expected_polls);
    }
    CHECK(b.event_locks == 0 && b.waiter_depth == 0);
    CHECK(fake.free_count == 4);
    CHECK(fake.submit_count == ((test == Case::ready || test == Case::out_of_order ||
                                test == Case::control_disconnect_ready) ? 5 : 4));
    if (test == Case::control_disconnect_ready) CHECK(hardware.source_counters().empty_intervals == 1);
    if (test == Case::drain) {
        CHECK(b.drain_polls == 1);
        CHECK(fake.callback_total.load() == 4);
    }
    std::printf("PASS case=%s branch=%s capture_polls=%d drain_polls=%d tries=%d waits=%d\n",
                case_name(test), contender ? "waiter" : "event-owner", b.polls,
                b.drain_polls, b.tries, b.waits);
    // Destructor keeps fake USB hooks installed, and no pending transfers remain.
    return true;
}

int main() {
    for (bool contender : {false, true}) {
        for (Case test : {Case::ready, Case::cancelled, Case::invalid, Case::status_error,
                          Case::unplug, Case::out_of_order, Case::spurious, Case::api_error, Case::drain,
                          Case::api_no_device, Case::api_interrupted, Case::control_disconnect,
                          Case::control_disconnect_ready})
            if (!run_case(test, contender)) return 1;
    }
    if (!run_case(Case::handler_departure, true) ||
        !run_case(Case::handling_not_ok, false)) return 1;
    std::printf("28 API-boundary scenarios passed; no hardware/scheduler claims.\n");
}
