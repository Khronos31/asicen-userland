// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef ASICEN_USERLAND_L3_LIBUSB_HOST_H
#define ASICEN_USERLAND_L3_LIBUSB_HOST_H

#include "adapter.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace asicen {

class LibusbW3u3Hardware;

}

namespace asicen::l3 {

// Product-owned semantics over the existing enclosure backend.
class LibusbProductHost final : public Host {
  public:
    explicit LibusbProductHost(LibusbW3u3Hardware& backend) : backend_(backend)
    {
    }

    dtv_l3_result tune(std::uint32_t receiver, std::uint32_t system, std::uint64_t frequency_hz,
                       std::uint32_t timeout_ms, std::uint64_t* programmed_hz) override;
    dtv_l3_result reader_open(std::uint32_t reader_id) override;
    dtv_l3_result reader_reset(std::uint32_t reader_id) override;
    dtv_l3_result reader_transmit(std::uint32_t reader_id, const std::uint8_t* command,
                                  std::size_t command_size, std::uint8_t* response,
                                  std::size_t response_capacity,
                                  std::size_t* response_size) override;
    dtv_l3_result reader_close(std::uint32_t reader_id) override;

  private:
    LibusbW3u3Hardware& backend_;
};

// Product-internal mapping of the owner stream IN transfer to the existing
// capture path. One submit performs prepare/run for exactly one framed chunk,
// then stops/drains the capture; the completion carries that chunk.
class LibusbStreamSubmit : public StreamConfig {
  public:
    explicit LibusbStreamSubmit(LibusbW3u3Hardware& backend) : backend_(backend)
    {
    }

    dtv_l3_result configure(std::uint32_t receiver, std::uint32_t system) override;
    dtv_l3_result stop() override;
    dtv_l3_result submit(dtv_l3_provider_binding* binding, std::uint64_t transfer_id,
                         const dtv_l3_transfer_request* request,
                         dtv_l3_transfer_complete_fn complete, void* callback_context);
    dtv_l3_result cancel(std::uint64_t transfer_id);

  protected:
    // Test seam: called after prepare and before the capture run. Production
    // leaves it empty; the real backend fixture uses it to complete a URB.
    virtual void after_prepare()
    {
    }

  private:
    struct Pending {
        dtv_l3_provider_binding* binding = nullptr;
        std::uint64_t id = 0U;
        dtv_l3_transfer_complete_fn complete = nullptr;
        void* callback_context = nullptr;
    };

    void capture_loop();
    void fail_pending_locked(dtv_l3_result result);
    static bool emit_thunk(void* context, const std::uint8_t* data, std::size_t size) noexcept;

    LibusbW3u3Hardware& backend_;
    std::atomic<std::uint32_t> receiver_{0U};
    std::atomic<std::uint32_t> system_{0U};
    std::atomic<bool> configured_{false};
    std::atomic<bool> cancelled_{false};
    std::mutex mutex_;
    std::deque<Pending> pending_;
    std::deque<std::vector<std::uint8_t>> packets_;
    std::thread capture_;
    bool started_ = false;
    bool done_ = false;
    dtv_l3_result capture_result_ = DTV_L3_OK;
};

// USB-only provider over the current enclosure claim. Control transfers use
// the product-internal single-control seam; bulk stream IN delegates to
// LibusbStreamSubmit when one is attached to the factory.
class LibusbUsbProvider final {
  public:
    explicit LibusbUsbProvider(LibusbW3u3Hardware& backend, LibusbStreamSubmit* stream = nullptr)
        : backend_(backend), stream_(stream)
    {
        provider_.provider_context = this;
        provider_.claim = &LibusbUsbProvider::claim_thunk;
        provider_.release_claim = &LibusbUsbProvider::release_claim_thunk;
        provider_.submit = &LibusbUsbProvider::submit_thunk;
        provider_.cancel = &LibusbUsbProvider::cancel_thunk;
        provider_.claim_cancel = nullptr;
        provider_.cancel_scope = DTV_L3_CANCEL_SCOPE_TRANSFER;
        provider_.logger.fn = nullptr;
        provider_.logger.userdata = nullptr;
        provider_.logger.min_level = DTV_L3_LOG_ERROR;
    }

    const dtv_l3_usb_provider* provider() const noexcept
    {
        return &provider_;
    }

  private:
    static dtv_l3_claim_result claim_thunk(void* context, dtv_l3_provider_binding* binding,
                                           std::uint32_t claim_index);
    static dtv_l3_release_result
    release_claim_thunk(void* context, dtv_l3_provider_binding* binding, std::uint32_t claim_index);
    static dtv_l3_result submit_thunk(void* context, dtv_l3_provider_binding* binding,
                                      std::uint64_t transfer_id,
                                      const dtv_l3_transfer_request* request,
                                      dtv_l3_transfer_complete_fn complete, void* callback_context);
    static dtv_l3_result cancel_thunk(void* context, dtv_l3_provider_binding* binding,
                                      std::uint64_t transfer_id);

    LibusbW3u3Hardware& backend_;
    LibusbStreamSubmit* stream_ = nullptr;
    dtv_l3_usb_provider provider_{};
    bool enclosure_claimed_ = false;
};

} // namespace asicen::l3

#endif // ASICEN_USERLAND_L3_LIBUSB_HOST_H
