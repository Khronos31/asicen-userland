// SPDX-License-Identifier: GPL-2.0-or-later
#include "asicen/l3.h"

#include "adapter.h"

#include "asicen/device_profile.h"

#include <memory>

namespace {

using asicen::l3::Adapter;
using asicen::l3::Host;

class CProductHost final : public Host {
  public:
    explicit CProductHost(const asicen_l3_product_ops& ops) : ops_(ops)
    {
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
    asicen_l3_product_ops ops_{};
};

} // namespace

struct asicen_l3_factory {
    std::unique_ptr<CProductHost> host;
    std::unique_ptr<asicen::l3::Factory> factory;
};

extern "C" {

dtv_l3_result asicen_l3_capabilities_for_model(asicen_l3_model model,
                                               dtv_l3_capabilities* capabilities)
{
    if (capabilities == nullptr)
    {
        return DTV_L3_INVALID;
    }
    const auto* profile = asicen::find_profile(static_cast<asicen::ModelId>(model));
    if (profile == nullptr)
    {
        return DTV_L3_UNSUPPORTED;
    }
    asicen::l3::fill_capabilities(*profile, capabilities);
    return DTV_L3_OK;
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

dtv_l3_result asicen_l3_factory_create(const asicen_l3_product_ops* ops, uint16_t vid, uint16_t pid,
                                       asicen_l3_factory** factory_out)
{
    if (ops == nullptr || factory_out == nullptr)
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
    factory->host = std::make_unique<CProductHost>(*ops);
    factory->factory = std::make_unique<asicen::l3::Factory>(*profile, *factory->host);
    *factory_out = factory.release();
    return DTV_L3_OK;
}

void asicen_l3_factory_destroy(asicen_l3_factory* factory)
{
    delete factory;
}

const dtv_l3_adapter_factory* asicen_l3_factory_adapter_factory(const asicen_l3_factory* factory)
{
    return factory == nullptr || factory->factory == nullptr ? nullptr
                                                             : factory->factory->core_factory();
}

uint32_t asicen_l3_factory_claim_count(const asicen_l3_factory* factory)
{
    return factory == nullptr || factory->factory == nullptr ? 0U : factory->factory->claim_count();
}

int asicen_l3_factory_route(const asicen_l3_factory* factory, uint32_t receiver,
                            asicen_l3_route* route)
{
    if (factory == nullptr || factory->factory == nullptr)
    {
        return 0;
    }
    return factory->factory->route(receiver, route) ? 1 : 0;
}

dtv_l3_result asicen_l3_product_tune(const dtv_l3_product_adapter* adapter, uint32_t receiver,
                                     uint32_t system, uint64_t frequency_hz, uint32_t timeout_ms,
                                     uint64_t* programmed_hz)
{
    if (adapter == nullptr || adapter->product_context == nullptr)
    {
        return DTV_L3_INVALID;
    }
    auto* product = static_cast<Adapter*>(adapter->product_context);
    return product->tune(receiver, system, frequency_hz, timeout_ms, programmed_hz);
}

} // extern "C"
