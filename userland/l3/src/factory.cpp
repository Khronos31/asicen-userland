// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/l3.h"

#include "adapter.h"

#include "asicen/device_profile.h"

#include <array>
#include <memory>

namespace {

using asicen::l3::Adapter;
using asicen::l3::Host;

constexpr std::size_t kModelCount = 5U;

struct CapabilityTable {
    std::array<asicen_l3_capability, kModelCount> entries{};

    CapabilityTable()
    {
        for (std::size_t index = 0U; index < kModelCount; ++index)
        {
            const auto* profile = asicen::find_profile(static_cast<asicen::ModelId>(index));
            if (profile != nullptr)
            {
                asicen::l3::fill_capability(*profile, &entries[index]);
            }
        }
    }
};

const CapabilityTable& capability_table()
{
    static const CapabilityTable table;
    return table;
}

class COpsHost final : public Host {
  public:
    explicit COpsHost(const asicen_l3_host_ops& ops) : ops_(ops)
    {
    }

    dtv_l3_claim_result claim(std::uint32_t claim_index) override
    {
        return ops_.claim(ops_.context, claim_index);
    }

    dtv_l3_release_result release_claim(std::uint32_t claim_index) override
    {
        return ops_.release_claim(ops_.context, claim_index);
    }

    dtv_l3_result submit(std::uint64_t transfer_id, const dtv_l3_transfer_request& request,
                         dtv_l3_transfer_complete_fn complete, void* callback_context) override
    {
        return ops_.submit(ops_.context, transfer_id, &request, complete, callback_context);
    }

    dtv_l3_result cancel(std::uint64_t transfer_id) override
    {
        return ops_.cancel(ops_.context, transfer_id);
    }

    dtv_l3_result tune(std::uint32_t receiver, std::uint32_t system, std::uint64_t frequency_hz,
                       std::uint32_t timeout_ms, std::uint64_t* programmed_hz) override
    {
        if (ops_.tune == nullptr)
        {
            return DTV_L3_UNSUPPORTED;
        }
        return ops_.tune(ops_.context, receiver, system, frequency_hz, timeout_ms, programmed_hz);
    }

    dtv_l3_result stream_begin(std::uint32_t receiver, std::uint32_t system,
                               std::uint32_t bridge_id) override
    {
        if (ops_.stream_begin == nullptr)
        {
            return DTV_L3_UNSUPPORTED;
        }
        return ops_.stream_begin(ops_.context, receiver, system, bridge_id);
    }

    dtv_l3_result stream_attach(std::uint64_t stream_id, std::uint32_t receiver,
                                std::uint32_t system, dtv_l3_ts_deliver_fn deliver,
                                void* callback_context) override
    {
        if (ops_.stream_attach == nullptr)
        {
            return DTV_L3_UNSUPPORTED;
        }
        return ops_.stream_attach(ops_.context, stream_id, receiver, system, deliver,
                                  callback_context);
    }

    dtv_l3_result stream_stop(std::uint64_t stream_id) override
    {
        if (ops_.stream_stop == nullptr)
        {
            return DTV_L3_UNSUPPORTED;
        }
        return ops_.stream_stop(ops_.context, stream_id);
    }

    dtv_l3_result reader_open(std::uint32_t reader_id) override
    {
        if (ops_.reader_open == nullptr)
        {
            return DTV_L3_UNSUPPORTED;
        }
        return ops_.reader_open(ops_.context, reader_id);
    }

    dtv_l3_result reader_reset(std::uint32_t reader_id) override
    {
        if (ops_.reader_reset == nullptr)
        {
            return DTV_L3_UNSUPPORTED;
        }
        return ops_.reader_reset(ops_.context, reader_id);
    }

    dtv_l3_result reader_transmit(std::uint32_t reader_id, const std::uint8_t* command,
                                  std::size_t command_size, std::uint8_t* response,
                                  std::size_t response_capacity,
                                  std::size_t* response_size) override
    {
        if (ops_.reader_transmit == nullptr)
        {
            return DTV_L3_UNSUPPORTED;
        }
        return ops_.reader_transmit(ops_.context, reader_id, command, command_size, response,
                                    response_capacity, response_size);
    }

    dtv_l3_result reader_close(std::uint32_t reader_id) override
    {
        if (ops_.reader_close == nullptr)
        {
            return DTV_L3_UNSUPPORTED;
        }
        return ops_.reader_close(ops_.context, reader_id);
    }

    dtv_l3_result lnb_set(std::uint32_t receiver, std::uint32_t mode) override
    {
        if (ops_.lnb_set == nullptr)
        {
            return DTV_L3_UNSUPPORTED;
        }
        return ops_.lnb_set(ops_.context, receiver, mode);
    }

  private:
    asicen_l3_host_ops ops_{};
};

} // namespace

struct asicen_l3_factory {
    std::unique_ptr<COpsHost> host;
    std::unique_ptr<asicen::l3::Adapter> adapter;
};

extern "C" {

const asicen_l3_capability* asicen_l3_capability_for_model(asicen_l3_model model)
{
    if (model < ASICEN_L3_MODEL_S3U || model > ASICEN_L3_MODEL_W3U3_V2)
    {
        return nullptr;
    }
    return &capability_table().entries[static_cast<std::size_t>(model)];
}

const asicen_l3_capability* asicen_l3_capability_for_ids(uint16_t vid, uint16_t pid)
{
    const auto* profile = asicen::find_profile(vid, pid);
    if (profile == nullptr)
    {
        return nullptr;
    }
    return asicen_l3_capability_for_model(static_cast<asicen_l3_model>(profile->model_id));
}

int asicen_l3_model_for_ids(uint16_t vid, uint16_t pid, asicen_l3_model* model)
{
    if (model == nullptr)
    {
        return 0;
    }
    const auto* profile = asicen::find_profile(vid, pid);
    if (profile == nullptr)
    {
        return 0;
    }
    *model = static_cast<asicen_l3_model>(profile->model_id);
    return 1;
}

dtv_l3_result asicen_l3_factory_create(const asicen_l3_host_ops* host, uint16_t vid, uint16_t pid,
                                       asicen_l3_factory** factory_out)
{
    if (host == nullptr || factory_out == nullptr || host->claim == nullptr ||
        host->release_claim == nullptr || host->submit == nullptr || host->cancel == nullptr)
    {
        return DTV_L3_INVALID;
    }
    *factory_out = nullptr;
    const auto* profile = asicen::find_profile(vid, pid);
    if (profile == nullptr || !asicen::profile_runtime_supported(*profile))
    {
        return DTV_L3_UNSUPPORTED;
    }
    auto factory = std::make_unique<asicen_l3_factory>();
    factory->host = std::make_unique<COpsHost>(*host);
    factory->adapter = std::make_unique<asicen::l3::Adapter>(*profile, *factory->host);
    *factory_out = factory.release();
    return DTV_L3_OK;
}

void asicen_l3_factory_destroy(asicen_l3_factory* factory)
{
    delete factory;
}

const dtv_l3_provider* asicen_l3_factory_provider(const asicen_l3_factory* factory)
{
    return factory == nullptr || factory->adapter == nullptr ? nullptr
                                                             : factory->adapter->provider();
}

const asicen_l3_capability* asicen_l3_factory_capability(const asicen_l3_factory* factory)
{
    return factory == nullptr || factory->adapter == nullptr ? nullptr
                                                             : &factory->adapter->capability();
}

uint32_t asicen_l3_factory_claim_count(const asicen_l3_factory* factory)
{
    return factory == nullptr || factory->adapter == nullptr ? 0U : factory->adapter->claim_count();
}

int asicen_l3_factory_route(const asicen_l3_factory* factory, uint32_t receiver,
                            asicen_l3_route* route)
{
    if (factory == nullptr || factory->adapter == nullptr)
    {
        return 0;
    }
    return factory->adapter->route(receiver, route) ? 1 : 0;
}

dtv_l3_result asicen_l3_factory_tune(asicen_l3_factory* factory, uint32_t receiver, uint32_t system,
                                     uint64_t frequency_hz, uint32_t timeout_ms,
                                     uint64_t* programmed_hz)
{
    return factory == nullptr || factory->adapter == nullptr
               ? DTV_L3_INVALID
               : factory->adapter->tune(receiver, system, frequency_hz, timeout_ms, programmed_hz);
}

dtv_l3_result asicen_l3_factory_reader_open(asicen_l3_factory* factory, uint32_t reader_id)
{
    return factory == nullptr || factory->adapter == nullptr
               ? DTV_L3_INVALID
               : factory->adapter->reader_open(reader_id);
}

dtv_l3_result asicen_l3_factory_reader_reset(asicen_l3_factory* factory, uint32_t reader_id)
{
    return factory == nullptr || factory->adapter == nullptr
               ? DTV_L3_INVALID
               : factory->adapter->reader_reset(reader_id);
}

dtv_l3_result asicen_l3_factory_reader_transmit(asicen_l3_factory* factory, uint32_t reader_id,
                                                const uint8_t* command, size_t command_size,
                                                uint8_t* response, size_t response_capacity,
                                                size_t* response_size)
{
    return factory == nullptr || factory->adapter == nullptr
               ? DTV_L3_INVALID
               : factory->adapter->reader_transmit(reader_id, command, command_size, response,
                                                   response_capacity, response_size);
}

dtv_l3_result asicen_l3_factory_reader_close(asicen_l3_factory* factory, uint32_t reader_id)
{
    return factory == nullptr || factory->adapter == nullptr
               ? DTV_L3_INVALID
               : factory->adapter->reader_close(reader_id);
}

dtv_l3_result asicen_l3_factory_lnb_set(asicen_l3_factory* factory, uint32_t receiver,
                                        uint32_t mode)
{
    return factory == nullptr || factory->adapter == nullptr
               ? DTV_L3_INVALID
               : factory->adapter->lnb_set(receiver, mode);
}

} // extern "C"
