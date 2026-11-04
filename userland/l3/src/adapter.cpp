// SPDX-License-Identifier: GPL-2.0-or-later
#include "adapter.h"

#include "asicen/enclosure.h"

#include "sha256.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>

namespace asicen::l3 {
namespace {

constexpr std::uint32_t kDefaultTuneTimeoutMs = 30000U;

bool has_system(std::uint32_t mask, std::uint32_t system) noexcept
{
    return (mask & system) != 0U;
}

bool firmware_matches_manifest(const dtv_l3_firmware* firmware, const dtv_l3_firmware* view)
{
    if (firmware == nullptr || firmware->manifest == nullptr)
    {
        return true;
    }
    if (view == nullptr)
    {
        return false;
    }
    const auto& manifest = *firmware->manifest;
    if (manifest.length_bytes != 0U && view->size != manifest.length_bytes)
    {
        return false;
    }
    if (manifest.sha256 != nullptr)
    {
        if (view->bytes == nullptr || view->size == 0U)
        {
            return false;
        }
        asicen::detail::Sha256 sha;
        sha.update(view->bytes, view->size);
        const auto digest = sha.finish();
        return std::memcmp(digest.data(), manifest.sha256, digest.size()) == 0;
    }
    return true;
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
    fill_catalog();
    adapter_.product_context = this;
    adapter_.capabilities = capabilities_;
    // Let Core choose its ~1 MiB default; the fixture only needs one packet.
    adapter_.stream_queue_packets = 0U;
    adapter_.catalog = &catalog_;
    adapter_.firmware = nullptr;
    adapter_.bind = &Adapter::bind_thunk;
    adapter_.initialize = &Adapter::initialize_thunk;
    adapter_.scope_query = &Adapter::scope_query_thunk;
    adapter_.tune = &Adapter::tune_thunk;
    adapter_.tune_ex = &Adapter::tune_ex_thunk;
    adapter_.bridge_start = &Adapter::bridge_start_thunk;
    adapter_.bridge_stop = &Adapter::bridge_stop_thunk;
    adapter_.stream_attach = &Adapter::stream_attach_thunk;
    adapter_.stream_attach_ex = &Adapter::stream_attach_ex_thunk;
    adapter_.stream_stop = &Adapter::stream_stop_thunk;
    adapter_.stream_stop_ex = &Adapter::stream_stop_ex_thunk;
}

Adapter::~Adapter()
{
    stream_stop_.store(true);
    if (stream_worker_.joinable())
    {
        stream_worker_.join();
    }
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
    if (firmware->manifest != nullptr)
    {
        manifest_ = *firmware->manifest;
        if (firmware->manifest->sha256 != nullptr)
        {
            std::memcpy(manifest_sha_, firmware->manifest->sha256, sizeof(manifest_sha_));
            manifest_.sha256 = manifest_sha_;
        }
        firmware_view_.manifest = &manifest_;
    } else
    {
        firmware_view_.manifest = nullptr;
    }
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

void Adapter::fill_catalog()
{
    const std::uint32_t receiver_count = capabilities_.receiver_count;
    for (std::uint32_t receiver = 0U; receiver < 4U; ++receiver)
    {
        receiver_caps_[receiver].id = receiver;
        receiver_caps_[receiver].systems = 0U;
        receiver_caps_[receiver].pid_filter = DTV_L3_PID_FILTER_NONE;
        receiver_caps_[receiver].support = DTV_L3_SUPPORT_UNKNOWN;
        if (receiver >= receiver_count)
        {
            continue;
        }
        const auto address = receiver_address(*profile_, receiver);
        if (!address)
        {
            continue;
        }
        if (profile_->combined_isdb_ts)
        {
            receiver_caps_[receiver].systems = DTV_L3_SYSTEM_ISDB_T | DTV_L3_SYSTEM_ISDB_S;
        } else
        {
            receiver_caps_[receiver].systems = address.value().system == BroadcastSystem::IsdbS
                                                   ? DTV_L3_SYSTEM_ISDB_S
                                                   : DTV_L3_SYSTEM_ISDB_T;
        }
        receiver_caps_[receiver].support = DTV_L3_SUPPORT_SUPPORTED;
    }

    reader_caps_[0].id = 0U;
    reader_caps_[0].external_apdu = DTV_L3_SUPPORT_SUPPORTED;
    reader_caps_[0].support = DTV_L3_SUPPORT_SUPPORTED;

    std::uint32_t lnb_count = 0U;
    for (std::uint32_t receiver = 0U; receiver < profile_->enclosure_receiver_count; ++receiver)
    {
        const auto address = receiver_address(*profile_, receiver);
        if (!address || address.value().system != BroadcastSystem::IsdbS)
        {
            continue;
        }
        if (profile_->model_id == ModelId::W3u2 || profile_->model_id == ModelId::W3u3 ||
            profile_->model_id == ModelId::W3u3V2)
        {
            lnb_caps_[lnb_count].id = receiver;
            lnb_caps_[lnb_count].support = DTV_L3_SUPPORT_SUPPORTED;
            ++lnb_count;
        }
    }
    if (lnb_count == 0U)
    {
        // S3U/S3U2 have no established model-specific LNB software control
        // in the existing backend; declare the difference instead of
        // generalizing every ASICEN model to "no LNB".
        lnb_caps_[0].id = 0U;
        lnb_caps_[0].support = DTV_L3_SUPPORT_UNSUPPORTED;
        lnb_count = 1U;
    }

    std::uint32_t shared_count = 0U;
    for (std::uint32_t receiver = 0U; receiver < profile_->enclosure_receiver_count; ++receiver)
    {
        const auto address = receiver_address(*profile_, receiver);
        if (!address)
        {
            continue;
        }
        bool seen = false;
        for (std::uint32_t index = 0U; index < shared_count; ++index)
        {
            if (shared_caps_[index].id == address.value().rf_function_index)
            {
                seen = true;
                break;
            }
        }
        if (!seen)
        {
            shared_caps_[shared_count].id = address.value().rf_function_index;
            shared_caps_[shared_count].compatibility = DTV_L3_LEASE_SHAREABLE;
            shared_caps_[shared_count].support = DTV_L3_SUPPORT_SUPPORTED;
            ++shared_count;
        }
    }

    catalog_.receivers = receiver_caps_;
    catalog_.receiver_count = receiver_count;
    catalog_.readers = reader_caps_;
    catalog_.reader_count = 1U;
    catalog_.lnb_scopes = lnb_caps_;
    catalog_.lnb_scope_count = lnb_count;
    catalog_.shared_scopes = shared_caps_;
    catalog_.shared_scope_count = shared_count;
    catalog_.ts_form = DTV_L3_SUPPORT_SUPPORTED;
}

dtv_l3_result Adapter::bind(dtv_l3_context* owner)
{
    if (owner == nullptr)
    {
        return DTV_L3_INVALID;
    }
    owner_ = owner;
    return DTV_L3_OK;
}

dtv_l3_result Adapter::initialize(dtv_l3_context* owner)
{
    dtv_l3_context* context = owner != nullptr ? owner : owner_;
    if (context == nullptr)
    {
        return DTV_L3_FAILED;
    }
    dtv_l3_transfer_request request{};
    request.kind = DTV_L3_TRANSFER_CONTROL;
    request.endpoint = 0U;
    request.timeout_ms = 2000U;
    request.control.request_type = 0xC0U;
    request.control.request = 0x01U;
    request.control.value = 0U;
    request.control.index = 0U;
    request.in_capacity = 1U;
    request.claim_index = 0U;
    request.deadline_ns = DTV_L3_DEADLINE_NONE;

    dtv_l3_op* op = nullptr;
    const auto started = dtv_l3_transfer_start(context, &request, &op);
    if (started != DTV_L3_ACCEPTED || op == nullptr)
    {
        return started == DTV_L3_OK ? DTV_L3_FAILED : started;
    }
    const auto waited = dtv_l3_op_wait(op, 2000U);
    dtv_l3_op_result result{};
    const auto polled = dtv_l3_op_poll(op, &result);
    if (waited != DTV_L3_OK || polled != DTV_L3_OK)
    {
        (void)dtv_l3_op_cancel(op);
        dtv_l3_op_result late{};
        if (dtv_l3_op_poll(op, &late) == DTV_L3_OK && late.state != DTV_L3_OP_RUNNING)
        {
            (void)dtv_l3_op_release(op);
        }
        return DTV_L3_FAILED;
    }
    const auto released = dtv_l3_op_release(op);
    if (result.state != DTV_L3_OP_DONE || result.result != DTV_L3_OK)
    {
        return result.result == DTV_L3_OK ? DTV_L3_FAILED : result.result;
    }
    return released;
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

std::uint32_t Adapter::stream_system_for(std::uint32_t receiver,
                                         const asicen_l3_route& route) const noexcept
{
    if (profile_->combined_isdb_ts)
    {
        return receiver < 4U ? tuned_system_[receiver] : 0U;
    }
    if (receiver < 4U && tuned_system_[receiver] != 0U &&
        has_system(tuned_system_[receiver], route.system))
    {
        return tuned_system_[receiver];
    }
    return route.system;
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
    const auto tuned = host_.tune(receiver, system, frequency_hz, timeout_ms, programmed_hz);
    if (tuned == DTV_L3_OK && receiver < 4U)
    {
        tuned_system_[receiver] = system;
    }
    return tuned;
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
    if (power_permission_ == 0U || receiver >= profile_->enclosure_receiver_count)
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

dtv_l3_result Adapter::bind_thunk(void* context, dtv_l3_context* owner)
{
    return static_cast<Adapter*>(context)->bind(owner);
}

dtv_l3_result Adapter::initialize_thunk(void* context, dtv_l3_context* owner)
{
    return static_cast<Adapter*>(context)->initialize(owner);
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

dtv_l3_result Adapter::tune_ex_thunk(void* context, dtv_l3_provider_binding*,
                                     std::uint32_t receiver_id, const dtv_l3_tune_request* request,
                                     std::uint64_t* programmed_hz)
{
    if (request == nullptr)
    {
        return DTV_L3_INVALID;
    }
    if (request->selection != DTV_L3_TUNE_SELECTION_AUTO)
    {
        return DTV_L3_UNSUPPORTED;
    }
    std::uint32_t system = 0U;
    switch (request->system)
    {
    case DTV_L3_TUNE_SYSTEM_ISDB_T:
        system = ASICEN_L3_SYSTEM_TERRESTRIAL;
        break;
    case DTV_L3_TUNE_SYSTEM_ISDB_S:
        system = ASICEN_L3_SYSTEM_SATELLITE;
        break;
    case DTV_L3_TUNE_SYSTEM_AUTO:
        system = 0U;
        break;
    default:
        return DTV_L3_UNSUPPORTED;
    }
    return static_cast<Adapter*>(context)->tune(receiver_id, system, request->requested_hz,
                                                kDefaultTuneTimeoutMs, programmed_hz);
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
    const auto stream_system = adapter->stream_system_for(receiver_id, route);
    if (stream_system == 0U)
    {
        return DTV_L3_UNSUPPORTED;
    }
    const auto started = adapter->host_.stream_begin(receiver_id, stream_system, bridge_id);
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
    const auto stream_system = adapter->stream_system_for(pending.receiver, route);
    if (stream_system == 0U)
    {
        return DTV_L3_UNSUPPORTED;
    }
    const auto attached = adapter->host_.stream_attach(stream_id, pending.receiver, stream_system,
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

dtv_l3_result Adapter::stream_attach_ex_thunk(void* context, dtv_l3_provider_binding*,
                                              std::uint64_t stream_id, std::uint32_t receiver_id,
                                              std::uint32_t bridge_id, dtv_l3_ts_deliver_fn deliver,
                                              void* callback_context)
{
    auto* adapter = static_cast<Adapter*>(context);
    asicen_l3_route route{};
    if (!adapter->route(receiver_id, &route) || !adapter->bridge_matches(route, bridge_id) ||
        adapter->owner_ == nullptr || adapter->stream_running_.exchange(true))
    {
        return DTV_L3_BUSY;
    }
    adapter->stream_stop_.store(false);
    adapter->stream_claim_index_ = route.function_index;
    adapter->stream_deliver_ = deliver;
    adapter->stream_deliver_context_ = callback_context;
    try
    {
        adapter->stream_worker_ = std::thread([adapter] { adapter->stream_loop(); });
    } catch (...)
    {
        adapter->stream_running_.store(false);
        return DTV_L3_FAILED;
    }
    {
        std::lock_guard<std::mutex> lock(adapter->mutex_);
        adapter->streams_.emplace_back(stream_id, receiver_id);
    }
    return DTV_L3_OK;
}

dtv_l3_result Adapter::stream_stop_ex_thunk(void* context, dtv_l3_provider_binding*,
                                            std::uint64_t stream_id, std::uint32_t receiver_id,
                                            std::uint32_t bridge_id)
{
    auto* adapter = static_cast<Adapter*>(context);
    asicen_l3_route route{};
    if (!adapter->route(receiver_id, &route) || !adapter->bridge_matches(route, bridge_id))
    {
        return DTV_L3_UNSUPPORTED;
    }
    adapter->stream_stop_.store(true);
    if (adapter->stream_worker_.joinable())
    {
        adapter->stream_worker_.join();
    }
    std::lock_guard<std::mutex> lock(adapter->mutex_);
    adapter->streams_.erase(
        std::remove_if(adapter->streams_.begin(), adapter->streams_.end(),
                       [stream_id](const auto& entry) { return entry.first == stream_id; }),
        adapter->streams_.end());
    return DTV_L3_OK;
}

void Adapter::stream_loop()
{
    dtv_l3_context* owner = owner_;
    if (owner != nullptr && !stream_stop_.load())
    {
        dtv_l3_transfer_request request{};
        request.kind = DTV_L3_TRANSFER_BULK;
        request.endpoint = 0x81U;
        request.timeout_ms = 200U;
        request.in_capacity = 188U;
        request.claim_index = stream_claim_index_;
        request.deadline_ns = DTV_L3_DEADLINE_NONE;

        dtv_l3_op* op = nullptr;
        const auto started = dtv_l3_transfer_start(owner, &request, &op);
        if (started == DTV_L3_ACCEPTED && op != nullptr)
        {
            while (true)
            {
                const auto waited = dtv_l3_op_wait(op, 200U);
                if (waited == DTV_L3_OK)
                {
                    break;
                }
                if (waited == DTV_L3_WAIT_TIMEOUT)
                {
                    if (stream_stop_.load())
                    {
                        (void)dtv_l3_op_cancel(op);
                    }
                    continue;
                }
                (void)dtv_l3_op_cancel(op);
                break;
            }
            dtv_l3_op_result result{};
            if (dtv_l3_op_poll(op, &result) == DTV_L3_OK && !stream_stop_.load() &&
                result.state == DTV_L3_OP_DONE && result.result == DTV_L3_OK &&
                result.data != nullptr && result.data_size != 0U && stream_deliver_ != nullptr)
            {
                stream_deliver_(stream_deliver_context_, result.data, result.data_size);
            }
            (void)dtv_l3_op_release(op);
        }
    }
    while (!stream_stop_.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    stream_running_.store(false);
}

Factory::Factory(const DeviceProfile& profile, Host& host) : profile_(&profile), host_(host)
{
    factory_.factory_context = this;
    factory_.create = &Factory::create_thunk;
    factory_.release = &Factory::release_thunk;
    factory_v1_.struct_size = static_cast<std::uint32_t>(sizeof(factory_v1_));
    factory_v1_.factory_context = this;
    factory_v1_.create = &Factory::create_v1_thunk;
    factory_v1_.release = &Factory::release_thunk;
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

dtv_l3_result Factory::create_v1_thunk(void* context, const dtv_l3_open_args* args,
                                       const dtv_l3_product_adapter** adapter_out,
                                       std::uint32_t* claim_count_out, void** adapter_owner_out)
{
    return static_cast<Factory*>(context)->create_v1(args, adapter_out, claim_count_out,
                                                     adapter_owner_out);
}

dtv_l3_result Factory::create_v1(const dtv_l3_open_args* args,
                                 const dtv_l3_product_adapter** adapter_out,
                                 std::uint32_t* claim_count_out, void** adapter_owner_out)
{
    if (args == nullptr || adapter_out == nullptr || claim_count_out == nullptr ||
        adapter_owner_out == nullptr)
    {
        return DTV_L3_INVALID;
    }
    *adapter_out = nullptr;
    *claim_count_out = 0U;
    *adapter_owner_out = nullptr;
    if (args->grant_count != 0U && args->grants == nullptr)
    {
        return DTV_L3_INVALID;
    }
    for (std::uint32_t index = 0U; index < args->grant_count; ++index)
    {
        const auto& grant = args->grants[index];
        if (grant.descriptor == nullptr)
        {
            return DTV_L3_INVALID;
        }
        if (grant.descriptor->vendor_id != profile_->vid ||
            grant.descriptor->product_id != profile_->pid ||
            grant.descriptor->num_interfaces < profile_->expected_runtime_functions)
        {
            return DTV_L3_UNSUPPORTED;
        }
    }

    auto adapter = std::make_unique<Adapter>(*profile_, host_);
    const auto firmware_result = adapter->prepare_firmware(args->firmware);
    if (firmware_result != DTV_L3_OK)
    {
        return firmware_result;
    }
    if (!firmware_matches_manifest(args->firmware, adapter->product_adapter()->firmware))
    {
        return DTV_L3_FAILED;
    }
    adapter->set_power_permission(args->power_permission);
    *adapter_out = adapter->product_adapter();
    *claim_count_out = profile_->expected_runtime_functions;
    *adapter_owner_out = adapter.release();
    return DTV_L3_OK;
}

void Factory::release_thunk(void* owner)
{
    delete static_cast<Adapter*>(owner);
}

} // namespace asicen::l3
