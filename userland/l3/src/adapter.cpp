// SPDX-License-Identifier: GPL-2.0-or-later
#include "adapter.h"

#include "asicen/enclosure.h"

#include <algorithm>
#include <cstring>

namespace asicen::l3 {
namespace {

constexpr std::uint32_t kDefaultTuneTimeoutMs = 30000U;

bool has_system(std::uint32_t mask, std::uint32_t system) noexcept
{
    return (mask & system) != 0U;
}

} // namespace

Adapter::Adapter(const DeviceProfile& profile, Host& host) : profile_(&profile), host_(host)
{
    fill_capability(profile, &capability_);
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

    provider_.provider_context = this;
    provider_.claim = &Adapter::claim_thunk;
    provider_.release_claim = &Adapter::release_claim_thunk;
    provider_.submit = &Adapter::submit_thunk;
    provider_.cancel = &Adapter::cancel_thunk;
    provider_.scope_query = &Adapter::scope_query_thunk;
    provider_.bridge_start = &Adapter::bridge_start_thunk;
    provider_.bridge_stop = &Adapter::bridge_stop_thunk;
    provider_.stream_attach = &Adapter::stream_attach_thunk;
    provider_.stream_stop = &Adapter::stream_stop_thunk;
    provider_.tune = &Adapter::tune_thunk;
    provider_.claim_cancel = nullptr;
    provider_.cancel_scope = DTV_L3_CANCEL_SCOPE_TRANSFER;
    provider_.logger.fn = nullptr;
    provider_.logger.userdata = nullptr;
    provider_.logger.min_level = DTV_L3_LOG_ERROR;
}

void fill_capability(const DeviceProfile& profile, asicen_l3_capability* capability)
{
    std::memset(capability, 0, sizeof(*capability));
    capability->struct_size = static_cast<std::uint32_t>(sizeof(*capability));
    capability->model = static_cast<std::uint32_t>(profile.model_id);
    capability->model_key = profile.model_key;
    capability->vid = profile.vid;
    capability->pid = profile.pid;
    capability->enclosure_receivers = profile.enclosure_receiver_count;
    const bool multi_function = profile.expected_runtime_functions > 1U;
    capability->public_receivers =
        multi_function ? profile.enclosure_receiver_count
                       : (profile.combined_isdb_ts ? 1U : profile.enclosure_receiver_count);
    capability->usb_functions = profile.expected_runtime_functions;
    capability->local_lanes = profile.local_lane_count;
    capability->combined_isdb_ts = profile.combined_isdb_ts ? 1U : 0U;
    capability->readers = 1U;
    capability->loader_firmware = 1U;
    capability->multi_endpoint = multi_function ? 1U : 0U;
    // Not measured. The existing capture service must be the source of truth
    // before a concurrent-stream limit is published.
    capability->max_concurrent_streams = 0U;

    const bool model_lnb = profile.model_id == ModelId::W3u2 || profile.model_id == ModelId::W3u3 ||
                           profile.model_id == ModelId::W3u3V2;
    for (std::uint32_t receiver = 0U; receiver < profile.enclosure_receiver_count; ++receiver)
    {
        const auto address = receiver_address(profile, receiver);
        if (!address)
        {
            continue;
        }
        const std::uint32_t system = address.value().system == BroadcastSystem::IsdbS
                                         ? ASICEN_L3_SYSTEM_SATELLITE
                                         : ASICEN_L3_SYSTEM_TERRESTRIAL;
        if (profile.combined_isdb_ts)
        {
            // One combined receiver serves both systems even though the
            // internal lane address names only one.
            capability->receiver_systems[receiver] =
                ASICEN_L3_SYSTEM_TERRESTRIAL | ASICEN_L3_SYSTEM_SATELLITE;
        } else
        {
            capability->receiver_systems[receiver] |= system;
        }
        if (model_lnb && system == ASICEN_L3_SYSTEM_SATELLITE)
        {
            capability->lnb_receiver_mask |= 1U << receiver;
        }
    }
}

std::uint32_t Adapter::claim_count() const noexcept
{
    return profile_->expected_runtime_functions;
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
    return static_cast<int>(capability_.receiver_systems[receiver]);
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
        // A combined T/S receiver has no single default in the candidate
        // provider contract; the caller must name the system.
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
    return reader_id < capability_.readers ? host_.reader_open(reader_id) : DTV_L3_UNSUPPORTED;
}

dtv_l3_result Adapter::reader_reset(std::uint32_t reader_id)
{
    return reader_id < capability_.readers ? host_.reader_reset(reader_id) : DTV_L3_UNSUPPORTED;
}

dtv_l3_result Adapter::reader_transmit(std::uint32_t reader_id, const std::uint8_t* command,
                                       std::size_t command_size, std::uint8_t* response,
                                       std::size_t response_capacity, std::size_t* response_size)
{
    if (reader_id >= capability_.readers || command == nullptr || command_size == 0U ||
        response_size == nullptr)
    {
        return DTV_L3_INVALID;
    }
    return host_.reader_transmit(reader_id, command, command_size, response, response_capacity,
                                 response_size);
}

dtv_l3_result Adapter::reader_close(std::uint32_t reader_id)
{
    return reader_id < capability_.readers ? host_.reader_close(reader_id) : DTV_L3_UNSUPPORTED;
}

dtv_l3_result Adapter::lnb_set(std::uint32_t receiver, std::uint32_t mode)
{
    if (receiver >= profile_->enclosure_receiver_count ||
        (capability_.lnb_receiver_mask & (1U << receiver)) == 0U)
    {
        return DTV_L3_UNSUPPORTED;
    }
    return host_.lnb_set(receiver, mode);
}

dtv_l3_claim_result Adapter::claim_thunk(void* context, dtv_l3_provider_binding*,
                                         std::uint32_t index)
{
    return static_cast<Adapter*>(context)->host_.claim(index);
}

dtv_l3_release_result Adapter::release_claim_thunk(void* context, dtv_l3_provider_binding*,
                                                   std::uint32_t index)
{
    return static_cast<Adapter*>(context)->host_.release_claim(index);
}

dtv_l3_result Adapter::submit_thunk(void* context, dtv_l3_provider_binding*,
                                    std::uint64_t transfer_id,
                                    const dtv_l3_transfer_request* request,
                                    dtv_l3_transfer_complete_fn complete, void* callback_context)
{
    if (request == nullptr)
    {
        return DTV_L3_INVALID;
    }
    return static_cast<Adapter*>(context)->host_.submit(transfer_id, *request, complete,
                                                        callback_context);
}

dtv_l3_result Adapter::cancel_thunk(void* context, dtv_l3_provider_binding*,
                                    std::uint64_t transfer_id)
{
    return static_cast<Adapter*>(context)->host_.cancel(transfer_id);
}

dtv_l3_result Adapter::scope_query_thunk(void* context, std::uint32_t receiver,
                                         dtv_l3_scope_info* scope)
{
    if (scope == nullptr)
    {
        return DTV_L3_INVALID;
    }
    // rf_function_index is the existing shared RF scope. Distinct local lanes
    // on one function remain separate receivers with their own tune revision.
    asicen_l3_route route{};
    auto* adapter = static_cast<Adapter*>(context);
    if (!adapter->route(receiver, &route))
    {
        return DTV_L3_UNSUPPORTED;
    }
    scope->bridge_id = route.rf_function_index;
    scope->compatibility = DTV_L3_LEASE_SHAREABLE;
    return DTV_L3_OK;
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
    if (adapter->capability_.combined_isdb_ts != 0U)
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
    adapter->active_receiver_ = receiver_id;
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
            // stream_attach carries no receiver or bridge identity; the
            // candidate provider needs that input added before this can be
            // made race-free.
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

dtv_l3_result Adapter::tune_thunk(void* context, dtv_l3_provider_binding*,
                                  std::uint32_t receiver_id, std::uint64_t requested_hz,
                                  std::uint64_t* programmed_hz)
{
    auto* adapter = static_cast<Adapter*>(context);
    std::lock_guard<std::mutex> lock(adapter->mutex_);
    return adapter->tune_locked(receiver_id, 0U, requested_hz, kDefaultTuneTimeoutMs,
                                programmed_hz);
}

} // namespace asicen::l3
