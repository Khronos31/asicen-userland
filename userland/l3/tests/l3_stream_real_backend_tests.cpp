// SPDX-License-Identifier: GPL-2.0-or-later
#define main asicen_lifecycle_tests_main
#include "libusb_capture_lifecycle_tests.cpp"
#undef main

#include "adapter.h"
#include "libusb_host.h"

#include "asicen/device_profile.h"

#include <cstdio>
#include <cstring>
#include <memory>

namespace {

using asicen::l3::Factory;
using asicen::l3::LibusbProductHost;
using asicen::l3::LibusbStreamSubmit;

struct TestStreamSubmit final : LibusbStreamSubmit {
    TestStreamSubmit(asicen::LibusbW3u3Hardware& hardware, FakeUsb& fake)
        : LibusbStreamSubmit(hardware), fake_(fake)
    {
    }

    void after_prepare() override
    {
        const std::size_t sizes[] = {100U, 600U, 500U, 680U};
        std::size_t offset = 0U;
        for (std::size_t index = 0U; index < fake_.submitted.size() && index < 4U; ++index)
        {
            auto* transfer = fake_.submitted[index];
            if (transfer == nullptr || transfer->buffer == nullptr)
            {
                continue;
            }
            const std::size_t length = sizes[index];
            for (std::size_t i = 0U; i < length; ++i)
            {
                const std::size_t position = offset + i;
                transfer->buffer[i] = position % 188U == 0U
                                          ? 0x47U
                                          : static_cast<std::uint8_t>(position / 188U & 0x7fU);
            }
            transfer->actual_length = static_cast<int>(length);
            transfer->status = LIBUSB_TRANSFER_COMPLETED;
            transfer->callback(transfer);
            fake_.callback_count.fetch_add(1U);
            fake_.callback_total.fetch_add(1U);
            offset += length;
        }
    }

  private:
    FakeUsb& fake_;
};

struct ProviderState {
    TestStreamSubmit* stream = nullptr;
};

dtv_l3_claim_result claim_ok(void*, dtv_l3_provider_binding*, std::uint32_t)
{
    return {DTV_L3_OK, DTV_L3_CLAIM_RETAINED};
}

dtv_l3_release_result release_ok(void*, dtv_l3_provider_binding*, std::uint32_t)
{
    return {DTV_L3_RELEASE_QUIESCED, DTV_L3_CLAIM_NONE, DTV_L3_RESTORE_NOT_ATTEMPTED, DTV_L3_OK};
}

dtv_l3_result submit_thunk(void* context, dtv_l3_provider_binding* binding,
                           std::uint64_t transfer_id, const dtv_l3_transfer_request* request,
                           dtv_l3_transfer_complete_fn complete, void* callback_context)
{
    auto* state = static_cast<ProviderState*>(context);
    return state->stream->submit(binding, transfer_id, request, complete, callback_context);
}

dtv_l3_result cancel_thunk(void* context, dtv_l3_provider_binding*, std::uint64_t transfer_id)
{
    auto* state = static_cast<ProviderState*>(context);
    return state->stream->cancel(transfer_id);
}

bool read_packets(dtv_l3_stream* stream, std::uint8_t* out, std::size_t out_size, std::size_t count)
{
    std::size_t received = 0U;
    for (int attempt = 0; attempt < 12000 && received < count; ++attempt)
    {
        std::size_t bytes = 0U;
        dtv_l3_read_state state = DTV_L3_READ_WAIT;
        if (dtv_l3_stream_read_ts(stream, out, out_size, &bytes, &state) == DTV_L3_OK &&
            state == DTV_L3_READ_DATA && bytes == out_size)
        {
            if (out[0] != 0x47U || out[1] != static_cast<std::uint8_t>(received & 0x7fU))
            {
                return false;
            }
            ++received;
            continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return received == count;
}

int real_backend_stream_path()
{
    FakeUsb fake;
    asicen::LibusbW3u3Hardware hardware(nullptr, {}, {}, {}, {});
    Hooks hooks{};
    configure(hardware, fake, hooks, 0U);

    auto stream = std::make_unique<TestStreamSubmit>(hardware, fake);
    LibusbProductHost host(hardware);
    const auto* profile = hardware.device_profile();
    Factory factory(*profile, host, stream.get());

    ProviderState state{stream.get()};
    dtv_l3_usb_provider usb{};
    usb.provider_context = &state;
    usb.claim = claim_ok;
    usb.release_claim = release_ok;
    usb.submit = submit_thunk;
    usb.cancel = cancel_thunk;
    usb.cancel_scope = DTV_L3_CANCEL_SCOPE_TRANSFER;
    usb.call_flags = 0U;

    dtv_l3_open_config config{};
    config.model = static_cast<std::uint32_t>(profile->model_id);
    config.selector = 0U;
    config.firmware = nullptr;
    auto opened = dtv_l3_open_factory(&usb, factory.core_factory(), &config, factory.claim_count());
    if (opened.result != DTV_L3_ACCEPTED || opened.owner == nullptr || opened.op == nullptr)
    {
        std::fprintf(stderr, "open failed: %d\n", static_cast<int>(opened.result));
        return 1;
    }
    if (dtv_l3_op_wait(opened.op, 5000U) != DTV_L3_OK || dtv_l3_op_release(opened.op) != DTV_L3_OK)
    {
        return 2;
    }
    dtv_l3_open_status open_status{};
    if (dtv_l3_open_status_get(opened.owner, &open_status) != DTV_L3_OK ||
        open_status.state != DTV_L3_OPEN_READY)
    {
        std::fprintf(stderr, "not ready: %d result=%d\n", static_cast<int>(open_status.state),
                     static_cast<int>(open_status.result));
        return 3;
    }

    dtv_l3_lease* lease = nullptr;
    dtv_l3_stream* stream_handle = nullptr;
    std::uint64_t programmed = 0U;
    std::uint64_t revision = 0U;
    if (dtv_l3_lease_acquire(opened.owner, 0U, &lease) != DTV_L3_OK)
    {
        return 4;
    }
    if (dtv_l3_lease_tune(lease, 1049480000ULL, &programmed, &revision) != DTV_L3_OK)
    {
        return 4;
    }
    if (dtv_l3_lease_start(lease) != DTV_L3_OK)
    {
        return 4;
    }
    if (dtv_l3_lease_attach(lease, &stream_handle) != DTV_L3_OK || stream_handle == nullptr)
    {
        return 4;
    }
    std::uint8_t packet[188]{};
    if (!read_packets(stream_handle, packet, sizeof(packet), 10U))
    {
        return 5;
    }
    if (dtv_l3_stream_stop(stream_handle, 2000U) != DTV_L3_OK ||
        dtv_l3_stream_release(stream_handle, DTV_L3_STREAM_RELEASE_DISCARD_UNREAD) != DTV_L3_OK ||
        dtv_l3_stream_destroy(stream_handle) != DTV_L3_OK ||
        dtv_l3_lease_release(lease) != DTV_L3_OK || dtv_l3_close(opened.owner, 2000U) != DTV_L3_OK)
    {
        return 6;
    }
    dtv_l3_close_status close_status{};
    for (int attempt = 0; attempt < 12000; ++attempt)
    {
        if (dtv_l3_close_status_get(opened.owner, &close_status) == DTV_L3_OK &&
            close_status.releasable != 0U)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (dtv_l3_context_release(opened.owner) != DTV_L3_OK)
    {
        return 7;
    }
    return 0;
}

} // namespace

int main()
{
    return real_backend_stream_path();
}
