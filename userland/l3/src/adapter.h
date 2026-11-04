// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef ASICEN_USERLAND_L3_ADAPTER_H
#define ASICEN_USERLAND_L3_ADAPTER_H

#include "asicen/l3.h"

#include "asicen/device_profile.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <string>
#include <utility>
#include <vector>

namespace asicen::l3 {

void fill_capabilities(const DeviceProfile& profile, dtv_l3_capabilities* capabilities);

// Product-local configuration seam used before a product-owned stream IN path
// starts (for example, telling the real backend which receiver/system feeds the
// provider submit). It is not a Core API.
class StreamConfig {
  public:
    virtual ~StreamConfig() = default;
    virtual dtv_l3_result configure(std::uint32_t receiver, std::uint32_t system) = 0;
    virtual dtv_l3_result stop()
    {
        return DTV_L3_OK;
    }
};

// Product-owned operations supplied by the existing backend. Core's USB-only
// provider is separate and is never called from here.
class Host {
  public:
    virtual ~Host() = default;
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
    Adapter(const DeviceProfile& profile, Host& host,
            StreamConfig* stream_config = nullptr);
    ~Adapter();

    Adapter(const Adapter&) = delete;
    Adapter& operator=(const Adapter&) = delete;

    const DeviceProfile& profile() const noexcept
    {
        return *profile_;
    }
    const dtv_l3_capabilities& capabilities() const noexcept
    {
        return capabilities_;
    }
    const dtv_l3_product_adapter* product_adapter() const noexcept
    {
        return &adapter_;
    }
    bool route(std::uint32_t receiver, asicen_l3_route* out) const noexcept;
    dtv_l3_result prepare_firmware(const dtv_l3_firmware* firmware);
    void set_power_permission(std::uint32_t permission) noexcept
    {
        power_permission_ = permission;
    }
    dtv_l3_result bind(dtv_l3_context* owner);
    dtv_l3_result initialize(dtv_l3_context* owner);

    // Explicit-system product tune used directly by ASICEN callers/tests. Core's
    // candidate tune callback has no system argument, so a combined S3U
    // receiver must use this path or a future Core field.
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
    struct ReaderToken {
        std::uint32_t reader_id;
    };

    static dtv_l3_result bind_thunk(void* context, dtv_l3_context* owner);
    static dtv_l3_result initialize_thunk(void* context, dtv_l3_context* owner);
    static dtv_l3_result scope_query_thunk(void* context, std::uint32_t receiver,
                                           dtv_l3_scope_info* scope);
    static dtv_l3_result tune_thunk(void* context, dtv_l3_provider_binding* binding,
                                    std::uint32_t receiver_id, std::uint64_t requested_hz,
                                    std::uint64_t* programmed_hz);
    static dtv_l3_result tune_ex_thunk(void* context, dtv_l3_provider_binding* binding,
                                       std::uint32_t receiver_id,
                                       const dtv_l3_tune_request* request,
                                       std::uint64_t* programmed_hz);
    static dtv_l3_result bridge_start_thunk(void* context, dtv_l3_provider_binding* binding,
                                            std::uint32_t bridge_id, std::uint32_t receiver_id);
    static dtv_l3_result bridge_stop_thunk(void* context, dtv_l3_provider_binding* binding,
                                           std::uint32_t bridge_id);
    static dtv_l3_result stream_attach_thunk(void* context, dtv_l3_provider_binding* binding,
                                             std::uint64_t stream_id, dtv_l3_ts_deliver_fn deliver,
                                             void* callback_context);
    static dtv_l3_result stream_stop_thunk(void* context, dtv_l3_provider_binding* binding,
                                           std::uint64_t stream_id);
    static dtv_l3_result stream_attach_ex_thunk(void* context, dtv_l3_provider_binding* binding,
                                                std::uint64_t stream_id, std::uint32_t receiver_id,
                                                std::uint32_t bridge_id,
                                                dtv_l3_ts_deliver_fn deliver,
                                                void* callback_context);
    static dtv_l3_result stream_stop_ex_thunk(void* context, dtv_l3_provider_binding* binding,
                                              std::uint64_t stream_id, std::uint32_t receiver_id,
                                              std::uint32_t bridge_id);

    void stream_loop();
    void fill_catalog();
    dtv_l3_result tune_locked(std::uint32_t receiver, std::uint32_t system,
                              std::uint64_t frequency_hz, std::uint32_t timeout_ms,
                              std::uint64_t* programmed_hz);
    bool bridge_matches(const asicen_l3_route& route, std::uint32_t bridge_id) const noexcept;
    int system_for_receiver(std::uint32_t receiver) const noexcept;
    std::uint32_t stream_system_for(std::uint32_t receiver,
                                    const asicen_l3_route& route) const noexcept;

    const DeviceProfile* profile_;
    Host& host_;
    StreamConfig* stream_config_ = nullptr;
    dtv_l3_capabilities capabilities_{};
    asicen_l3_route routes_[4]{};
    dtv_l3_product_adapter adapter_{};
    std::vector<std::uint8_t> firmware_bytes_;
    std::string firmware_path_;
    dtv_l3_firmware firmware_view_{};
    dtv_l3_firmware_manifest manifest_{};
    std::uint8_t manifest_sha_[32]{};
    ReaderToken reader_token_{};
    dtv_l3_receiver_capability receiver_caps_[4]{};
    dtv_l3_reader_capability reader_caps_[1]{};
    dtv_l3_lnb_scope_capability lnb_caps_[4]{};
    dtv_l3_shared_scope_capability shared_caps_[2]{};
    dtv_l3_capability_catalog catalog_{};
    dtv_l3_context* owner_ = nullptr;
    std::uint32_t power_permission_ = 1U;
    std::uint32_t tuned_system_[4]{};
    std::thread stream_worker_;
    std::atomic<bool> stream_stop_{false};
    std::atomic<bool> stream_running_{false};
    std::uint32_t stream_claim_index_ = 0U;
    dtv_l3_ts_deliver_fn stream_deliver_ = nullptr;
    void* stream_deliver_context_ = nullptr;
    mutable std::mutex mutex_;
    std::deque<PendingAttach> pending_attaches_;
    std::deque<std::pair<std::uint64_t, std::uint32_t>> streams_;
};

class Factory final {
  public:
    Factory(const DeviceProfile& profile, Host& host,
            StreamConfig* stream_config = nullptr);

    Factory(const Factory&) = delete;
    Factory& operator=(const Factory&) = delete;

    const dtv_l3_adapter_factory* core_factory() const noexcept
    {
        return &factory_;
    }
    const dtv_l3_factory* core_factory_v1() const noexcept
    {
        return &factory_v1_;
    }
    std::uint32_t claim_count() const noexcept;
    bool route(std::uint32_t receiver, asicen_l3_route* out) const noexcept;
    const DeviceProfile& profile() const noexcept
    {
        return *profile_;
    }

  private:
    static dtv_l3_result create_thunk(void* context, const dtv_l3_open_config* config,
                                      const dtv_l3_product_adapter** adapter_out,
                                      void** adapter_owner_out);
    static void release_thunk(void* owner);
    static dtv_l3_result create_v1_thunk(void* context, const dtv_l3_open_args* args,
                                         const dtv_l3_product_adapter** adapter_out,
                                         std::uint32_t* claim_count_out, void** adapter_owner_out);
    dtv_l3_result create_v1(const dtv_l3_open_args* args,
                            const dtv_l3_product_adapter** adapter_out,
                            std::uint32_t* claim_count_out, void** adapter_owner_out);

    const DeviceProfile* profile_;
    Host& host_;
    StreamConfig* stream_config_ = nullptr;
    dtv_l3_adapter_factory factory_{};
    dtv_l3_factory factory_v1_{};
};

} // namespace asicen::l3

#endif // ASICEN_USERLAND_L3_ADAPTER_H
