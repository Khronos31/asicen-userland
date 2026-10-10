// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/libusb_hardware_backend.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <map>
#include <tuple>
#include <thread>
#include <vector>

namespace asicen {

struct LibusbW3u3HardwareTestPeer {
    using Hooks = LibusbW3u3Hardware::CaptureUsbHooks;

    static void configure(LibusbW3u3Hardware& hardware, const Hooks* hooks, std::uint8_t receiver)
    {
        if (hardware.profile_ == nullptr) {
            hardware.profile_ = find_profile(ModelId::W3u3);
        }
        hardware.capture_usb_hooks_ = hooks;
        hardware.claimed_ = true;
        hardware.initialized_ = true;
        hardware.tuned_ = true;
        hardware.tuned_receiver_ = receiver;
        hardware.tuned_frequency_khz_ = 557142U;
        hardware.tuned_system_ = receiver == 0U ? px4::userland::ipc::System::ISDB_S
                                                : px4::userland::ipc::System::ISDB_T;
        (void)hardware.active_receiver_.reserve(receiver);
    }

    static bool verify_v2_roles(LibusbW3u3Hardware& hardware)
    {
        return hardware.verify_v2_pair_roles();
    }
    static bool v2_identity_verified(const LibusbW3u3Hardware& hardware)
    {
        return hardware.v2_identity_verified_;
    }
    static bool claim_fixture(LibusbW3u3Hardware& hardware, UsbFunctionClaim& primary,
                              UsbFunctionClaim* sibling)
    {
        const auto* profile = hardware.profile_;
        return profile != nullptr &&
               hardware.ownership_.claim_profile(*profile, primary, sibling, {1U, 2U, 1U},
                                                 sibling == nullptr
                                                     ? std::vector<std::uint8_t>{}
                                                     : std::vector<std::uint8_t>{1U, 2U, 2U}) ==
                   OwnershipError::none;
    }
    static void require_initialization(LibusbW3u3Hardware& hardware)
    {
        hardware.initialized_ = false;
        hardware.tuned_ = false;
    }
    static bool gain_applied(const LibusbW3u3Hardware& hardware) { return hardware.gain_applied_; }
    static bool gpio_snapshot_valid(const LibusbW3u3Hardware& hardware)
    {
        return hardware.gpio_snapshot_valid_ || hardware.board_power_attempted_;
    }
    static void install_hooks_only(LibusbW3u3Hardware& hardware, const Hooks* hooks)
    {
        hardware.capture_usb_hooks_ = hooks;
        hardware.claimed_ = true;
    }
    static void mark_tuned(LibusbW3u3Hardware& hardware, std::uint8_t receiver)
    {
        hardware.tuned_ = true;
        hardware.tuned_receiver_ = receiver;
        hardware.tuned_frequency_khz_ = 557142U;
        hardware.tuned_system_ = receiver == 0U ? px4::userland::ipc::System::ISDB_S
                                                : px4::userland::ipc::System::ISDB_T;
    }

    static void set_cleanup_failed(LibusbW3u3Hardware& hardware)
    {
        hardware.mark_cleanup_failed(px4::userland::Error::USB_IO);
    }
    static void set_gpio_snapshot(LibusbW3u3Hardware& hardware, std::uint8_t value)
    {
        hardware.gpio_snapshot_ = value;
        hardware.gpio_snapshot_valid_ = true;
    }
    static bool begin_card_cleanup(LibusbW3u3Hardware& hardware)
    {
        return hardware.begin_card_cleanup(1000U).has_value();
    }
    static void end_card_cleanup(LibusbW3u3Hardware& hardware, bool success)
    {
        hardware.end_card_cleanup(success);
    }
    static bool begin_card_operation(LibusbW3u3Hardware& hardware)
    {
        return hardware.begin_card_operation(1000U, nullptr).has_value();
    }
    static void end_card_operation(LibusbW3u3Hardware& hardware) { hardware.end_card_operation(); }
    static int card_control(LibusbW3u3Hardware& hardware, const ControlTransfer& transfer,
                            unsigned char* response)
    {
        return static_cast<FrontendTransport&>(hardware).control(transfer, response);
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
    std::uint16_t timeout_ms = 0;
    std::chrono::steady_clock::time_point at{};
};

class FakeFunction final : public UsbFunctionClaim {
public:
    UsbFunctionSnapshot state{};
    UsbFunctionSnapshot snapshot() const override { return state; }
    int claim_interface0() override { return 0; }
    int release_interface0() noexcept override { return 0; }
};

struct FakeUsb {
    LibusbW3u3Hardware* hardware = nullptr;
    FakeFunction primary;
    FakeFunction sibling;
    std::array<std::uint8_t, 256> controller{};
    std::array<std::array<std::uint8_t, 0x45>, 2> cf{};
    std::vector<ControlEvent> controls;
    std::vector<std::uint8_t> control_functions;
    std::uint8_t current_function = 0U;
    std::array<std::uint8_t, 2> role_fields{{0U, 0x80U}};
    bool identity_mismatch = false;
    bool v2_locked = true;
    bool v2_calibration_ready = true;
    using V2Key = std::tuple<std::uint8_t, bool, std::uint16_t>;
    std::map<V2Key, std::uint32_t> v2_registers;
    std::array<std::uint8_t, 64> v2_staged{};
    std::uint16_t v2_selected_reg = 0;
    bool v2_selected_tuner = false;
    std::vector<std::uint8_t> endpoints;
    std::atomic<int> submit_count{0};
    int fail_submit_number = 0;
    int fail_submit_error = LIBUSB_ERROR_IO;
    int fail_control_once = 0;
    bool short_control_once = false;
    bool expire_after_power_write = false;
    unsigned fake_elapsed_ms = 0U;
    unsigned expire_after_tsid_read_ms = 0U;
    unsigned missing_tsid_reads = 0U;
    int cancel_count = 0;
    int free_count = 0;
    int dsc_start_count = 0;
    int dsc_stop_count = 0;
    int seed_write_count = 0;
    int seed_clear_count = 0;
    int fail_seed_once = 0;
    bool fail_dsc_start = false;
    bool fail_cf_restore_once = false;
    std::uint8_t gpio = 0xa5U;
    std::uint8_t gpio_ex = 0xa3U;
    std::uint8_t revision = 0x11U;
    std::uint8_t sibling_revision = 0x11U;
    int gpio_ex_write_count = 0;
    int gpio_ex_read_count = 0;
    bool fail_gpio_ex_off_once = false;
    std::uint8_t gpio_ex_external_low = 0U;
    int gpio_write_count = 0;
    int gpio_read_count = 0;
    bool trigger_reentrant_open = false;
    Error reentrant_open_error = Error::OK;
    std::size_t control_count_at_reentrant_open = 0U;
    bool reentrant_open_added_control = false;
    std::size_t controls_at_restore = 0U;
    std::array<libusb_transfer*, 4> submitted{};
    std::atomic<bool> complete_inside_control{false};
    std::atomic<unsigned> callback_count{0U};
    std::atomic<unsigned> callback_total{0U};
    std::atomic<unsigned> pump_count{0U};
    bool require_callbacks_before_free = false;
    bool suppress_cancel_callbacks = false;
    int free_before_callbacks = 0;
    std::mutex event_mutex;
    std::condition_variable event_changed;
    bool interrupted_events = false;
    bool event_ready = false;

    FakeUsb()
    {
        for (auto slave : {0x20U, 0x24U, 0x28U, 0x2cU}) {
            v2_registers[{static_cast<std::uint8_t>(slave), true, 0x3fcU}] = 0x12000U;
        }
        for (auto slave : {0x22U, 0x26U, 0x2aU, 0x2eU}) {
            v2_registers[{static_cast<std::uint8_t>(slave), true, 0x21U}] = 0x40U;
        }
        v2_registers[{0x22U, false, 0xceU}] = 0x40U;
        v2_registers[{0x22U, false, 0xcfU}] = 0x10U;
        controller[0x05] = 0U;
        controller[0x09] = 0x1eU;
        for (std::size_t local = 0; local < cf.size(); ++local) {
            for (std::size_t offset = 0; offset < cf[local].size(); ++offset) {
                cf[local][offset] = static_cast<std::uint8_t>(0x30U + local + offset);
            }
            cf[local][0x40] = 0x04U;
        }
    }

    static int control_hook(void* context, const ControlTransfer& transfer, unsigned char* response)
    {
        return static_cast<FakeUsb*>(context)->control(transfer, response);
    }
    static std::chrono::steady_clock::time_point now_hook(void* context)
    {
        return std::chrono::steady_clock::now() +
               std::chrono::milliseconds(static_cast<FakeUsb*>(context)->fake_elapsed_ms);
    }
    static int control_function_hook(void* context, std::uint8_t function,
                                     const ControlTransfer& transfer, unsigned char* response)
    {
        auto& fake = *static_cast<FakeUsb*>(context);
        fake.current_function = function;
        const int result = fake.control(transfer, response);
        fake.current_function = 0U;
        return result;
    }
    static libusb_transfer* allocate_hook(void*)
    {
        return static_cast<libusb_transfer*>(std::calloc(1U, sizeof(libusb_transfer)));
    }
    static int submit_hook(void* context, libusb_transfer* transfer)
    {
        auto& fake = *static_cast<FakeUsb*>(context);
        ++fake.submit_count;
        fake.submitted[static_cast<std::size_t>((fake.submit_count - 1) % 4)] = transfer;
        fake.endpoints.push_back(transfer->endpoint);
        if (fake.fail_submit_number != 0 && fake.submit_count == fake.fail_submit_number) {
            return fake.fail_submit_error;
        }
        return 0;
    }
    static int cancel_hook(void* context, libusb_transfer* transfer)
    {
        auto& fake = *static_cast<FakeUsb*>(context);
        ++fake.cancel_count;
        transfer->status = LIBUSB_TRANSFER_CANCELLED;
        transfer->actual_length = 0;
        if (transfer->callback != nullptr && !fake.suppress_cancel_callbacks) {
            transfer->callback(transfer);
            fake.callback_total.fetch_add(1U);
        }
        return 0;
    }
    static void free_hook(void* context, libusb_transfer* transfer)
    {
        auto& fake = *static_cast<FakeUsb*>(context);
        if (fake.require_callbacks_before_free && fake.callback_total.load() < 4U) {
            ++fake.free_before_callbacks;
        }
        ++fake.free_count;
        std::free(transfer);
    }
    static int pump_hook(void*, unsigned) { return 0; }

    static int pump_wait_hook(void* context, unsigned timeout_ms)
    {
        auto& fake = *static_cast<FakeUsb*>(context);
        ++fake.pump_count;
        std::unique_lock<std::mutex> lock(fake.event_mutex);
        fake.event_changed.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                    [&] { return fake.interrupted_events || fake.event_ready; });
        fake.interrupted_events = false;
        fake.event_ready = false;
        return 0;
    }

    static void interrupt_events_hook(void* context)
    {
        auto& fake = *static_cast<FakeUsb*>(context);
        {
            std::lock_guard<std::mutex> lock(fake.event_mutex);
            fake.interrupted_events = true;
        }
        fake.event_changed.notify_all();
    }

    Hooks hooks()
    {
        return {this,        control_hook, allocate_hook,  submit_hook,
                cancel_hook, free_hook,    pump_wait_hook, interrupt_events_hook};
    }

    bool is_cf_restore(const ControlTransfer& transfer) const
    {
        return transfer.request == Request::ChannelFilterWrite && dsc_stop_count > 0;
    }

    int control(const ControlTransfer& transfer, unsigned char* response)
    {
        if (short_control_once) {
            short_control_once = false;
            return transfer.length - 1;
        }
        if (fail_control_once != 0) {
            const int error = fail_control_once;
            fail_control_once = 0;
            return error;
        }
        controls.push_back({transfer.request, transfer.value, transfer.index, transfer.timeout_ms,
                            std::chrono::steady_clock::now()});
        control_functions.push_back(current_function);
        const auto status = [&] {
            if (response != nullptr && transfer.length > 0U) {
                response[0] = 1U;
            }
            return static_cast<int>(transfer.length);
        };
        switch (transfer.request) {
        case Request::CustomerInfo:
            if (response == nullptr || transfer.length != kCustomerInfoSize) {
                return LIBUSB_ERROR_IO;
            }
            std::fill_n(response, transfer.length, 0U);
            response[0] = 1U;
            response[3] = 0x06U;
            response[4] = 0x0bU;
            response[5] = 0x06U;
            response[57] = role_fields[current_function];
            return transfer.length;
        case Request::I2cBufferFill: {
            const auto offset = transfer.value & 0xffU;
            const std::uint8_t bytes[] = {static_cast<std::uint8_t>(transfer.value >> 8U),
                                          static_cast<std::uint8_t>(transfer.index),
                                          static_cast<std::uint8_t>(transfer.index >> 8U)};
            for (unsigned i = 0; i < transfer.length - 1U; ++i) {
                v2_staged.at(offset + i) = bytes[i];
            }
            return status();
        }
        case Request::I2cBufferSend: {
            const auto slave = static_cast<std::uint8_t>(transfer.value);
            const auto size = transfer.length - 1U;
            if (v2_staged[0] == 0xfeU && size >= 2U) {
                v2_selected_tuner = true;
                if (v2_staged[1] == 0xceU && size >= 4U) {
                    v2_selected_reg =
                        static_cast<std::uint16_t>((v2_staged[2] << 8U) | v2_staged[3]);
                    if (size > 4U) {
                        std::uint32_t value = 0U;
                        for (unsigned i = 4U; i < size; ++i) {
                            value |= static_cast<std::uint32_t>(v2_staged[i]) << (8U * (i - 4U));
                        }
                        v2_registers[{slave, true, v2_selected_reg}] = value;
                    }
                } else if (v2_staged[1] == 0xa8U && size >= 3U) {
                    v2_selected_reg = v2_staged[2];
                    if (size == 4U) {
                        v2_registers[{slave, true, v2_selected_reg}] = v2_staged[3];
                    }
                }
            } else {
                v2_selected_tuner = false;
                v2_selected_reg = v2_staged[0];
                if (size == 2U) {
                    v2_registers[{slave, false, v2_selected_reg}] = v2_staged[1];
                }
            }
            return status();
        }
        case Request::I2cReadNoWait: {
            if (response == nullptr || transfer.length == 0U) {
                return LIBUSB_ERROR_IO;
            }
            response[0] = 1U;
            const auto slave = static_cast<std::uint8_t>(transfer.value);
            auto value = v2_registers[{slave, v2_selected_tuner, v2_selected_reg}];
            if (v2_selected_tuner && (slave & 2U) &&
                (v2_selected_reg == 0x11U || v2_selected_reg == 0x15U)) {
                value = v2_calibration_ready ? value | 0x10U : value & ~0x10U;
            }
            if (!v2_selected_tuner) {
                if (v2_selected_reg == 0xb0U) {
                    value = v2_locked ? 0xa8U : 0U;
                }
                if (v2_selected_reg == 0x80U) {
                    value = v2_locked ? 0U : 8U;
                }
                if (v2_selected_reg == 0xc3U) {
                    value = v2_locked ? 0U : 0x10U;
                }
            }
            for (unsigned i = 1U; i < transfer.length; ++i) {
                response[i] = static_cast<std::uint8_t>(value >> (8U * (i - 1U)));
            }
            return transfer.length;
        }
        case Request::SysCtrlRead:
            if (response != nullptr && transfer.length >= 3U) {
                response[0] = 1U;
                response[1] = current_function == 0U ? revision : sibling_revision;
                response[2] = 0x52U;
            }
            return static_cast<int>(transfer.length);
        case Request::I2cRead: {
            if (response == nullptr || transfer.length == 0U) {
                return LIBUSB_ERROR_IO;
            }
            response[0] = 1U;
            const std::uint8_t slave = static_cast<std::uint8_t>(transfer.value & 0xffU);
            const std::uint8_t first = static_cast<std::uint8_t>(transfer.value >> 8U);
            if (first == 0xceU && expire_after_tsid_read_ms != 0U) {
                fake_elapsed_ms = expire_after_tsid_read_ms;
                expire_after_tsid_read_ms = 0U;
            }
            const std::size_t payload = transfer.length - 1U;
            for (std::size_t i = 0; i < payload; ++i) {
                const std::uint8_t reg = static_cast<std::uint8_t>(first + i);
                response[i + 1U] =
                    slave == 0x4aU ? controller[reg]
                    : slave == 0xa8U && first == 0xb0U
                        ? static_cast<std::uint8_t>(
                              0x50U + i + (identity_mismatch && current_function == 1U ? 1U : 0U))
                        : 0U;
            }
            if (first == 0xceU && missing_tsid_reads != 0U) {
                --missing_tsid_reads;
                std::fill_n(response + 1U, payload, 0xffU);
            }
            if (complete_inside_control.exchange(false)) {
                libusb_transfer* pending = submitted[0];
                if (pending != nullptr && pending->callback != nullptr) {
                    std::memset(pending->buffer, 0x47, static_cast<std::size_t>(pending->length));
                    for (std::size_t offset = 0; offset < 188U; offset += 188U) {
                        pending->buffer[offset] = 0x47U;
                    }
                    pending->actual_length = 188;
                    pending->status = LIBUSB_TRANSFER_COMPLETED;
                    pending->callback(pending);
                    callback_count.fetch_add(1U);
                    callback_total.fetch_add(1U);
                    {
                        std::lock_guard<std::mutex> lock(event_mutex);
                        event_ready = true;
                    }
                    event_changed.notify_all();
                }
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
                    if (value == 0U) {
                        ++seed_clear_count;
                    } else {
                        ++seed_write_count;
                    }
                    if (fail_seed_once && value != 0U) {
                        fail_seed_once = 0;
                        if (response != nullptr) {
                            response[0] = 0U;
                        }
                        return static_cast<int>(transfer.length);
                    }
                }
                controller[first] = value;
                if (transfer.length > 2U) {
                    controller[static_cast<std::uint8_t>(first + 1U)] =
                        static_cast<std::uint8_t>(transfer.index >> 8U);
                }
            }
            return status();
        }
        case Request::ChannelFilterRead: {
            if (response == nullptr || transfer.length == 0U) {
                return LIBUSB_ERROR_IO;
            }
            response[0] = 0U;  // W3U3 CF reads use transfer length, not this byte.
            const std::uint8_t subcmd = static_cast<std::uint8_t>(transfer.value & 0xffU);
            const std::size_t local = (subcmd & 0x80U) != 0U ? 1U : 0U;
            const std::size_t offset = subcmd & 0x7fU;
            const std::size_t count = transfer.length - 1U;
            for (std::size_t i = 0; i < count && offset + i < cf[local].size(); ++i) {
                response[i + 1U] = cf[local][offset + i];
            }
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
            if (offset < cf[local].size()) {
                cf[local][offset] = static_cast<std::uint8_t>(transfer.value >> 8U);
            }
            if (count > 1U && offset + 1U < cf[local].size()) {
                cf[local][offset + 1U] = static_cast<std::uint8_t>(transfer.index & 0xffU);
            }
            if (count > 2U && offset + 2U < cf[local].size()) {
                cf[local][offset + 2U] = static_cast<std::uint8_t>(transfer.index >> 8U);
            }
            return static_cast<int>(transfer.length);
        }
        case Request::DscStart:
            ++dsc_start_count;
            if (response != nullptr && transfer.length > 0U) {
                response[0] = fail_dsc_start ? 0U : 1U;
            }
            return static_cast<int>(transfer.length);
        case Request::DscStop:
            ++dsc_stop_count;
            return status();
        case Request::GpioRead:
            if (response == nullptr) {
                return LIBUSB_ERROR_IO;
            }
            response[0] = gpio;
            return static_cast<int>(transfer.length);
        case Request::Gpio: {
            if (response == nullptr) {
                return LIBUSB_ERROR_IO;
            }
            const std::uint8_t value = static_cast<std::uint8_t>(transfer.value & 0xffU);
            const std::uint8_t mask = static_cast<std::uint8_t>(transfer.value >> 8U);
            if (mask == 0U) {
                ++gpio_read_count;
            } else {
                ++gpio_write_count;
                gpio = static_cast<std::uint8_t>((gpio & ~mask) | (value & mask));
                if (expire_after_power_write && mask == 0x9bU) {
                    fake_elapsed_ms = 20001U;
                    expire_after_power_write = false;
                }
            }
            response[0] = gpio;
            return static_cast<int>(transfer.length);
        }
        case Request::GpioExGet:
            ++gpio_ex_read_count;
            if (response == nullptr) {
                return LIBUSB_ERROR_IO;
            }
            response[0] = static_cast<std::uint8_t>(gpio_ex & ~gpio_ex_external_low);
            return static_cast<int>(transfer.length);
        case Request::GpioExSet: {
            ++gpio_ex_write_count;
            if (fail_gpio_ex_off_once && transfer.value == setup_word(0x02U, 0x02U)) {
                fail_gpio_ex_off_once = false;
                return LIBUSB_ERROR_IO;
            }
            const auto value = static_cast<std::uint8_t>(transfer.value);
            const auto mask = static_cast<std::uint8_t>(transfer.value >> 8U);
            gpio_ex = static_cast<std::uint8_t>((gpio_ex & ~mask) | (value & mask));
            if (response == nullptr) {
                return LIBUSB_ERROR_IO;
            }
            response[0] = gpio_ex;
            return static_cast<int>(transfer.length);
        }
        case Request::ResetChannel: {
            const std::size_t local = static_cast<std::uint8_t>(transfer.value) == 1U ? 1U : 0U;
            cf[local].fill(0U);
            return status();
        }
        default:
            return status();
        }
    }

    std::array<std::uint8_t, 0x45> initial_cf(std::size_t local) const
    {
        return local < cf.size() ? initial_cf_[local] : std::array<std::uint8_t, 0x45>{};
    }
    void save_initial_cf() { initial_cf_ = cf; }

private:
    std::array<std::array<std::uint8_t, 0x45>, 2> initial_cf_{};
};

bool check(bool condition, const char* expression, int line)
{
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "line %d: CHECK failed: %s\n", line, expression);
    return false;
}
#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!check((expr), #expr, __LINE__))                                                       \
            return false;                                                                          \
    } while (false)

void configure(LibusbW3u3Hardware& hardware, FakeUsb& fake, Hooks& hooks, std::uint8_t receiver)
{
    hooks = fake.hooks();
    fake.hardware = &hardware;
    fake.save_initial_cf();
    LibusbW3u3HardwareTestPeer::configure(hardware, &hooks, receiver);
    const auto* profile = hardware.device_profile();
    if (profile->model_id == ModelId::W3u3V2) {
        hooks.control_function = FakeUsb::control_function_hook;
    }
    UsbFunctionSnapshot snapshot{};
    snapshot.vendor_id = profile->vid;
    snapshot.product_id = profile->pid;
    snapshot.bus = 1U;
    snapshot.address = 4U;
    snapshot.port_path = {1U, 2U, 1U};
    snapshot.interface0_present = true;
    snapshot.kernel_driver_state_known = true;
    snapshot.active_alt0 = 0;
    snapshot.endpoint81_in_alt0 = snapshot.endpoint81_bulk_in_alt0 = true;
    snapshot.endpoint82_in_alt0 = snapshot.endpoint82_bulk_in_alt0 = true;
    fake.primary.state = snapshot;
    snapshot.address = 5U;
    snapshot.port_path = {1U, 2U, 2U};
    fake.sibling.state = snapshot;
    if (!LibusbW3u3HardwareTestPeer::claim_fixture(
            hardware, fake.primary,
            profile->expected_runtime_functions == 2U ? &fake.sibling : nullptr)) {
        std::abort();
    }
}

bool saw_dsc(const FakeUsb& fake, Request request, std::uint8_t local)
{
    for (const auto& event : fake.controls) {
        if (event.request == request && event.value == local) {
            return true;
        }
    }
    return false;
}

bool has_cf_request(const FakeUsb& fake, std::size_t local, Request request)
{
    return std::any_of(fake.controls.begin(), fake.controls.end(), [&](const ControlEvent& event) {
        if (event.request != request) {
            return false;
        }
        const std::uint8_t subcmd = static_cast<std::uint8_t>(event.value & 0xffU);
        return ((subcmd & 0x80U) != 0U ? 1U : 0U) == local;
    });
}

bool discard_packets(void*, const std::uint8_t*, std::size_t)
{
    return true;
}

bool test_transport_errors_keep_their_public_class()
{
    const std::array<std::pair<int, Error>, 4> errors{{
        {LIBUSB_ERROR_TIMEOUT, Error::TIMEOUT},
        {LIBUSB_ERROR_BUSY, Error::BUSY},
        {LIBUSB_ERROR_NO_MEM, Error::INTERNAL},
        {LIBUSB_ERROR_NO_DEVICE, Error::DISCONNECTED},
    }};
    for (const auto& failure : errors) {
        for (unsigned operation = 0U; operation < 9U; ++operation) {
            FakeUsb fake;
            LibusbW3u3Hardware hardware(nullptr, {}, {}, {}, {});
            Hooks hooks{};
            const std::uint8_t receiver = operation >= 3U ? 0U : 1U;
            configure(hardware, fake, hooks, receiver);
            fake.fail_control_once = failure.first;
            Error result = Error::OK;
            switch (operation) {
            case 0U:
                LibusbW3u3HardwareTestPeer::require_initialization(hardware);
                result = hardware.open_receiver(receiver).error();
                break;
            case 1U:
                result = hardware.tune_terrestrial(receiver, 557142U, 1000U).error();
                break;
            case 2U:
                result = hardware.is_locked(receiver, System::ISDB_T).error();
                break;
            case 3U:
                result = hardware.tune_satellite(receiver, 1049480U, 1000U).error();
                break;
            case 4U:
                result = hardware.is_locked(receiver, System::ISDB_S).error();
                break;
            case 5U:
                result = hardware.select_satellite_slot(receiver, 0U, 1000U).error();
                break;
            case 6U:
                result = hardware.select_satellite_tsid(receiver, 0x1234U, 1000U).error();
                break;
            case 7U:
                result = hardware.begin_tune_power(receiver, System::ISDB_S, 15U).error();
                break;
            case 8U: {
                fake.fail_control_once = 0;
                fake.fail_submit_number = 1;
                fake.fail_submit_error = failure.first;
                const std::atomic<bool> cancelled{false};
                result = hardware.prepare(receiver, System::ISDB_S, cancelled).error();
                break;
            }
            }
            CHECK(result == failure.second);
        }
    }
    return true;
}

bool test_initialization_deadline_without_usb_error_is_timeout()
{
    FakeUsb fake;
    LibusbW3u3Hardware hardware(nullptr, {}, {}, {}, {});
    Hooks hooks{};
    configure(hardware, fake, hooks, 1U);
    hooks.now = FakeUsb::now_hook;
    fake.expire_after_power_write = true;
    LibusbW3u3HardwareTestPeer::require_initialization(hardware);
    CHECK(hardware.open_receiver(1U).error() == Error::TIMEOUT);
    CHECK(fake.fake_elapsed_ms == 20001U);
    CHECK(!LibusbW3u3HardwareTestPeer::gpio_snapshot_valid(hardware));
    return true;
}

bool test_short_revision_transfer_is_protocol_error()
{
    FakeUsb fake;
    LibusbW3u3Hardware hardware(nullptr, {}, {}, {}, {});
    Hooks hooks{};
    configure(hardware, fake, hooks, 1U);
    fake.short_control_once = true;
    LibusbW3u3HardwareTestPeer::require_initialization(hardware);
    CHECK(hardware.open_receiver(1U).error() == Error::PROTOCOL_ERROR);
    return true;
}

bool test_final_ready_read_keeps_bounded_selection_budget()
{
    {
        FakeUsb fake;
        LibusbW3u3Hardware hardware(nullptr, {}, {}, {}, {});
        Hooks hooks{};
        configure(hardware, fake, hooks, 0U);
        hooks.now = FakeUsb::now_hook;
        fake.missing_tsid_reads = 1U;
        fake.expire_after_tsid_read_ms = 1000U;
        CHECK(hardware.select_satellite_tsid(0U, 0U, 1000U).has_value());
        CHECK(std::count_if(
                  fake.controls.begin(), fake.controls.end(), [](const ControlEvent& event) {
                      return event.request == Request::I2cRead && (event.value >> 8U) == 0xceU;
                  }) == 2);
    }
    for (const unsigned elapsed : {999U, 1000U, 1001U}) {
        FakeUsb fake;
        LibusbW3u3Hardware hardware(nullptr, {}, {}, {}, {});
        Hooks hooks{};
        configure(hardware, fake, hooks, 0U);
        hooks.now = FakeUsb::now_hook;
        fake.expire_after_tsid_read_ms = elapsed;
        CHECK(hardware.select_satellite_tsid(0U, 0U, 1000U).has_value());
        CHECK(fake.fake_elapsed_ms == elapsed);
        CHECK(
            std::any_of(fake.controls.begin(), fake.controls.end(), [](const ControlEvent& event) {
                return event.request == Request::I2cWrite && (event.value >> 8U) == 0x8fU;
            }));
    }
    return true;
}

bool test_resubmit_disconnect_is_terminal_disconnect()
{
    FakeUsb fake;
    LibusbW3u3Hardware hardware(nullptr, {}, {}, {}, {});
    Hooks hooks{};
    configure(hardware, fake, hooks, 1U);
    const std::atomic<bool> cancelled{false};
    CHECK(hardware.prepare(1U, System::ISDB_T, cancelled).has_value());
    fake.fail_submit_number = 5;
    fake.fail_submit_error = LIBUSB_ERROR_NO_DEVICE;
    auto* completed = fake.submitted[0U];
    completed->status = LIBUSB_TRANSFER_COMPLETED;
    completed->actual_length = 0;
    completed->callback(completed);
    CHECK(hardware.run(cancelled, discard_packets, nullptr) == CaptureRunResult::disconnected);
    return true;
}

bool test_completion_order_and_length_contract()
{
    {
        FakeUsb fake;
        LibusbW3u3Hardware hardware(nullptr, {}, {}, {}, {});
        Hooks hooks{};
        configure(hardware, fake, hooks, 1U);
        std::atomic<bool> cancelled{false};
        CHECK(hardware.prepare(1U, System::ISDB_T, cancelled).has_value());
        auto* first = fake.submitted[0U];
        auto* second = fake.submitted[1U];
        second->status = LIBUSB_TRANSFER_ERROR;
        second->actual_length = 8;
        second->callback(second);
        first->status = LIBUSB_TRANSFER_COMPLETED;
        first->actual_length = 0;
        first->callback(first);
        CHECK(hardware.run(cancelled, discard_packets, nullptr) == CaptureRunResult::usb_error);
        CHECK(fake.submit_count == 5);
        CHECK(hardware.source_counters().empty_intervals == 1U);
        CHECK(fake.free_count == 4);
    }
    for (const int actual : {-1, 4097}) {
        FakeUsb fake;
        LibusbW3u3Hardware hardware(nullptr, {}, {}, {}, {});
        Hooks hooks{};
        configure(hardware, fake, hooks, 1U);
        std::atomic<bool> cancelled{false};
        CHECK(hardware.prepare(1U, System::ISDB_T, cancelled).has_value());
        auto* transfer = fake.submitted[0U];
        transfer->status = LIBUSB_TRANSFER_COMPLETED;
        transfer->actual_length = actual;
        transfer->callback(transfer);
        CHECK(hardware.run(cancelled, discard_packets, nullptr) == CaptureRunResult::usb_error);
        CHECK(fake.submit_count == 4);
        CHECK(fake.free_count == 4);
    }
    return true;
}

bool test_callback_quarantine_retains_storage_after_owner_destruction()
{
    FakeUsb fake;
    fake.suppress_cancel_callbacks = true;
    {
        LibusbW3u3Hardware hardware(nullptr, {}, {}, {}, {});
        Hooks hooks{};
        configure(hardware, fake, hooks, 1U);
        std::atomic<bool> cancelled{false};
        CHECK(hardware.prepare(1U, System::ISDB_T, cancelled).has_value());
        cancelled.store(true);
        const auto start = std::chrono::steady_clock::now();
        CHECK(hardware.run(cancelled, discard_packets, nullptr) == CaptureRunResult::fatal_drain);
        CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(10));
        CHECK(fake.free_count == 0);
        CHECK(hardware.stop().error() == Error::USB_IO);
        CHECK(hardware.shutdown().error() == Error::USB_IO);
    }
    CHECK(fake.free_count == 0);
    for (auto* transfer : fake.submitted) {
        CHECK(transfer != nullptr);
        transfer->status = LIBUSB_TRANSFER_CANCELLED;
        transfer->actual_length = 0;
        transfer->callback(transfer);
    }
    // Quarantined allocations deliberately remain retained until process exit.
    return true;
}

bool test_primary_prepare_stop_and_receiver1_reacquisition()
{
    FakeUsb fake;
    LibusbW3u3Hardware hardware(nullptr, {}, {}, {}, {});
    Hooks hooks{};
    configure(hardware, fake, hooks, 0U);
    CHECK(hardware.begin_tune_power(0U, System::ISDB_S, 15U).has_value());
    CHECK(hardware.commit_tune_power(0U).has_value());
    CHECK((fake.gpio & 0x20U) == 0U);

    std::atomic<bool> cancelled{false};
    CHECK(hardware.prepare(0U, System::ISDB_S, cancelled).has_value());
    CHECK((fake.gpio & 0x20U) == 0U);
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
    CHECK((fake.gpio & 0x20U) == 0U);  // STOP_STREAM retains committed lease power.
    CHECK(hardware.close_receiver(0U).has_value());
    CHECK((fake.gpio & 0x20U) != 0U);
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

bool test_partial_submit_rolls_back_local0_and_releases_lane()
{
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

bool test_dsc_failure_stops_local0_and_restores_cf()
{
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

bool test_seed_failure_stops_local0_clears_seed_and_restores_cf()
{
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

bool test_rejected_open_reserves_before_cleanup_state_and_quarantines()
{
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

bool test_shutdown_preserves_gpio_restore_after_nested_cf_cleanup_failure()
{
    FakeUsb fake;
    LibusbW3u3Hardware hardware(nullptr, {}, {}, {}, {});
    Hooks hooks{};
    configure(hardware, fake, hooks, 1U);
    LibusbW3u3HardwareTestPeer::set_gpio_snapshot(hardware, fake.gpio);
    fake.fail_cf_restore_once = true;
    std::atomic<bool> cancelled{false};
    CHECK(hardware.prepare(1U, System::ISDB_T, cancelled).has_value());

    const auto stopped = hardware.shutdown();

    CHECK(!stopped.has_value());
    CHECK(stopped.error() == Error::USB_IO);
    CHECK(fake.dsc_stop_count > 0);
    CHECK(fake.controls_at_restore > 0U);
    CHECK(fake.gpio_write_count > 0);
    CHECK(fake.gpio_read_count > 0);
    CHECK(fake.gpio == 0xa5U);
    return true;
}

bool test_card_control_can_deliver_bulk_callback_and_failed_cleanup_stops_capture()
{
    FakeUsb fake;
    fake.require_callbacks_before_free = true;
    LibusbW3u3Hardware hardware(nullptr, {}, {}, {}, {});
    Hooks hooks{};
    configure(hardware, fake, hooks, 1U);
    std::atomic<bool> cancelled{false};
    CHECK(hardware.prepare(1U, System::ISDB_T, cancelled).has_value());

    CaptureRunResult capture_result = CaptureRunResult::cancelled;
    std::thread capture(
        [&] { capture_result = hardware.run(cancelled, discard_packets, nullptr); });
    for (unsigned attempt = 0; attempt < 100U && fake.pump_count.load() == 0U; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool pump_started = fake.pump_count.load() != 0U;
    const bool operation_started = LibusbW3u3HardwareTestPeer::begin_card_operation(hardware);
    int control_result = LIBUSB_ERROR_IO;
    bool bulk_progress_while_gate_held = false;
    if (operation_started) {
        fake.complete_inside_control.store(true);
        std::array<unsigned char, 2> response{};
        const ControlTransfer card_read{1U, Request::I2cRead, 0x004aU, 0U, 2U, Direction::In, 500U};
        control_result =
            LibusbW3u3HardwareTestPeer::card_control(hardware, card_read, response.data());
        const auto progress_deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        while (fake.submit_count.load() < 5 &&
               std::chrono::steady_clock::now() < progress_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        bulk_progress_while_gate_held = fake.submit_count.load() >= 5;
        LibusbW3u3HardwareTestPeer::end_card_operation(hardware);
    }
    const bool cleanup_started = LibusbW3u3HardwareTestPeer::begin_card_cleanup(hardware);
    if (cleanup_started) {
        LibusbW3u3HardwareTestPeer::end_card_cleanup(hardware, false);
    } else {
        LibusbW3u3HardwareTestPeer::set_cleanup_failed(hardware);
    }
    capture.join();

    CHECK(pump_started);
    CHECK(operation_started);
    CHECK(control_result == 2);
    CHECK(bulk_progress_while_gate_held);
    CHECK(fake.callback_count.load() == 1U);
    CHECK(cleanup_started);
    CHECK(capture_result == CaptureRunResult::usb_error);
    CHECK(fake.cancel_count == 3 || fake.cancel_count == 4);
    CHECK(fake.free_count == 4);
    CHECK(fake.callback_total.load() >= 4U);
    CHECK(fake.free_before_callbacks == 0);
    CHECK(fake.dsc_stop_count == 1);
    CHECK(fake.controller[0x05] == 0U);
    return true;
}

bool test_failed_card_cleanup_before_capture_worker_starts_still_drains()
{
    FakeUsb fake;
    fake.require_callbacks_before_free = true;
    LibusbW3u3Hardware hardware(nullptr, {}, {}, {}, {});
    Hooks hooks{};
    configure(hardware, fake, hooks, 1U);
    std::atomic<bool> cancelled{false};
    CHECK(hardware.prepare(1U, System::ISDB_T, cancelled).has_value());
    CHECK(LibusbW3u3HardwareTestPeer::begin_card_cleanup(hardware));
    LibusbW3u3HardwareTestPeer::end_card_cleanup(hardware, false);

    CHECK(hardware.run(cancelled, discard_packets, nullptr) == CaptureRunResult::usb_error);
    CHECK(fake.dsc_stop_count == 1);
    CHECK(fake.cancel_count == 4);
    CHECK(fake.free_count == 4);
    CHECK(fake.callback_total.load() == 4U);
    CHECK(fake.free_before_callbacks == 0);
    CHECK(fake.controller[0x05] == 0U);
    return true;
}

bool test_prepare_waiter_rechecks_quarantine_after_control_gate()
{
    FakeUsb fake;
    LibusbW3u3Hardware hardware(nullptr, {}, {}, {}, {});
    Hooks hooks{};
    configure(hardware, fake, hooks, 1U);
    CHECK(LibusbW3u3HardwareTestPeer::begin_card_cleanup(hardware));
    const std::size_t controls_before = fake.controls.size();
    std::atomic<bool> started{false};
    std::atomic<bool> prepare_succeeded{false};
    std::atomic<int> prepare_error{static_cast<int>(Error::OK)};
    std::atomic<bool> cancelled{false};
    std::thread waiter([&] {
        started.store(true, std::memory_order_release);
        const auto result = hardware.prepare(1U, System::ISDB_T, cancelled);
        prepare_succeeded.store(result.has_value(), std::memory_order_release);
        if (!result) {
            prepare_error.store(static_cast<int>(result.error()), std::memory_order_release);
        }
    });
    while (!started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    LibusbW3u3HardwareTestPeer::end_card_cleanup(hardware, false);
    waiter.join();

    CHECK(!prepare_succeeded.load());
    CHECK(prepare_error.load() == static_cast<int>(Error::USB_IO) ||
          prepare_error.load() == static_cast<int>(Error::TIMEOUT));
    CHECK(fake.controls.size() == controls_before);
    CHECK(fake.submit_count.load() == 0);
    return true;
}

bool test_satellite_slot_wait_uses_deadline_including_gate_wait()
{
    struct GateWaitClock {
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        unsigned elapsed_ms = 0U;
        unsigned reads = 0U;
        std::mutex mutex;
        std::condition_variable changed;
        bool gate_wait_entered = false;
        bool advance_allowed = false;
        std::vector<ControlTransfer> controls;

        static std::chrono::steady_clock::time_point now(void* context)
        {
            auto& clock = *static_cast<GateWaitClock*>(context);
            std::unique_lock<std::mutex> lock(clock.mutex);
            // The first read establishes the operation's absolute deadline.
            // Pause its first gate attempt until the test chooses whether to
            // release the gate or let the deadline expire while still held.
            if (clock.reads++ == 0U) {
                return clock.start;
            }
            clock.gate_wait_entered = true;
            clock.changed.notify_one();
            clock.changed.wait(lock, [&] { return clock.advance_allowed; });
            return clock.start + std::chrono::milliseconds(clock.elapsed_ms);
        }

        static int control(void* context, const ControlTransfer& transfer, unsigned char*)
        {
            auto& clock = *static_cast<GateWaitClock*>(context);
            clock.controls.push_back(transfer);
            // Stop at the first USB request: only its remaining timeout budget
            // matters here, not TSID polling or real wall-clock delays.
            return LIBUSB_ERROR_TIMEOUT;
        }
    };

    for (const unsigned elapsed_ms : {70U, 100U, 125U}) {
        FakeUsb fake;
        LibusbW3u3Hardware hardware(nullptr, {}, {}, {}, {});
        Hooks hooks{};
        configure(hardware, fake, hooks, 0U);
        CHECK(LibusbW3u3HardwareTestPeer::begin_card_operation(hardware));

        GateWaitClock clock;
        clock.elapsed_ms = elapsed_ms;
        hooks = {};
        hooks.context = &clock;
        hooks.now = GateWaitClock::now;
        hooks.control = GateWaitClock::control;
        Error result = Error::OK;
        std::thread waiter([&] {
            const auto selected = hardware.select_satellite_slot(0U, 0U, 100U);
            result = selected ? Error::OK : selected.error();
        });
        {
            std::unique_lock<std::mutex> lock(clock.mutex);
            clock.changed.wait(lock, [&] { return clock.gate_wait_entered; });
        }
        const bool expired_while_held = elapsed_ms >= 100U;
        if (!expired_while_held) {
            LibusbW3u3HardwareTestPeer::end_card_operation(hardware);
        }
        {
            std::lock_guard<std::mutex> lock(clock.mutex);
            clock.advance_allowed = true;
        }
        clock.changed.notify_one();
        waiter.join();
        if (expired_while_held) {
            LibusbW3u3HardwareTestPeer::end_card_operation(hardware);
        }
        hooks = fake.hooks();

        if (expired_while_held) {
            CHECK(result == Error::TIMEOUT);
            CHECK(clock.controls.empty());
        } else {
            CHECK(result == Error::TIMEOUT);  // Preserve the fake USB timeout.
            CHECK(clock.controls.size() == 1U);
            CHECK(clock.controls.front().request == Request::I2cRead);
            CHECK(clock.controls.front().timeout_ms == 30U);
        }
    }
    return true;
}

bool saw_gpio_mask(const FakeUsb& fake, std::uint8_t mask)
{
    return std::any_of(fake.controls.begin(), fake.controls.end(),
                       [mask](const ControlEvent& event) {
                           return event.request == Request::Gpio &&
                                  (static_cast<std::uint8_t>(event.value >> 8U) & mask) != 0U;
                       });
}

bool saw_demod_write(const FakeUsb& fake, std::uint8_t reg, std::uint8_t value)
{
    return std::any_of(fake.controls.begin(), fake.controls.end(),
                       [reg, value](const ControlEvent& event) {
                           return (event.request == Request::I2cWrite ||
                                   event.request == Request::I2cWriteNoStop) &&
                                  event.value == static_cast<std::uint16_t>((reg << 8U) | 0x30U) &&
                                  static_cast<std::uint8_t>(event.index) == value;
                       });
}

bool test_unsupported_models_fail_before_any_usb_io()
{
    FakeUsb fake;
    Hooks hooks = fake.hooks();
    LibusbW3u3Hardware unknown(nullptr, {}, {}, {}, {});
    LibusbW3u3HardwareTestPeer::install_hooks_only(unknown, &hooks);
    CHECK(unknown.device_profile() == nullptr);
    CHECK(unknown.receiver_count() == 0U);
    CHECK(unknown.claim().error() == Error::UNSUPPORTED);
    CHECK(unknown.open_receiver(0U).error() == Error::UNSUPPORTED);
    const auto* v2_profile = find_profile(ModelId::W3u3V2);
    LibusbW3u3Hardware v2(nullptr, {}, {}, {}, {}, v2_profile);
    LibusbW3u3HardwareTestPeer::install_hooks_only(v2, &hooks);
    CHECK(v2.device_profile() == v2_profile && v2.receiver_count() == 2U);
    CHECK(v2.receiver_supports(0U, System::ISDB_S));
    CHECK(v2.claim().error() == Error::DISCONNECTED);
    CHECK(v2.open_receiver(0U).error() == Error::UNSUPPORTED);
    std::array<unsigned char, 1> data{};
    const ControlTransfer write{0U, Request::Gpio, 0x00ffU, 0U, 1U, Direction::In, 100U};
    CHECK(LibusbW3u3HardwareTestPeer::card_control(v2, write, data.data()) == LIBUSB_ERROR_ACCESS);
    CHECK(fake.controls.empty() && fake.submit_count == 0);
    return true;
}

bool test_w3u2_reuses_guarded_primary_path_and_reports_operational_capacity()
{
    FakeUsb fake;
    LibusbW3u3Hardware hardware(nullptr, {}, {}, {}, {}, find_profile(ModelId::W3u2));
    Hooks hooks{};
    configure(hardware, fake, hooks, 1U);
    CHECK(hardware.device_profile()->enclosure_receiver_count == 4U);
    CHECK(hardware.receiver_count() == 2U);
    CHECK(hardware.receiver_supports(0U, System::ISDB_S));
    CHECK(hardware.receiver_supports(1U, System::ISDB_T));
    CHECK(!hardware.receiver_supports(2U, System::ISDB_S));
    CHECK(!hardware.receiver_supports(3U, System::ISDB_T));
    CHECK(hardware.open_receiver(2U).error() == Error::UNSUPPORTED);
    CHECK(fake.controls.empty());
    LibusbW3u3HardwareTestPeer::require_initialization(hardware);
    CHECK(hardware.open_receiver(1U).has_value());
    CHECK(saw_gpio_mask(fake, 0x20U));  // Explicit OFF initialization is allowed.
    CHECK((fake.gpio & 0x20U) != 0U);
    for (const auto& control : fake.controls) {
        if (control.request == Request::Gpio && (control.value & 0x2000U) != 0U) {
            CHECK((control.value & 0x20U) != 0U);  // Never enable at startup.
        }
    }
    CHECK(fake.gpio_ex_write_count == 0);
    CHECK(hardware.tune_terrestrial(1U, 557142U, 3000U).has_value());
    std::atomic<bool> cancelled{false};
    CHECK(hardware.prepare(1U, System::ISDB_T, cancelled).has_value());
    CHECK(hardware.stop().has_value());
    CHECK(hardware.shutdown().has_value());
    CHECK(fake.cf[1] == fake.initial_cf(1U));
    CHECK(fake.gpio == 0xa5U);
    return true;
}

bool test_s3u_combined_receiver_uses_lane0_for_both_systems()
{
    FakeUsb fake;
    LibusbW3u3Hardware hardware(nullptr, {}, {}, {}, {}, find_profile(ModelId::S3u));
    Hooks hooks{};
    configure(hardware, fake, hooks, 0U);
    CHECK(hardware.receiver_count() == 1U);
    CHECK(hardware.receiver_supports(0U, System::ISDB_T));
    CHECK(hardware.receiver_supports(0U, System::ISDB_S));
    CHECK(!hardware.receiver_supports(1U, System::ISDB_T));
    CHECK(!hardware.requires_terrestrial_lock_settle());
    LibusbW3u3HardwareTestPeer::require_initialization(hardware);
    CHECK(hardware.open_receiver(0U).has_value());
    CHECK(saw_gpio_mask(fake, 0x20U));
    CHECK(fake.gpio_ex_write_count == 0);
    CHECK(hardware.begin_tune_power(0U, System::ISDB_T, 0U).has_value());
    CHECK(hardware.begin_tune_power(0U, System::ISDB_S, 15U).error() == Error::UNSUPPORTED);
    CHECK(hardware.tune_terrestrial(0U, 557142U, 3000U).has_value());
    CHECK(saw_demod_write(fake, 0x0fU, 0x14U));
    CHECK(!LibusbW3u3HardwareTestPeer::gain_applied(hardware));
    std::atomic<bool> cancelled{false};
    CHECK(hardware.prepare(0U, System::ISDB_T, cancelled).has_value());
    CHECK(LibusbW3u3HardwareTestPeer::gain_applied(hardware));
    CHECK(saw_dsc(fake, Request::DscStart, 0U));
    CHECK(hardware.stop().has_value());
    CHECK(fake.cf[0] == fake.initial_cf(0U));
    CHECK(!has_cf_request(fake, 1U, Request::ChannelFilterWrite));
    CHECK(hardware.tune_satellite(0U, 1049480U, 3000U).has_value());
    CHECK(!LibusbW3u3HardwareTestPeer::gain_applied(hardware));
    CHECK(saw_demod_write(fake, 0x0fU, 0x3cU));
    CHECK(hardware.prepare(0U, System::ISDB_S, cancelled).has_value());
    CHECK(hardware.stop().has_value());
    CHECK(std::all_of(fake.endpoints.begin(), fake.endpoints.end(),
                      [](std::uint8_t endpoint) { return endpoint == 0x81U; }));
    CHECK(hardware.shutdown().has_value());
    CHECK((fake.gpio & 0x08U) != 0U);
    CHECK(!LibusbW3u3HardwareTestPeer::gpio_snapshot_valid(hardware));
    return true;
}

bool test_s3u2_powers_off_gpioex_and_retains_single_capture_lease()
{
    FakeUsb fake;
    fake.gpio_ex_external_low = 0x01U;  // A released pin need not read high.
    LibusbW3u3Hardware hardware(nullptr, {}, {}, {}, {}, find_profile(ModelId::S3u2));
    Hooks hooks{};
    configure(hardware, fake, hooks, 1U);
    CHECK(hardware.receiver_count() == 2U);
    CHECK(!hardware.receiver_supports(0U, System::ISDB_T));
    CHECK(hardware.receiver_supports(1U, System::ISDB_T));
    LibusbW3u3HardwareTestPeer::require_initialization(hardware);
    CHECK(hardware.open_receiver(1U).has_value());
    CHECK((fake.gpio & 0x08U) == 0U);  // DTV_Init prelude clears board sleep.
    const auto prelude =
        std::find_if(fake.controls.begin(), fake.controls.end(), [](const ControlEvent& event) {
            return event.request == Request::Gpio && event.value == setup_word(0U, 0x08U);
        });
    CHECK(prelude != fake.controls.end());
    const auto powered_guard =
        std::find_if(fake.controls.begin(), fake.controls.end(), [](const ControlEvent& event) {
            return event.request == Request::I2cRead && event.value == 0x094aU;
        });
    CHECK(powered_guard != fake.controls.end() && prelude < powered_guard);
    CHECK(saw_gpio_mask(fake, 0x20U));
    CHECK(fake.gpio_ex_read_count == 0 && fake.gpio_ex_write_count == 4);
    CHECK(fake.gpio_ex == 0xa0U);
    CHECK(hardware.open_receiver(0U).error() == Error::BUSY);
    CHECK(hardware.tune_terrestrial(1U, 557142U, 3000U).has_value());
    std::atomic<bool> cancelled{false};
    CHECK(hardware.prepare(1U, System::ISDB_T, cancelled).has_value());
    CHECK(hardware.stop().has_value());
    CHECK(hardware.close_receiver(1U).has_value());
    CHECK(hardware.open_receiver(0U).has_value());
    CHECK(hardware.tune_satellite(0U, 1049480U, 3000U).has_value());
    CHECK(saw_demod_write(fake, 0x0fU, 0x34U));
    CHECK(hardware.prepare(0U, System::ISDB_S, cancelled).has_value());
    CHECK(hardware.stop().has_value());
    CHECK(fake.cf[0] == fake.initial_cf(0U) && fake.cf[1] == fake.initial_cf(1U));
    CHECK(hardware.shutdown().has_value());
    CHECK((fake.gpio & 0xecU) == 0xa8U && fake.gpio_ex == 0xa3U);
    CHECK(!LibusbW3u3HardwareTestPeer::gpio_snapshot_valid(hardware));
    return true;
}

bool test_legacy_controller_guard_runs_board_off_sequence()
{
    FakeUsb fake;
    fake.controller[0x09U] = 0x02U;  // Not the supported controller type0f.
    LibusbW3u3Hardware hardware(nullptr, {}, {}, {}, {}, find_profile(ModelId::S3u2));
    Hooks hooks{};
    configure(hardware, fake, hooks, 1U);
    LibusbW3u3HardwareTestPeer::require_initialization(hardware);
    CHECK(hardware.open_receiver(1U).error() == Error::UNSUPPORTED);
    CHECK(fake.gpio_ex_write_count == 6);
    CHECK((fake.gpio & 0xecU) == 0xa8U && fake.gpio_ex == 0xa3U);
    CHECK(!saw_demod_write(fake, 0x0fU, 0x34U));
    CHECK(!LibusbW3u3HardwareTestPeer::gpio_snapshot_valid(hardware));
    return true;
}

bool test_legacy_gpioex_off_failure_is_visible_and_quarantines()
{
    FakeUsb fake;
    LibusbW3u3Hardware hardware(nullptr, {}, {}, {}, {}, find_profile(ModelId::S3u2));
    Hooks hooks{};
    configure(hardware, fake, hooks, 1U);
    LibusbW3u3HardwareTestPeer::require_initialization(hardware);
    CHECK(hardware.open_receiver(1U).has_value());
    fake.fail_gpio_ex_off_once = true;
    CHECK(hardware.shutdown().error() == Error::USB_IO);
    CHECK((fake.gpio & 0xecU) == 0xa8U && fake.gpio_ex == 0xa1U);
    CHECK(LibusbW3u3HardwareTestPeer::gpio_snapshot_valid(hardware));
    const auto before = fake.controls.size();
    CHECK(hardware.tune_terrestrial(1U, 557142U, 3000U).error() == Error::USB_IO);
    CHECK(fake.controls.size() == before);
    return true;
}

bool test_legacy_capture_cleanup_failure_still_powers_off_gpioex()
{
    FakeUsb fake;
    LibusbW3u3Hardware hardware(nullptr, {}, {}, {}, {}, find_profile(ModelId::S3u2));
    Hooks hooks{};
    configure(hardware, fake, hooks, 1U);
    LibusbW3u3HardwareTestPeer::require_initialization(hardware);
    CHECK(hardware.open_receiver(1U).has_value());
    CHECK(hardware.tune_terrestrial(1U, 557142U, 3000U).has_value());
    std::atomic<bool> cancelled{false};
    CHECK(hardware.prepare(1U, System::ISDB_T, cancelled).has_value());
    fake.fail_cf_restore_once = true;
    CHECK(hardware.shutdown().error() == Error::USB_IO);
    CHECK(fake.cancel_count == 4 && fake.free_count == 4);
    CHECK((fake.gpio & 0xecU) == 0xa8U && fake.gpio_ex == 0xa3U);
    CHECK(fake.controls_at_restore > 0U);
    return true;
}

bool test_legacy_gain_runs_once_per_terrestrial_tune()
{
    FakeUsb fake;
    LibusbW3u3Hardware hardware(nullptr, {}, {}, {}, {}, find_profile(ModelId::S3u2));
    Hooks hooks{};
    configure(hardware, fake, hooks, 1U);
    CHECK(hardware.tune_terrestrial(1U, 557142U, 3000U).has_value());
    const auto plan = plan_legacy_default_gain(LegacyFrontendProfile::S3u2, true);
    CHECK(!plan.empty());
    const auto transfer = plan.front().transfer;
    const auto count_gain = [&] {
        return std::count_if(
            fake.controls.begin(), fake.controls.end(), [&](const ControlEvent& event) {
                return event.request == transfer.request && event.value == transfer.value &&
                       event.index == transfer.index;
            });
    };
    const auto before = count_gain();
    std::atomic<bool> cancelled{false};
    CHECK(hardware.prepare(1U, System::ISDB_T, cancelled).has_value());
    CHECK(count_gain() == before + 1);
    CHECK(hardware.stop().has_value());
    CHECK(hardware.prepare(1U, System::ISDB_T, cancelled).has_value());
    CHECK(count_gain() == before + 1);
    CHECK(hardware.stop().has_value());
    CHECK(hardware.tune_terrestrial(1U, 563142U, 3000U).has_value());
    const auto before_retuned = count_gain();
    CHECK(hardware.prepare(1U, System::ISDB_T, cancelled).has_value());
    CHECK(count_gain() == before_retuned + 1);
    CHECK(hardware.stop().has_value());
    return true;
}

bool test_all_source_terrestrial_channels_and_invalid_requests()
{
    for (const auto model : {ModelId::S3u, ModelId::S3u2, ModelId::W3u2, ModelId::W3u3}) {
        FakeUsb fake;
        LibusbW3u3Hardware hardware(nullptr, {}, {}, {}, {}, find_profile(model));
        Hooks hooks{};
        const std::uint8_t receiver = model == ModelId::S3u ? 0U : 1U;
        configure(hardware, fake, hooks, receiver);
        for (const std::uint32_t frequency : {473142U, 557142U, 707142U, 767142U}) {
            CHECK(hardware.tune_terrestrial(receiver, frequency, 3000U).has_value());
        }
        const auto before = fake.controls.size();
        CHECK(hardware.tune_terrestrial(receiver, 0U, 3000U).error() == Error::INVALID_ARGUMENT);
        CHECK(hardware.tune_terrestrial(receiver, 0xffffffffU, 3000U).error() ==
              Error::INVALID_ARGUMENT);
        CHECK(hardware.tune_terrestrial(receiver, 473142U, 0U).error() == Error::INVALID_ARGUMENT);
        CHECK(fake.controls.size() == before);
    }
    return true;
}

bool test_v2_master_routing_tune_capture_and_tsid()
{
    FakeUsb fake;
    LibusbW3u3Hardware hardware(nullptr, {}, {}, {}, {}, find_profile(ModelId::W3u3V2));
    Hooks hooks{};
    configure(hardware, fake, hooks, 1U);
    CHECK(hardware.receiver_count() == 2U &&
          hardware.device_profile()->enclosure_receiver_count == 4U);
    CHECK(LibusbW3u3HardwareTestPeer::verify_v2_roles(hardware));
    LibusbW3u3HardwareTestPeer::require_initialization(hardware);
    CHECK(hardware.open_receiver(1U).has_value());
    CHECK(LibusbW3u3HardwareTestPeer::v2_identity_verified(hardware));
    CHECK((fake.gpio & 0x20U) == 0U);  // Active-high V2 must start OFF.
    for (const auto& control : fake.controls) {
        if (control.request == Request::Gpio && (control.value & 0x2000U) != 0U) {
            CHECK((control.value & 0x20U) == 0U);
        }
    }
    CHECK(!hardware.requires_terrestrial_lock_settle());
    CHECK(hardware.tune_terrestrial(1U, 473142U, 3000U).has_value());
    CHECK(hardware.is_locked(1U, System::ISDB_T).value());
    std::atomic<bool> cancelled{false};
    CHECK(hardware.prepare(1U, System::ISDB_T, cancelled).has_value());
    CHECK(fake.controller[0x05U] == 0xa0U);
    CHECK(hardware.stop().has_value());
    CHECK(hardware.close_receiver(1U).has_value());
    CHECK(hardware.open_receiver(0U).has_value());
    CHECK(hardware.tune_satellite(0U, 1049480U, 3000U).has_value());
    CHECK(hardware.is_locked(0U, System::ISDB_S).value());
    CHECK(hardware.select_satellite_slot(0U, 0U, 1000U).has_value());
    CHECK((fake.v2_registers[FakeUsb::V2Key{0x22U, false, 0x8fU}] == 0x40U));
    CHECK((fake.v2_registers[FakeUsb::V2Key{0x22U, false, 0x90U}] == 0x10U));
    fake.v2_registers[FakeUsb::V2Key{0x22U, false, 0xceU}] = 0U;
    fake.v2_registers[FakeUsb::V2Key{0x22U, false, 0xcfU}] = 0U;
    CHECK(hardware.select_satellite_tsid(0U, 0U, 1000U).has_value());
    CHECK((fake.v2_registers[FakeUsb::V2Key{0x22U, false, 0x8fU}] == 0U));
    CHECK((fake.v2_registers[FakeUsb::V2Key{0x22U, false, 0x90U}] == 0U));
    CHECK(hardware.prepare(0U, System::ISDB_S, cancelled).has_value());
    CHECK(hardware.stop().has_value());
    CHECK(hardware.shutdown().has_value());
    CHECK((fake.gpio & 0x48U) == 0x48U && fake.gpio_ex_write_count == 0);
    CHECK(fake.cf[0] == fake.initial_cf(0U) && fake.cf[1] == fake.initial_cf(1U));
    CHECK(fake.controller[0x05U] == 0U);
    for (std::size_t i = 0; i < fake.controls.size(); ++i) {
        if (fake.control_functions[i] == 0U) {
            continue;
        }
        CHECK(fake.controls[i].request == Request::CustomerInfo ||
              fake.controls[i].request == Request::SysCtrlRead ||
              (fake.controls[i].request == Request::I2cRead && fake.controls[i].value == 0xb0a8U));
    }
    CHECK(std::count(fake.endpoints.begin(), fake.endpoints.end(), 0x81U) == 4);
    CHECK(std::count(fake.endpoints.begin(), fake.endpoints.end(), 0x82U) == 4);
    return true;
}

bool test_v2_identity_mismatch_powers_off_before_rf_or_link_writes()
{
    FakeUsb fake;
    fake.identity_mismatch = true;
    LibusbW3u3Hardware hardware(nullptr, {}, {}, {}, {}, find_profile(ModelId::W3u3V2));
    Hooks hooks{};
    configure(hardware, fake, hooks, 1U);
    CHECK(LibusbW3u3HardwareTestPeer::verify_v2_roles(hardware));
    LibusbW3u3HardwareTestPeer::require_initialization(hardware);
    CHECK(!hardware.open_receiver(1U).has_value());
    CHECK(!LibusbW3u3HardwareTestPeer::v2_identity_verified(hardware));
    CHECK((fake.gpio & 0x48U) == 0x48U && fake.seed_write_count == 0 && fake.submit_count == 0);
    CHECK(std::none_of(fake.controls.begin(), fake.controls.end(), [](const ControlEvent& event) {
        return event.request == Request::I2cBufferFill || event.request == Request::I2cBufferSend;
    }));
    return true;
}

bool test_v2_duplicate_roles_and_revision16_fail_before_board_writes()
{
    FakeUsb fake;
    fake.role_fields[1] = 0U;
    LibusbW3u3Hardware hardware(nullptr, {}, {}, {}, {}, find_profile(ModelId::W3u3V2));
    Hooks hooks{};
    configure(hardware, fake, hooks, 1U);
    CHECK(!LibusbW3u3HardwareTestPeer::verify_v2_roles(hardware));
    LibusbW3u3HardwareTestPeer::require_initialization(hardware);
    CHECK(hardware.open_receiver(1U).error() == Error::UNSUPPORTED);
    CHECK(fake.gpio_write_count == 0 && fake.seed_write_count == 0);
    fake.role_fields[1] = 0x80U;
    fake.sibling_revision = 0x16U;
    CHECK(!LibusbW3u3HardwareTestPeer::verify_v2_roles(hardware));
    CHECK(fake.gpio_write_count == 0 && fake.seed_write_count == 0);
    fake.sibling_revision = 0x11U;
    CHECK(LibusbW3u3HardwareTestPeer::verify_v2_roles(hardware));
    fake.revision = 0x16U;
    CHECK(hardware.open_receiver(1U).error() == Error::UNSUPPORTED);
    CHECK(fake.gpio_write_count == 0 && fake.seed_write_count == 0);
    return true;
}

bool test_legacy_revision_guard_precedes_any_board_write()
{
    FakeUsb fake;
    fake.revision = 0x16U;
    LibusbW3u3Hardware hardware(nullptr, {}, {}, {}, {}, find_profile(ModelId::S3u));
    Hooks hooks{};
    configure(hardware, fake, hooks, 0U);
    LibusbW3u3HardwareTestPeer::require_initialization(hardware);
    CHECK(hardware.open_receiver(0U).error() == Error::UNSUPPORTED);
    CHECK(fake.gpio_write_count == 0 && fake.gpio_ex_write_count == 0);
    CHECK(fake.controls.size() == 1U && fake.controls[0].request == Request::SysCtrlRead);
    return true;
}

}  // namespace

int main()
{
    struct TestCase {
        const char* name;
        bool (*run)();
    };
    const TestCase tests[] = {
        {"test_final_ready_read_keeps_bounded_selection_budget",
         test_final_ready_read_keeps_bounded_selection_budget},
        {"test_resubmit_disconnect_is_terminal_disconnect",
         test_resubmit_disconnect_is_terminal_disconnect},
        {"test_short_revision_transfer_is_protocol_error",
         test_short_revision_transfer_is_protocol_error},
        {"test_initialization_deadline_without_usb_error_is_timeout",
         test_initialization_deadline_without_usb_error_is_timeout},
        {"test_transport_errors_keep_their_public_class",
         test_transport_errors_keep_their_public_class},
        {"test_completion_order_and_length_contract", test_completion_order_and_length_contract},
        {"test_callback_quarantine_retains_storage_after_owner_destruction",
         test_callback_quarantine_retains_storage_after_owner_destruction},
        {"test_unsupported_models_fail_before_any_usb_io",
         test_unsupported_models_fail_before_any_usb_io},
        {"test_w3u2_reuses_guarded_primary_path_and_reports_operational_capacity",
         test_w3u2_reuses_guarded_primary_path_and_reports_operational_capacity},
        {"test_s3u_combined_receiver_uses_lane0_for_both_systems",
         test_s3u_combined_receiver_uses_lane0_for_both_systems},
        {"test_s3u2_powers_off_gpioex_and_retains_single_capture_lease",
         test_s3u2_powers_off_gpioex_and_retains_single_capture_lease},
        {"test_legacy_controller_guard_runs_board_off_sequence",
         test_legacy_controller_guard_runs_board_off_sequence},
        {"test_legacy_gpioex_off_failure_is_visible_and_quarantines",
         test_legacy_gpioex_off_failure_is_visible_and_quarantines},
        {"test_legacy_revision_guard_precedes_any_board_write",
         test_legacy_revision_guard_precedes_any_board_write},
        {"test_legacy_capture_cleanup_failure_still_powers_off_gpioex",
         test_legacy_capture_cleanup_failure_still_powers_off_gpioex},
        {"test_legacy_gain_runs_once_per_terrestrial_tune",
         test_legacy_gain_runs_once_per_terrestrial_tune},
        {"test_all_source_terrestrial_channels_and_invalid_requests",
         test_all_source_terrestrial_channels_and_invalid_requests},
        {"test_v2_master_routing_tune_capture_and_tsid",
         test_v2_master_routing_tune_capture_and_tsid},
        {"test_v2_identity_mismatch_powers_off_before_rf_or_link_writes",
         test_v2_identity_mismatch_powers_off_before_rf_or_link_writes},
        {"test_v2_duplicate_roles_and_revision16_fail_before_board_writes",
         test_v2_duplicate_roles_and_revision16_fail_before_board_writes},
        {"test_primary_prepare_stop_and_receiver1_reacquisition",
         test_primary_prepare_stop_and_receiver1_reacquisition},
        {"test_partial_submit_rolls_back_local0_and_releases_lane",
         test_partial_submit_rolls_back_local0_and_releases_lane},
        {"test_dsc_failure_stops_local0_and_restores_cf",
         test_dsc_failure_stops_local0_and_restores_cf},
        {"test_seed_failure_stops_local0_clears_seed_and_restores_cf",
         test_seed_failure_stops_local0_clears_seed_and_restores_cf},
        {"test_rejected_open_reserves_before_cleanup_state_and_quarantines",
         test_rejected_open_reserves_before_cleanup_state_and_quarantines},
        {"test_shutdown_preserves_gpio_restore_after_nested_cf_cleanup_failure",
         test_shutdown_preserves_gpio_restore_after_nested_cf_cleanup_failure},
        {"test_card_control_can_deliver_bulk_callback_and_failed_cleanup_stops_capture",
         test_card_control_can_deliver_bulk_callback_and_failed_cleanup_stops_capture},
        {"test_failed_card_cleanup_before_capture_worker_starts_still_drains",
         test_failed_card_cleanup_before_capture_worker_starts_still_drains},
        {"test_prepare_waiter_rechecks_quarantine_after_control_gate",
         test_prepare_waiter_rechecks_quarantine_after_control_gate},
        {"test_satellite_slot_wait_uses_deadline_including_gate_wait",
         test_satellite_slot_wait_uses_deadline_including_gate_wait},
    };
    for (const auto& test : tests) {
        std::fprintf(stderr, "[ RUN      ] %s\n", test.name);
        std::fflush(stderr);
        const auto start = std::chrono::steady_clock::now();
        const bool passed = test.run();
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - start)
                                    .count();
        std::fprintf(stderr, "[ %s ] %s (%lld ms)\n", passed ? "      OK" : "  FAILED", test.name,
                     static_cast<long long>(elapsed_ms));
        std::fflush(stderr);
        if (!passed) {
            return 1;
        }
    }
    return 0;
}
