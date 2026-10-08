// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/libusb_hardware_backend.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace asicen {

struct LibusbW3u3HardwareTestPeer {
    using Hooks = LibusbW3u3Hardware::CaptureUsbHooks;

    static void configure(LibusbW3u3Hardware& hardware, const Hooks* hooks,
                          std::uint8_t receiver) {
        hardware.capture_usb_hooks_ = hooks;
        hardware.claimed_ = true;
        hardware.initialized_ = true;
        hardware.tuned_ = true;
        hardware.tuned_receiver_ = receiver;
        hardware.tuned_system_ = receiver == 0U
                                     ? px4::userland::ipc::System::ISDB_S
                                     : px4::userland::ipc::System::ISDB_T;
        (void)hardware.active_receiver_.reserve(receiver);
    }

    static void mark_tuned(LibusbW3u3Hardware& hardware, std::uint8_t receiver) {
        hardware.tuned_ = true;
        hardware.tuned_receiver_ = receiver;
        hardware.tuned_system_ = receiver == 0U
                                     ? px4::userland::ipc::System::ISDB_S
                                     : px4::userland::ipc::System::ISDB_T;
    }

    static void set_cleanup_failed(LibusbW3u3Hardware& hardware) {
        hardware.cleanup_failed_ = true;
    }
};

}  // namespace asicen

namespace {
using namespace asicen;
using px4::userland::Error;
using px4::userland::ipc::System;
using Hooks = LibusbW3u3HardwareTestPeer::Hooks;

struct ControlEvent {
    Request request{};
    std::uint16_t value = 0;
    std::uint16_t index = 0;
};

struct FakeUsb {
    LibusbW3u3Hardware* hardware = nullptr;
    std::array<std::uint8_t, 256> controller{};
    std::array<std::array<std::uint8_t, 0x45>, 2> cf{};
    std::vector<ControlEvent> controls;
    std::vector<std::uint8_t> endpoints;
    int submit_count = 0;
    int fail_submit_number = 0;
    int cancel_count = 0;
    int free_count = 0;
    int dsc_start_count = 0;
    int dsc_stop_count = 0;
    int seed_write_count = 0;
    int seed_clear_count = 0;
    int fail_seed_once = 0;
    bool fail_dsc_start = false;
    bool fail_cf_restore_once = false;
    bool trigger_reentrant_open = false;
    Error reentrant_open_error = Error::OK;
    std::size_t control_count_at_reentrant_open = 0U;
    bool reentrant_open_added_control = false;
    std::size_t controls_at_restore = 0U;

    FakeUsb() {
        controller[0x05] = 0U;
        controller[0x09] = 0x1eU;
        for (std::size_t local = 0; local < cf.size(); ++local) {
            for (std::size_t offset = 0; offset < cf[local].size(); ++offset)
                cf[local][offset] = static_cast<std::uint8_t>(0x30U + local + offset);
            cf[local][0x40] = 0x04U;
        }
    }

    static int control_hook(void* context, const ControlTransfer& transfer,
                            unsigned char* response) {
        return static_cast<FakeUsb*>(context)->control(transfer, response);
    }
    static libusb_transfer* allocate_hook(void*) {
        return static_cast<libusb_transfer*>(std::calloc(1U, sizeof(libusb_transfer)));
    }
    static int submit_hook(void* context, libusb_transfer* transfer) {
        auto& fake = *static_cast<FakeUsb*>(context);
        ++fake.submit_count;
        fake.endpoints.push_back(transfer->endpoint);
        if (fake.fail_submit_number != 0 &&
            fake.submit_count == fake.fail_submit_number) return LIBUSB_ERROR_IO;
        return 0;
    }
    static int cancel_hook(void* context, libusb_transfer* transfer) {
        auto& fake = *static_cast<FakeUsb*>(context);
        ++fake.cancel_count;
        transfer->status = LIBUSB_TRANSFER_CANCELLED;
        transfer->actual_length = 0;
        if (transfer->callback != nullptr) transfer->callback(transfer);
        return 0;
    }
    static void free_hook(void* context, libusb_transfer* transfer) {
        ++static_cast<FakeUsb*>(context)->free_count;
        std::free(transfer);
    }
    static int pump_hook(void*, unsigned) { return 0; }

    Hooks hooks() {
        return {this, control_hook, allocate_hook, submit_hook, cancel_hook,
                free_hook, pump_hook};
    }

    bool is_cf_restore(const ControlTransfer& transfer) const {
        return transfer.request == Request::ChannelFilterWrite && dsc_stop_count > 0;
    }

    int control(const ControlTransfer& transfer, unsigned char* response) {
        controls.push_back({transfer.request, transfer.value, transfer.index});
        const auto status = [&] {
            if (response != nullptr && transfer.length > 0U) response[0] = 1U;
            return static_cast<int>(transfer.length);
        };
        switch (transfer.request) {
            case Request::SysCtrlRead:
                if (response != nullptr && transfer.length >= 3U) {
                    response[0] = 1U;
                    response[1] = 0x11U;
                    response[2] = 0x52U;
                }
                return static_cast<int>(transfer.length);
            case Request::I2cRead: {
                if (response == nullptr || transfer.length == 0U) return LIBUSB_ERROR_IO;
                response[0] = 1U;
                const std::uint8_t slave = static_cast<std::uint8_t>(transfer.value & 0xffU);
                const std::uint8_t first = static_cast<std::uint8_t>(transfer.value >> 8U);
                const std::size_t payload = transfer.length - 1U;
                for (std::size_t i = 0; i < payload; ++i) {
                    const std::uint8_t reg = static_cast<std::uint8_t>(first + i);
                    response[i + 1U] = slave == 0x4aU ? controller[reg] : 0U;
                }
                return static_cast<int>(transfer.length);
            }
            case Request::I2cWrite:
            case Request::I2cWriteNoStop: {
                const std::uint8_t slave = static_cast<std::uint8_t>(transfer.value & 0xffU);
                const std::uint8_t first = static_cast<std::uint8_t>(transfer.value >> 8U);
                if (slave == 0x4aU && transfer.length > 1U) {
                    const std::uint8_t value = static_cast<std::uint8_t>(transfer.index & 0xffU);
                    if (first >= 0x10U && first <= 0x1fU) {
                        if (value == 0U) ++seed_clear_count;
                        else ++seed_write_count;
                        if (fail_seed_once && value != 0U) {
                            fail_seed_once = 0;
                            if (response != nullptr) response[0] = 0U;
                            return static_cast<int>(transfer.length);
                        }
                    }
                    controller[first] = value;
                    if (transfer.length > 2U)
                        controller[static_cast<std::uint8_t>(first + 1U)] =
                            static_cast<std::uint8_t>(transfer.index >> 8U);
                }
                return status();
            }
            case Request::ChannelFilterRead: {
                if (response == nullptr || transfer.length == 0U) return LIBUSB_ERROR_IO;
                response[0] = 0U;  // W3U3 CF reads use transfer length, not this byte.
                const std::uint8_t subcmd = static_cast<std::uint8_t>(transfer.value & 0xffU);
                const std::size_t local = (subcmd & 0x80U) != 0U ? 1U : 0U;
                const std::size_t offset = subcmd & 0x7fU;
                const std::size_t count = transfer.length - 1U;
                for (std::size_t i = 0; i < count && offset + i < cf[local].size(); ++i)
                    response[i + 1U] = cf[local][offset + i];
                return static_cast<int>(transfer.length);
            }
            case Request::ChannelFilterWrite: {
                if (is_cf_restore(transfer)) {
                    controls_at_restore = controls.size();
                    if (trigger_reentrant_open && hardware != nullptr) {
                        trigger_reentrant_open = false;
                        LibusbW3u3HardwareTestPeer::set_cleanup_failed(*hardware);
                        const std::size_t before = controls.size();
                        const auto open = hardware->open_receiver(1U);
                        reentrant_open_error = open.error();
                        reentrant_open_added_control = controls.size() != before;
                    }
                    if (fail_cf_restore_once) {
                        fail_cf_restore_once = false;
                        return static_cast<int>(transfer.length) - 1;
                    }
                }
                const std::uint8_t subcmd = static_cast<std::uint8_t>(transfer.value & 0xffU);
                const std::size_t local = (subcmd & 0x80U) != 0U ? 1U : 0U;
                const std::size_t offset = subcmd & 0x7fU;
                const std::size_t count = transfer.length - 1U;
                if (offset < cf[local].size())
                    cf[local][offset] = static_cast<std::uint8_t>(transfer.value >> 8U);
                if (count > 1U && offset + 1U < cf[local].size())
                    cf[local][offset + 1U] = static_cast<std::uint8_t>(transfer.index & 0xffU);
                if (count > 2U && offset + 2U < cf[local].size())
                    cf[local][offset + 2U] = static_cast<std::uint8_t>(transfer.index >> 8U);
                return static_cast<int>(transfer.length);
            }
            case Request::DscStart:
                ++dsc_start_count;
                if (response != nullptr && transfer.length > 0U)
                    response[0] = fail_dsc_start ? 0U : 1U;
                return static_cast<int>(transfer.length);
            case Request::DscStop:
                ++dsc_stop_count;
                return status();
            case Request::ResetChannel: {
                const std::size_t local = static_cast<std::uint8_t>(transfer.value) == 1U ? 1U : 0U;
                cf[local].fill(0U);
                return status();
            }
            default:
                return status();
        }
    }

    std::array<std::uint8_t, 0x45> initial_cf(std::size_t local) const {
        return local < cf.size() ? initial_cf_[local] : std::array<std::uint8_t, 0x45>{};
    }
    void save_initial_cf() { initial_cf_ = cf; }

private:
    std::array<std::array<std::uint8_t, 0x45>, 2> initial_cf_{};
};

bool check(bool condition, const char* expression, int line) {
    if (condition) return true;
    std::fprintf(stderr, "line %d: CHECK failed: %s\n", line, expression);
    return false;
}
#define CHECK(expr) do { if (!check((expr), #expr, __LINE__)) return false; } while (false)

void configure(LibusbW3u3Hardware& hardware, FakeUsb& fake,
               Hooks& hooks, std::uint8_t receiver) {
    hooks = fake.hooks();
    fake.hardware = &hardware;
    fake.save_initial_cf();
    LibusbW3u3HardwareTestPeer::configure(hardware, &hooks, receiver);
}

bool saw_dsc(const FakeUsb& fake, Request request, std::uint8_t local) {
    return std::any_of(fake.controls.begin(), fake.controls.end(), [&](const ControlEvent& event) {
        return event.request == request && event.value == local;
    });
}

bool has_cf_request(const FakeUsb& fake, std::size_t local, Request request) {
    return std::any_of(fake.controls.begin(), fake.controls.end(), [&](const ControlEvent& event) {
        if (event.request != request) return false;
        const std::uint8_t subcmd = static_cast<std::uint8_t>(event.value & 0xffU);
        return ((subcmd & 0x80U) != 0U ? 1U : 0U) == local;
    });
}

bool discard_packets(void*, const std::uint8_t*, std::size_t) { return true; }

bool test_primary_prepare_stop_and_receiver1_reacquisition() {
    FakeUsb fake;
    LibusbW3u3Hardware hardware(nullptr, {}, {}, {}, {});
    Hooks hooks{};
    configure(hardware, fake, hooks, 0U);

    std::atomic<bool> cancelled{false};
    CHECK(hardware.prepare(0U, System::ISDB_S, cancelled).has_value());
    CHECK(fake.submit_count == 4);
    CHECK(std::all_of(fake.endpoints.begin(), fake.endpoints.end(),
                      [](std::uint8_t endpoint) { return endpoint == 0x81U; }));
    CHECK(saw_dsc(fake, Request::DscStart, 0U));
    CHECK(has_cf_request(fake, 0U, Request::ChannelFilterRead));
    CHECK(has_cf_request(fake, 0U, Request::ChannelFilterWrite));
    CHECK(!has_cf_request(fake, 1U, Request::ChannelFilterRead));
    const std::size_t before_busy_open = fake.controls.size();
    CHECK(hardware.open_receiver(1U).error() == Error::BUSY);
    CHECK(fake.controls.size() == before_busy_open);

    cancelled.store(true);
    CHECK(hardware.run(cancelled, discard_packets, nullptr) == CaptureRunResult::cancelled);
    CHECK(saw_dsc(fake, Request::DscStop, 0U));
    CHECK(fake.cancel_count == 4);
    CHECK(fake.free_count == 4);
    CHECK(fake.cf[0] == fake.initial_cf(0U));
    CHECK(hardware.close_receiver(0U).has_value());
    CHECK(hardware.open_receiver(1U).has_value());
    LibusbW3u3HardwareTestPeer::mark_tuned(hardware, 1U);

    fake.endpoints.clear();
    cancelled.store(false);
    const int old_start_count = fake.dsc_start_count;
    CHECK(hardware.prepare(1U, System::ISDB_T, cancelled).has_value());
    CHECK(fake.dsc_start_count == old_start_count + 1);
    CHECK(std::all_of(fake.endpoints.begin(), fake.endpoints.end(),
                      [](std::uint8_t endpoint) { return endpoint == 0x82U; }));
    CHECK(saw_dsc(fake, Request::DscStart, 1U));
    CHECK(hardware.stop().has_value());
    CHECK(saw_dsc(fake, Request::DscStop, 1U));
    CHECK(fake.cf[1] == fake.initial_cf(1U));
    return true;
}

bool test_partial_submit_rolls_back_local0_and_releases_lane() {
    FakeUsb fake;
    fake.fail_submit_number = 3;
    LibusbW3u3Hardware hardware(nullptr, {}, {}, {}, {});
    Hooks hooks{};
    configure(hardware, fake, hooks, 0U);
    std::atomic<bool> cancelled{false};

    CHECK(!hardware.prepare(0U, System::ISDB_S, cancelled).has_value());
    CHECK(fake.submit_count == 3);
    CHECK(fake.cancel_count == 2);
    CHECK(fake.free_count == 4);
    CHECK(fake.dsc_start_count == 0);
    CHECK(fake.cf[0] == fake.initial_cf(0U));
    CHECK(hardware.close_receiver(0U).has_value());
    CHECK(hardware.open_receiver(1U).has_value());
    return true;
}

bool test_dsc_failure_stops_local0_and_restores_cf() {
    FakeUsb fake;
    fake.fail_dsc_start = true;
    LibusbW3u3Hardware hardware(nullptr, {}, {}, {}, {});
    Hooks hooks{};
    configure(hardware, fake, hooks, 0U);
    std::atomic<bool> cancelled{false};

    CHECK(!hardware.prepare(0U, System::ISDB_S, cancelled).has_value());
    CHECK(saw_dsc(fake, Request::DscStart, 0U));
    CHECK(saw_dsc(fake, Request::DscStop, 0U));
    CHECK(fake.cancel_count == 4);
    CHECK(fake.cf[0] == fake.initial_cf(0U));
    CHECK(fake.controller[0x05] == 0U);
    return true;
}

bool test_seed_failure_stops_local0_clears_seed_and_restores_cf() {
    FakeUsb fake;
    fake.fail_seed_once = 1;
    LibusbW3u3Hardware hardware(nullptr, {}, {}, {}, {});
    Hooks hooks{};
    configure(hardware, fake, hooks, 0U);
    std::atomic<bool> cancelled{false};

    CHECK(!hardware.prepare(0U, System::ISDB_S, cancelled).has_value());
    CHECK(saw_dsc(fake, Request::DscStart, 0U));
    CHECK(saw_dsc(fake, Request::DscStop, 0U));
    CHECK(fake.seed_write_count >= 1);
    CHECK(fake.seed_clear_count == 16);
    CHECK(fake.controller[0x05] == 0U);
    CHECK(fake.cancel_count == 4);
    CHECK(fake.cf[0] == fake.initial_cf(0U));
    return true;
}

bool test_rejected_open_reserves_before_cleanup_state_and_quarantines() {
    FakeUsb fake;
    LibusbW3u3Hardware hardware(nullptr, {}, {}, {}, {});
    Hooks hooks{};
    configure(hardware, fake, hooks, 0U);
    std::atomic<bool> cancelled{false};
    CHECK(hardware.prepare(0U, System::ISDB_S, cancelled).has_value());
    fake.trigger_reentrant_open = true;
    fake.fail_cf_restore_once = true;

    CHECK(!hardware.stop().has_value());
    CHECK(fake.reentrant_open_error == Error::BUSY);
    CHECK(!fake.reentrant_open_added_control);
    CHECK(fake.controls_at_restore > 0U);
    const std::size_t before_quarantined_open = fake.controls.size();
    CHECK(hardware.open_receiver(1U).error() == Error::BUSY);
    CHECK(fake.controls.size() == before_quarantined_open);
    return true;
}

}  // namespace

int main() {
    if (!test_primary_prepare_stop_and_receiver1_reacquisition()) return 1;
    if (!test_partial_submit_rolls_back_local0_and_releases_lane()) return 1;
    if (!test_dsc_failure_stops_local0_and_restores_cf()) return 1;
    if (!test_seed_failure_stops_local0_clears_seed_and_restores_cf()) return 1;
    if (!test_rejected_open_reserves_before_cleanup_state_and_quarantines()) return 1;
    return 0;
}
