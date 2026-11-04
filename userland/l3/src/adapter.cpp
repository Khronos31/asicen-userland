// SPDX-License-Identifier: GPL-2.0-or-later
#include "adapter.h"

#include "asicen/enclosure.h"

#include <algorithm>
#include <cstring>
#include <memory>

namespace asicen::l3 {
namespace {

constexpr std::uint32_t kDefaultTuneTimeoutMs = 30000U;

bool has_system(std::uint32_t mask, std::uint32_t system) noexcept
{
    return (mask & system) != 0U;
}

} // namespace

void fill_capabilities(const DeviceProfile& profile, dtv_l3_capabilities* capabilities)
{
    std::memset(capabilities, 0, sizeof(*capabilities));
    const bool multi_function = profile.expected_runtime_functions > 1U;
    capabilities->receiver_count =
        multi_function ? profile.enclosure_receiver_count
                       : (profile.combined_isdb_ts ? 1U : profile.enclosure_receiver_count);
    capabilities->reader_count = 1U;
    const bool model_lnb = profile.model_id == ModelId::W3u2 || profile.model_id == ModelId::W3u3 ||
                           profile.model_id == ModelId::W3u3V2;
    capabilities->lnb_power = model_lnb ? 1U : 0U;
    capabilities->serial = 0U;
    capabilities->cancel_scope = DTV_L3_CANCEL_SCOPE_TRANSFER;
}

Adapter::Adapter(const DeviceProfile& profile, Host& host) : profile_(&profile), host_(host)
{
    fill_capabilities(profile, &capabilities_);
    for (std::uint32_t receiver = 0U; receiver < profile.enclosure_receiver_count; ++receiver)
    {
        const auto address = receiver_address(profile, receiver);
        if (!address)
        {
            continue;
        }
        routes_[receiver].valid = 1U;
        routes_[receiver].function_index = address.value().function_index;
        routes_[receiver].local_lane = address.value().local_lane;
        routes_[receiver].rf_function_index = address.value().rf_function_index;
        routes_[receiver].frontend_source = address.value().frontend_source;
        routes_[receiver].system = address.value().system == BroadcastSystem::IsdbS
                                       ? ASICEN_L3_SYSTEM_SATELLITE
                                       : ASICEN_L3_SYSTEM_TERRESTRIAL;
    }

    reader_token_.reader_id = 0U;
    adapter_.product_context = this;
    adapter_.capabilities = capabilities_;
    adapter_.firmware = nullptr;
    adapter_.scope_query = &Adapter::scope_query_thunk;
    adapter_.tune = &Adapter::tune_thunk;
    adapter_.bridge_start = &Adapter::bridge_start_thunk;
    adapter_.bridge_stop = &Adapter::bridge_stop_thunk;
    adapter_.stream_attach = &Adapter::stream_attach_thunk;
    adapter_.stream_stop = &Adapter::stream_stop_thunk;
    adapter_.reader_acquire = &Adapter::reader_acquire_thunk;
    adapter_.lnb_set = &Adapter::lnb_set_thunk;
}

dtv_l3_result Adapter::prepare_firmware(const dtv_l3_firmware* firmware)
{
    if (firmware == nullptr || firmware->source == DTV_L3_FIRMWARE_NONE)
    {
        adapter_.firmware = nullptr;
        return DTV_L3_OK;
    }
    switch (firmware->source)
    {
    case DTV_L3_FIRMWARE_BYTES:
        if (firmware->bytes == nullptr || firmware->size == 0U)
        {
            return DTV_L3_INVALID;
        }
        firmware_bytes_.assign(firmware->bytes, firmware->bytes + firmware->size);
        break;
    case DTV_L3_FIRMWARE_PATH:
        if (firmware->path == nullptr || firmware->path[0] == '\0')
        {
            return DTV_L3_INVALID;
        }
        firmware_path_ = firmware->path;
        break;
    case DTV_L3_FIRMWARE_READ_CALLBACK: {
        if (firmware->read == nullptr)
        {
            return DTV_L3_INVALID;
        }
        const std::uint8_t* data = nullptr;
        std::size_t size = 0U;
        const auto read = firmware->read(firmware->read_context, &data, &size);
        if (read != DTV_L3_OK)
        {
            return read;
        }
        if (data == nullptr || size == 0U)
        {
            return DTV_L3_INVALID;
        }
        firmware_bytes_.assign(data, data + size);
        break;
    }
    default:
        return DTV_L3_INVALID;
    }

    firmware_view_ = *firmware;
    firmware_view_.read = nullptr;
    firmware_view_.read_context = nullptr;
    if (firmware->source == DTV_L3_FIRMWARE_PATH)
    {
        firmware_view_.path = firmware_path_.c_str();
        firmware_view_.bytes = nullptr;
        firmware_view_.size = 0U;
    } else
    {
        firmware_view_.path = nullptr;
        firmware_view_.bytes = firmware_bytes_.data();
        firmware_view_.size = firmware_bytes_.size();
    }
    adapter_.firmware = &firmware_view_;
    return DTV_L3_OK;
}

bool Adapter::route(std::uint32_t receiver, asicen_l3_route* out) const noexcept
{
    if (out == nullptr || receiver >= profile_->enclosure_receiver_count ||
        routes_[receiver].valid == 0U)
    {
        return false;
    }
    *out = routes_[receiver];
    return true;
}

bool Adapter::bridge_matches(const asicen_l3_route& route, std::uint32_t bridge_id) const noexcept
{
    return route.valid != 0U && route.rf_function_index == bridge_id;
}

int Adapter::system_for_receiver(std::uint32_t receiver) const noexcept
{
    if (receiver >= profile_->enclosure_receiver_count)
    {
        return 0;
    }
    std::uint32_t mask = 0U;
    const auto address = receiver_address(*profile_, receiver);
    if (!address)
    {
        return 0;
    }
    if (profile_->combined_isdb_ts)
    {
        mask = ASICEN_L3_SYSTEM_TERRESTRIAL | ASICEN_L3_SYSTEM_SATELLITE;
    } else
    {
        mask = address.value().system == BroadcastSystem::IsdbS ? ASICEN_L3_SYSTEM_SATELLITE
                                                                : ASICEN_L3_SYSTEM_TERRESTRIAL;
    }
    return static_cast<int>(mask);
}

dtv_l3_result Adapter::tune_locked(std::uint32_t receiver, std::uint32_t system,
                                   std::uint64_t frequency_hz, std::uint32_t timeout_ms,
                                   std::uint64_t* programmed_hz)
{
    if (programmed_hz == nullptr || frequency_hz == 0U)
    {
        return DTV_L3_INVALID;
    }
    const int supported = system_for_receiver(receiver);
    if (supported == 0)
    {
        return DTV_L3_UNSUPPORTED;
    }
    if (system == 0U)
    {
        if (supported != ASICEN_L3_SYSTEM_TERRESTRIAL && supported != ASICEN_L3_SYSTEM_SATELLITE)
        {
            return DTV_L3_UNSUPPORTED;
        }
        system = static_cast<std::uint32_t>(supported);
    }
    if (!has_system(static_cast<std::uint32_t>(supported), system))
    {
        return DTV_L3_UNSUPPORTED;
    }
    return host_.tune(receiver, system, frequency_hz, timeout_ms, programmed_hz);
}

dtv_l3_result Adapter::tune(std::uint32_t receiver, std::uint32_t system,
                            std::uint64_t frequency_hz, std::uint32_t timeout_ms,
                            std::uint64_t* programmed_hz)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return tune_locked(receiver, system, frequency_hz, timeout_ms, programmed_hz);
}

dtv_l3_result Adapter::reader_open(std::uint32_t reader_id)
{
    return reader_id < capabilities_.reader_count ? host_.reader_open(reader_id)
                                                  : DTV_L3_UNSUPPORTED;
}

dtv_l3_result Adapter::reader_reset(std::uint32_t reader_id)
{
    return reader_id < capabilities_.reader_count ? host_.reader_reset(reader_id)
                                                  : DTV_L3_UNSUPPORTED;
}

dtv_l3_result Adapter::reader_transmit(std::uint32_t reader_id, const std::uint8_t* command,
                                       std::size_t command_size, std::uint8_t* response,
                                       std::size_t response_capacity, std::size_t* response_size)
{
    if (reader_id >= capabilities_.reader_count || command == nullptr || command_size == 0U ||
        response_size == nullptr)
    {
        return DTV_L3_INVALID;
    }
    return host_.reader_transmit(reader_id, command, command_size, response, response_capacity,
                                 response_size);
}

dtv_l3_result Adapter::reader_close(std::uint32_t reader_id)
{
    return reader_id < capabilities_.reader_count ? host_.reader_close(reader_id)
                                                  : DTV_L3_UNSUPPORTED;
}

dtv_l3_result Adapter::lnb_set(std::uint32_t receiver, std::uint32_t mode)
{
    if (receiver >= profile_->enclosure_receiver_count)
    {
        return DTV_L3_UNSUPPORTED;
    }
    const auto address = receiver_address(*profile_, receiver);
    if (!address || address.value().system != BroadcastSystem::IsdbS ||
        profile_->model_id == ModelId::S3u || profile_->model_id == ModelId::S3u2)
    {
        return DTV_L3_UNSUPPORTED;
    }
    return host_.lnb_set(receiver, mode);
}

dtv_l3_result Adapter::scope_query_thunk(void* context, std::uint32_t receiver,
                                         dtv_l3_scope_info* scope)
{
    if (scope == nullptr)
    {
        return DTV_L3_INVALID;
    }
    auto* adapter = static_cast<Adapter*>(context);
    asicen_l3_route route{};
    if (!adapter->route(receiver, &route))
    {
        return DTV_L3_UNSUPPORTED;
    }
    scope->bridge_id = route.rf_function_index;
    scope->compatibility = DTV_L3_LEASE_SHAREABLE;
    return DTV_L3_OK;
}

dtv_l3_result Adapter::tune_thunk(void* context, dtv_l3_provider_binding*,
                                  std::uint32_t receiver_id, std::uint64_t requested_hz,
                                  std::uint64_t* programmed_hz)
{
    auto* adapter = static_cast<Adapter*>(context);
    std::lock_guard<std::mutex> lock(adapter->mutex_);
    return adapter->tune_locked(receiver_id, 0U, requested_hz, kDefaultTuneTimeoutMs,
                                programmed_hz);
}

dtv_l3_result Adapter::bridge_start_thunk(void* context, dtv_l3_provider_binding*,
                                          std::uint32_t bridge_id, std::uint32_t receiver_id)
{
    auto* adapter = static_cast<Adapter*>(context);
    asicen_l3_route route{};
    if (!adapter->route(receiver_id, &route) || !adapter->bridge_matches(route, bridge_id))
    {
        return DTV_L3_UNSUPPORTED;
    }
    if (adapter->profile_->combined_isdb_ts)
    {
        // The candidate bridge_start carries no system, so a combined T/S
        // receiver cannot choose a stream input here.
        return DTV_L3_UNSUPPORTED;
    }
    const auto started = adapter->host_.stream_begin(receiver_id, route.system, bridge_id);
    if (started != DTV_L3_OK)
    {
        return started;
    }
    std::lock_guard<std::mutex> lock(adapter->mutex_);
    adapter->pending_attaches_.push_back(PendingAttach{bridge_id, receiver_id});
    return DTV_L3_OK;
}

dtv_l3_result Adapter::bridge_stop_thunk(void* context, dtv_l3_provider_binding*,
                                         std::uint32_t bridge_id)
{
    auto* adapter = static_cast<Adapter*>(context);
    std::lock_guard<std::mutex> lock(adapter->mutex_);
    adapter->pending_attaches_.erase(std::remove_if(adapter->pending_attaches_.begin(),
                                                    adapter->pending_attaches_.end(),
                                                    [bridge_id](const PendingAttach& pending) {
                                                        return pending.bridge_id == bridge_id;
                                                    }),
                                     adapter->pending_attaches_.end());
    return DTV_L3_OK;
}

dtv_l3_result Adapter::stream_attach_thunk(void* context, dtv_l3_provider_binding*,
                                           std::uint64_t stream_id, dtv_l3_ts_deliver_fn deliver,
                                           void* callback_context)
{
    auto* adapter = static_cast<Adapter*>(context);
    PendingAttach pending{};
    {
        std::lock_guard<std::mutex> lock(adapter->mutex_);
        if (adapter->pending_attaches_.empty())
        {
            // Core's candidate stream_attach carries no receiver or bridge
            // identity; add one before this can be made race-free.
            return DTV_L3_FAILED;
        }
        pending = adapter->pending_attaches_.front();
        adapter->pending_attaches_.pop_front();
    }
    asicen_l3_route route{};
    if (!adapter->route(pending.receiver, &route))
    {
        return DTV_L3_UNSUPPORTED;
    }
    const auto attached = adapter->host_.stream_attach(stream_id, pending.receiver, route.system,
                                                       deliver, callback_context);
    if (attached == DTV_L3_OK)
    {
        std::lock_guard<std::mutex> lock(adapter->mutex_);
        adapter->streams_.emplace_back(stream_id, pending.receiver);
    }
    return attached;
}

dtv_l3_result Adapter::stream_stop_thunk(void* context, dtv_l3_provider_binding*,
                                         std::uint64_t stream_id)
{
    auto* adapter = static_cast<Adapter*>(context);
    {
        std::lock_guard<std::mutex> lock(adapter->mutex_);
        const auto found =
            std::find_if(adapter->streams_.begin(), adapter->streams_.end(),
                         [stream_id](const auto& entry) { return entry.first == stream_id; });
        if (found == adapter->streams_.end())
        {
            return DTV_L3_STALE;
        }
    }
    const auto stopped = adapter->host_.stream_stop(stream_id);
    if (stopped == DTV_L3_OK)
    {
        std::lock_guard<std::mutex> lock(adapter->mutex_);
        adapter->streams_.erase(
            std::remove_if(adapter->streams_.begin(), adapter->streams_.end(),
                           [stream_id](const auto& entry) { return entry.first == stream_id; }),
            adapter->streams_.end());
    }
    return stopped;
}

dtv_l3_result Adapter::reader_acquire_thunk(void* context, std::uint32_t reader_id,
                                            dtv_l3_reader** reader_out)
{
    auto* adapter = static_cast<Adapter*>(context);
    if (reader_out == nullptr)
    {
        return DTV_L3_INVALID;
    }
    *reader_out = nullptr;
    if (reader_id >= adapter->capabilities_.reader_count)
    {
        return DTV_L3_UNSUPPORTED;
    }
    const auto opened = adapter->host_.reader_open(reader_id);
    if (opened != DTV_L3_OK)
    {
        return opened;
    }
    adapter->reader_token_.reader_id = reader_id;
    *reader_out = reinterpret_cast<dtv_l3_reader*>(&adapter->reader_token_);
    return DTV_L3_OK;
}

dtv_l3_result Adapter::lnb_set_thunk(void* context, std::uint32_t receiver_id, std::uint32_t mode)
{
    return static_cast<Adapter*>(context)->lnb_set(receiver_id, mode);
}

Factory::Factory(const DeviceProfile& profile, Host& host) : profile_(&profile), host_(host)
{
    factory_.factory_context = this;
    factory_.create = &Factory::create_thunk;
    factory_.release = &Factory::release_thunk;
}

std::uint32_t Factory::claim_count() const noexcept
{
    return profile_->expected_runtime_functions;
}

bool Factory::route(std::uint32_t receiver, asicen_l3_route* out) const noexcept
{
    if (out == nullptr || receiver >= profile_->enclosure_receiver_count)
    {
        return false;
    }
    const auto address = receiver_address(*profile_, receiver);
    if (!address)
    {
        return false;
    }
    out->valid = 1U;
    out->function_index = address.value().function_index;
    out->local_lane = address.value().local_lane;
    out->rf_function_index = address.value().rf_function_index;
    out->frontend_source = address.value().frontend_source;
    out->system = address.value().system == BroadcastSystem::IsdbS ? ASICEN_L3_SYSTEM_SATELLITE
                                                                   : ASICEN_L3_SYSTEM_TERRESTRIAL;
    return true;
}

dtv_l3_result Factory::create_thunk(void* context, const dtv_l3_open_config* config,
                                    const dtv_l3_product_adapter** adapter_out,
                                    void** adapter_owner_out)
{
    auto* factory = static_cast<Factory*>(context);
    if (config == nullptr || adapter_out == nullptr || adapter_owner_out == nullptr)
    {
        return DTV_L3_INVALID;
    }
    *adapter_out = nullptr;
    *adapter_owner_out = nullptr;
    if (config->model != static_cast<std::uint32_t>(factory->profile_->model_id))
    {
        return DTV_L3_UNSUPPORTED;
    }
    auto adapter = std::make_unique<Adapter>(*factory->profile_, factory->host_);
    const auto firmware_result = adapter->prepare_firmware(config->firmware);
    if (firmware_result != DTV_L3_OK)
    {
        return firmware_result;
    }
    *adapter_out = adapter->product_adapter();
    *adapter_owner_out = adapter.release();
    return DTV_L3_OK;
}

void Factory::release_thunk(void* owner)
{
    delete static_cast<Adapter*>(owner);
}

} // namespace asicen::l3
