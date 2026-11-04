// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef ASICEN_USERLAND_L3_ADAPTER_H
#define ASICEN_USERLAND_L3_ADAPTER_H

#include "asicen/l3.h"

#include "asicen/device_profile.h"

#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>

namespace asicen::l3 {

void fill_capability(const DeviceProfile& profile, asicen_l3_capability* capability);

// Operations the existing backend already owns. Optional calls default to
// UNSUPPORTED so a fixture or a partial host never reports success for work it
// did not perform.
class Host {
  public:
    virtual ~Host() = default;
    virtual dtv_l3_claim_result claim(std::uint32_t claim_index) = 0;
    virtual dtv_l3_release_result release_claim(std::uint32_t claim_index) = 0;
    virtual dtv_l3_result submit(std::uint64_t transfer_id, const dtv_l3_transfer_request& request,
                                 dtv_l3_transfer_complete_fn complete, void* callback_context) = 0;
    virtual dtv_l3_result cancel(std::uint64_t transfer_id) = 0;
    virtual dtv_l3_result tune(std::uint32_t, std::uint32_t, std::uint64_t, std::uint32_t,
                               std::uint64_t*)
    {
        return DTV_L3_UNSUPPORTED;
    }
    virtual dtv_l3_result stream_begin(std::uint32_t, std::uint32_t, std::uint32_t)
    {
        return DTV_L3_UNSUPPORTED;
    }
    virtual dtv_l3_result stream_attach(std::uint64_t, std::uint32_t, std::uint32_t,
                                        dtv_l3_ts_deliver_fn, void*)
    {
        return DTV_L3_UNSUPPORTED;
    }
    virtual dtv_l3_result stream_stop(std::uint64_t)
    {
        return DTV_L3_UNSUPPORTED;
    }
    virtual dtv_l3_result reader_open(std::uint32_t)
    {
        return DTV_L3_UNSUPPORTED;
    }
    virtual dtv_l3_result reader_reset(std::uint32_t)
    {
        return DTV_L3_UNSUPPORTED;
    }
    virtual dtv_l3_result reader_transmit(std::uint32_t, const std::uint8_t*, std::size_t,
                                          std::uint8_t*, std::size_t, std::size_t*)
    {
        return DTV_L3_UNSUPPORTED;
    }
    virtual dtv_l3_result reader_close(std::uint32_t)
    {
        return DTV_L3_UNSUPPORTED;
    }
    virtual dtv_l3_result lnb_set(std::uint32_t, std::uint32_t)
    {
        return DTV_L3_UNSUPPORTED;
    }
};

class Adapter final {
  public:
    Adapter(const DeviceProfile& profile, Host& host);

    Adapter(const Adapter&) = delete;
    Adapter& operator=(const Adapter&) = delete;

    const DeviceProfile& profile() const noexcept
    {
        return *profile_;
    }
    const asicen_l3_capability& capability() const noexcept
    {
        return capability_;
    }
    const dtv_l3_provider* provider() const noexcept
    {
        return &provider_;
    }
    std::uint32_t claim_count() const noexcept;
    bool route(std::uint32_t receiver, asicen_l3_route* out) const noexcept;

    dtv_l3_result tune(std::uint32_t receiver, std::uint32_t system, std::uint64_t frequency_hz,
                       std::uint32_t timeout_ms, std::uint64_t* programmed_hz);
    dtv_l3_result reader_open(std::uint32_t reader_id);
    dtv_l3_result reader_reset(std::uint32_t reader_id);
    dtv_l3_result reader_transmit(std::uint32_t reader_id, const std::uint8_t* command,
                                  std::size_t command_size, std::uint8_t* response,
                                  std::size_t response_capacity, std::size_t* response_size);
    dtv_l3_result reader_close(std::uint32_t reader_id);
    dtv_l3_result lnb_set(std::uint32_t receiver, std::uint32_t mode);

  private:
    struct PendingAttach {
        std::uint32_t bridge_id;
        std::uint32_t receiver;
    };

    static dtv_l3_claim_result claim_thunk(void* context, dtv_l3_provider_binding* binding,
                                           std::uint32_t index);
    static dtv_l3_release_result
    release_claim_thunk(void* context, dtv_l3_provider_binding* binding, std::uint32_t index);
    static dtv_l3_result submit_thunk(void* context, dtv_l3_provider_binding* binding,
                                      std::uint64_t transfer_id,
                                      const dtv_l3_transfer_request* request,
                                      dtv_l3_transfer_complete_fn complete, void* callback_context);
    static dtv_l3_result cancel_thunk(void* context, dtv_l3_provider_binding* binding,
                                      std::uint64_t transfer_id);
    static dtv_l3_result scope_query_thunk(void* context, std::uint32_t receiver,
                                           dtv_l3_scope_info* scope);
    static dtv_l3_result bridge_start_thunk(void* context, dtv_l3_provider_binding* binding,
                                            std::uint32_t bridge_id, std::uint32_t receiver_id);
    static dtv_l3_result bridge_stop_thunk(void* context, dtv_l3_provider_binding* binding,
                                           std::uint32_t bridge_id);
    static dtv_l3_result stream_attach_thunk(void* context, dtv_l3_provider_binding* binding,
                                             std::uint64_t stream_id, dtv_l3_ts_deliver_fn deliver,
                                             void* callback_context);
    static dtv_l3_result stream_stop_thunk(void* context, dtv_l3_provider_binding* binding,
                                           std::uint64_t stream_id);
    static dtv_l3_result tune_thunk(void* context, dtv_l3_provider_binding* binding,
                                    std::uint32_t receiver_id, std::uint64_t requested_hz,
                                    std::uint64_t* programmed_hz);

    dtv_l3_result tune_locked(std::uint32_t receiver, std::uint32_t system,
                              std::uint64_t frequency_hz, std::uint32_t timeout_ms,
                              std::uint64_t* programmed_hz);
    bool bridge_matches(const asicen_l3_route& route, std::uint32_t bridge_id) const noexcept;
    int system_for_receiver(std::uint32_t receiver) const noexcept;

    const DeviceProfile* profile_;
    Host& host_;
    asicen_l3_capability capability_{};
    asicen_l3_route routes_[4]{};
    dtv_l3_provider provider_{};
    mutable std::mutex mutex_;
    std::deque<PendingAttach> pending_attaches_;
    std::deque<std::pair<std::uint64_t, std::uint32_t>> streams_;
    std::uint32_t active_receiver_ = 0xffffffffu;
};

} // namespace asicen::l3

#endif // ASICEN_USERLAND_L3_ADAPTER_H
