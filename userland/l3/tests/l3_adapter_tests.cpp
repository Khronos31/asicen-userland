// SPDX-License-Identifier: GPL-2.0-or-later
#include "adapter.h"

#include "asicen/device_profile.h"
#include "asicen/l3.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

#define CHECK(condition)                                                                           \
    do                                                                                             \
    {                                                                                              \
        if (!(condition))                                                                          \
        {                                                                                          \
            std::fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #condition);             \
            return false;                                                                          \
        }                                                                                          \
    } while (false)

class FakeHost final : public asicen::l3::Host {
  public:
    dtv_l3_result tune(std::uint32_t receiver, std::uint32_t system, std::uint64_t frequency_hz,
                       std::uint32_t timeout_ms, std::uint64_t* programmed_hz) override
    {
        ++tune_calls;
        last_receiver = receiver;
        last_system = system;
        last_hz = frequency_hz;
        last_timeout_ms = timeout_ms;
        *programmed_hz = frequency_hz;
        return DTV_L3_OK;
    }

    dtv_l3_result stream_begin(std::uint32_t receiver, std::uint32_t system,
                               std::uint32_t bridge_id) override
    {
        ++begin_calls;
        last_receiver = receiver;
        last_system = system;
        last_bridge = bridge_id;
        return DTV_L3_OK;
    }

    dtv_l3_result stream_attach(std::uint64_t stream_id, std::uint32_t receiver,
                                std::uint32_t system, dtv_l3_ts_deliver_fn, void*) override
    {
        ++attach_calls;
        last_stream = stream_id;
        last_receiver = receiver;
        last_system = system;
        return DTV_L3_OK;
    }

    dtv_l3_result stream_stop(std::uint64_t stream_id) override
    {
        ++stop_calls;
        last_stream = stream_id;
        return DTV_L3_OK;
    }

    dtv_l3_result reader_open(std::uint32_t reader_id) override
    {
        ++reader_open_calls;
        last_reader = reader_id;
        return DTV_L3_OK;
    }

    dtv_l3_result reader_reset(std::uint32_t reader_id) override
    {
        ++reader_reset_calls;
        last_reader = reader_id;
        return DTV_L3_OK;
    }

    dtv_l3_result reader_transmit(std::uint32_t reader_id, const std::uint8_t* command,
                                  std::size_t command_size, std::uint8_t* response, std::size_t,
                                  std::size_t* response_size) override
    {
        ++reader_transmit_calls;
        last_reader = reader_id;
        last_command_size = command_size;
        if (response_size != nullptr)
        {
            *response_size = command_size;
        }
        if (response != nullptr && command != nullptr)
        {
            std::memcpy(response, command, command_size);
        }
        return DTV_L3_OK;
    }

    dtv_l3_result reader_close(std::uint32_t reader_id) override
    {
        ++reader_close_calls;
        last_reader = reader_id;
        return DTV_L3_OK;
    }

    dtv_l3_result lnb_set(std::uint32_t receiver, std::uint32_t mode) override
    {
        ++lnb_calls;
        last_receiver = receiver;
        last_lnb_mode = mode;
        return DTV_L3_OK;
    }

    std::uint32_t tune_calls = 0U;
    std::uint32_t begin_calls = 0U;
    std::uint32_t attach_calls = 0U;
    std::uint32_t stop_calls = 0U;
    std::uint32_t reader_open_calls = 0U;
    std::uint32_t reader_reset_calls = 0U;
    std::uint32_t reader_transmit_calls = 0U;
    std::uint32_t reader_close_calls = 0U;
    std::uint32_t lnb_calls = 0U;
    std::uint32_t last_receiver = 0xffffffffU;
    std::uint32_t last_system = 0U;
    std::uint32_t last_bridge = 0xffffffffU;
    std::uint32_t last_reader = 0xffffffffU;
    std::uint32_t last_lnb_mode = 0xffffffffU;
    std::uint64_t last_hz = 0U;
    std::uint32_t last_timeout_ms = 0U;
    std::uint64_t last_stream = 0U;
    std::size_t last_command_size = 0U;
};

bool expect_route(const asicen::l3::Adapter& adapter, std::uint32_t receiver,
                  std::uint32_t function_index, std::uint32_t local_lane,
                  std::uint32_t rf_function_index, std::uint32_t frontend_source,
                  std::uint32_t system)
{
    asicen_l3_route route{};
    if (!adapter.route(receiver, &route))
    {
        return false;
    }
    return route.valid == 1U && route.function_index == function_index &&
           route.local_lane == local_lane && route.rf_function_index == rf_function_index &&
           route.frontend_source == frontend_source && route.system == system;
}

bool test_capabilities_and_routes()
{
    const auto* s3u = asicen::find_profile(asicen::ModelId::S3u);
    const auto* s3u2 = asicen::find_profile(asicen::ModelId::S3u2);
    const auto* w3u3 = asicen::find_profile(asicen::ModelId::W3u3);
    const auto* v2 = asicen::find_profile(asicen::ModelId::W3u3V2);
    CHECK(s3u != nullptr && s3u2 != nullptr && w3u3 != nullptr && v2 != nullptr);

    FakeHost host;
    asicen::l3::Adapter s3u_adapter(*s3u, host);
    asicen::l3::Adapter s3u2_adapter(*s3u2, host);
    asicen::l3::Adapter w3u3_adapter(*w3u3, host);
    asicen::l3::Adapter v2_adapter(*v2, host);
    asicen::l3::Factory w3u3_factory(*w3u3, host);

    CHECK(s3u_adapter.capabilities().receiver_count == 1U);
    CHECK(s3u_adapter.capabilities().reader_count == 1U);
    CHECK(s3u_adapter.capabilities().lnb_power == 0U);
    CHECK(s3u_adapter.capabilities().serial == 0U);
    CHECK(s3u_adapter.capabilities().cancel_scope == DTV_L3_CANCEL_SCOPE_TRANSFER);
    CHECK(expect_route(s3u_adapter, 0U, 0U, 0U, 0U, 0U, ASICEN_L3_SYSTEM_SATELLITE));

    CHECK(s3u2_adapter.capabilities().receiver_count == 2U);
    CHECK(s3u2_adapter.capabilities().lnb_power == 0U);
    CHECK(expect_route(s3u2_adapter, 0U, 0U, 0U, 0U, 0U, ASICEN_L3_SYSTEM_SATELLITE));
    CHECK(expect_route(s3u2_adapter, 1U, 0U, 1U, 0U, 1U, ASICEN_L3_SYSTEM_TERRESTRIAL));

    CHECK(w3u3_adapter.capabilities().receiver_count == 4U);
    CHECK(w3u3_adapter.capabilities().reader_count == 1U);
    CHECK(w3u3_adapter.capabilities().lnb_power == 1U);
    CHECK(w3u3_factory.claim_count() == 2U);
    CHECK(expect_route(w3u3_adapter, 0U, 0U, 0U, 0U, 0U, ASICEN_L3_SYSTEM_SATELLITE));
    CHECK(expect_route(w3u3_adapter, 1U, 0U, 1U, 0U, 1U, ASICEN_L3_SYSTEM_TERRESTRIAL));
    CHECK(expect_route(w3u3_adapter, 2U, 1U, 0U, 1U, 0U, ASICEN_L3_SYSTEM_SATELLITE));
    CHECK(expect_route(w3u3_adapter, 3U, 1U, 1U, 1U, 1U, ASICEN_L3_SYSTEM_TERRESTRIAL));
    CHECK(!w3u3_adapter.route(4U, nullptr));

    /* V2 assigns all four receivers to the shared RF function 0 and flips the
     * public source order. */
    CHECK(v2_adapter.capabilities().receiver_count == 4U);
    CHECK(expect_route(v2_adapter, 0U, 0U, 0U, 0U, 1U, ASICEN_L3_SYSTEM_SATELLITE));
    CHECK(expect_route(v2_adapter, 1U, 0U, 1U, 0U, 0U, ASICEN_L3_SYSTEM_TERRESTRIAL));
    CHECK(expect_route(v2_adapter, 2U, 1U, 0U, 0U, 3U, ASICEN_L3_SYSTEM_SATELLITE));
    CHECK(expect_route(v2_adapter, 3U, 1U, 1U, 0U, 2U, ASICEN_L3_SYSTEM_TERRESTRIAL));
    return true;
}

bool test_tune_semantics()
{
    FakeHost host;
    const auto* s3u = asicen::find_profile(asicen::ModelId::S3u);
    const auto* s3u2 = asicen::find_profile(asicen::ModelId::S3u2);
    const auto* w3u3 = asicen::find_profile(asicen::ModelId::W3u3);
    CHECK(s3u != nullptr && s3u2 != nullptr && w3u3 != nullptr);
    asicen::l3::Adapter s3u_adapter(*s3u, host);
    asicen::l3::Adapter s3u2_adapter(*s3u2, host);
    asicen::l3::Adapter w3u3_adapter(*w3u3, host);

    std::uint64_t programmed = 0U;
    CHECK(w3u3_adapter.tune(0U, ASICEN_L3_SYSTEM_SATELLITE, 11000000000ULL, 2500U, &programmed) ==
          DTV_L3_OK);
    CHECK(host.tune_calls == 1U && host.last_receiver == 0U &&
          host.last_system == ASICEN_L3_SYSTEM_SATELLITE && host.last_hz == 11000000000ULL &&
          host.last_timeout_ms == 2500U && programmed == 11000000000ULL);
    CHECK(w3u3_adapter.tune(1U, ASICEN_L3_SYSTEM_TERRESTRIAL, 527143000ULL, 2500U, &programmed) ==
          DTV_L3_OK);
    CHECK(host.tune_calls == 2U && host.last_system == ASICEN_L3_SYSTEM_TERRESTRIAL);
    CHECK(w3u3_adapter.tune(1U, ASICEN_L3_SYSTEM_SATELLITE, 11000000000ULL, 2500U, &programmed) ==
          DTV_L3_UNSUPPORTED);
    CHECK(host.tune_calls == 2U);
    CHECK(w3u3_adapter.tune(9U, ASICEN_L3_SYSTEM_SATELLITE, 11000000000ULL, 2500U, &programmed) ==
          DTV_L3_UNSUPPORTED);
    CHECK(w3u3_adapter.tune(0U, ASICEN_L3_SYSTEM_SATELLITE, 0U, 2500U, &programmed) ==
          DTV_L3_INVALID);

    /* Combined S3U has one receiver that accepts both systems, so the
     * candidate Core tune callback cannot choose one. */
    CHECK(s3u_adapter.tune(0U, 0U, 11000000000ULL, 2500U, &programmed) == DTV_L3_UNSUPPORTED);
    CHECK(host.tune_calls == 2U);
    CHECK(s3u_adapter.tune(0U, ASICEN_L3_SYSTEM_TERRESTRIAL, 527143000ULL, 2500U, &programmed) ==
          DTV_L3_OK);
    CHECK(host.tune_calls == 3U && host.last_system == ASICEN_L3_SYSTEM_TERRESTRIAL);

    CHECK(s3u2_adapter.tune(0U, 0U, 11000000000ULL, 2500U, &programmed) == DTV_L3_OK);
    CHECK(host.last_system == ASICEN_L3_SYSTEM_SATELLITE);
    CHECK(s3u2_adapter.tune(1U, 0U, 527143000ULL, 2500U, &programmed) == DTV_L3_OK);
    CHECK(host.last_system == ASICEN_L3_SYSTEM_TERRESTRIAL);
    return true;
}

bool test_product_adapter_callbacks()
{
    FakeHost host;
    const auto* s3u = asicen::find_profile(asicen::ModelId::S3u);
    const auto* w3u3 = asicen::find_profile(asicen::ModelId::W3u3);
    CHECK(s3u != nullptr && w3u3 != nullptr);
    asicen::l3::Adapter s3u_adapter(*s3u, host);
    asicen::l3::Adapter w3u3_adapter(*w3u3, host);
    const dtv_l3_product_adapter* product = w3u3_adapter.product_adapter();
    CHECK(product != nullptr && product->tune != nullptr && product->bridge_start != nullptr &&
          product->stream_attach != nullptr && product->reader_acquire != nullptr);

    std::uint64_t programmed = 0U;
    CHECK(product->tune(product->product_context, nullptr, 0U, 11000000000ULL, &programmed) ==
          DTV_L3_OK);
    CHECK(host.last_system == ASICEN_L3_SYSTEM_SATELLITE);
    CHECK(product->tune(product->product_context, nullptr, 1U, 527143000ULL, &programmed) ==
          DTV_L3_OK);
    CHECK(host.last_system == ASICEN_L3_SYSTEM_TERRESTRIAL);

    dtv_l3_scope_info scope{};
    CHECK(product->scope_query(product->product_context, 2U, &scope) == DTV_L3_OK);
    CHECK(scope.bridge_id == 1U && scope.compatibility == DTV_L3_LEASE_SHAREABLE);
    CHECK(product->scope_query(product->product_context, 9U, &scope) == DTV_L3_UNSUPPORTED);

    CHECK(product->bridge_start(product->product_context, nullptr, 0U, 1U) == DTV_L3_OK);
    CHECK(host.begin_calls == 1U && host.last_receiver == 1U &&
          host.last_system == ASICEN_L3_SYSTEM_TERRESTRIAL && host.last_bridge == 0U);
    CHECK(product->stream_attach(product->product_context, nullptr, 77U, nullptr, nullptr) ==
          DTV_L3_OK);
    CHECK(host.attach_calls == 1U && host.last_stream == 77U && host.last_receiver == 1U &&
          host.last_system == ASICEN_L3_SYSTEM_TERRESTRIAL);
    CHECK(product->stream_stop(product->product_context, nullptr, 77U) == DTV_L3_OK);
    CHECK(host.stop_calls == 1U);
    CHECK(product->stream_stop(product->product_context, nullptr, 77U) == DTV_L3_STALE);
    CHECK(product->bridge_start(product->product_context, nullptr, 1U, 1U) == DTV_L3_UNSUPPORTED);
    CHECK(product->bridge_start(product->product_context, nullptr, 0U, 3U) == DTV_L3_UNSUPPORTED);

    dtv_l3_reader* reader = nullptr;
    CHECK(product->reader_acquire(product->product_context, 0U, &reader) == DTV_L3_OK &&
          reader != nullptr);
    CHECK(host.reader_open_calls == 1U && host.last_reader == 0U);
    CHECK(product->reader_acquire(product->product_context, 1U, &reader) == DTV_L3_UNSUPPORTED);
    CHECK(product->lnb_set(product->product_context, 0U, 15U) == DTV_L3_OK);
    CHECK(host.lnb_calls == 1U && host.last_receiver == 0U && host.last_lnb_mode == 15U);
    CHECK(product->lnb_set(product->product_context, 1U, 15U) == DTV_L3_UNSUPPORTED);
    CHECK(product->lnb_set(product->product_context, 2U, 0U) == DTV_L3_OK);
    CHECK(host.lnb_calls == 2U);
    CHECK(product->lnb_set(product->product_context, 9U, 15U) == DTV_L3_UNSUPPORTED);
    CHECK(product->lnb_set(product->product_context, 1U, 15U) == DTV_L3_UNSUPPORTED);

    const dtv_l3_product_adapter* combined = s3u_adapter.product_adapter();
    CHECK(combined->tune(combined->product_context, nullptr, 0U, 11000000000ULL, &programmed) ==
          DTV_L3_UNSUPPORTED);
    CHECK(combined->bridge_start(combined->product_context, nullptr, 0U, 0U) == DTV_L3_UNSUPPORTED);
    CHECK(combined->lnb_set(combined->product_context, 0U, 15U) == DTV_L3_UNSUPPORTED);
    return true;
}

bool test_reader_and_transmit()
{
    FakeHost host;
    const auto* w3u3 = asicen::find_profile(asicen::ModelId::W3u3);
    CHECK(w3u3 != nullptr);
    asicen::l3::Adapter adapter(*w3u3, host);

    CHECK(adapter.reader_open(0U) == DTV_L3_OK);
    CHECK(host.reader_open_calls == 1U && host.last_reader == 0U);
    CHECK(adapter.reader_open(1U) == DTV_L3_UNSUPPORTED);
    CHECK(host.reader_open_calls == 1U);
    CHECK(adapter.reader_reset(0U) == DTV_L3_OK);
    CHECK(host.reader_reset_calls == 1U);
    const std::uint8_t command[] = {0x00U, 0xA4U, 0x00U, 0x00U};
    std::uint8_t response[8]{};
    std::size_t response_size = 0U;
    CHECK(adapter.reader_transmit(0U, command, sizeof(command), response, sizeof(response),
                                  &response_size) == DTV_L3_OK);
    CHECK(host.reader_transmit_calls == 1U && response_size == sizeof(command) &&
          std::memcmp(response, command, sizeof(command)) == 0);
    CHECK(adapter.reader_transmit(1U, command, sizeof(command), response, sizeof(response),
                                  &response_size) == DTV_L3_INVALID);
    CHECK(adapter.reader_close(0U) == DTV_L3_OK);
    CHECK(host.reader_close_calls == 1U);
    return true;
}

bool test_firmware_handling()
{
    FakeHost host;
    const auto* w3u3 = asicen::find_profile(asicen::ModelId::W3u3);
    CHECK(w3u3 != nullptr);

    std::uint8_t bytes[] = {0x01U, 0x02U, 0x03U};
    const dtv_l3_firmware bytes_view{DTV_L3_FIRMWARE_BYTES, nullptr, bytes,
                                     sizeof(bytes),         nullptr, nullptr};
    asicen::l3::Adapter bytes_adapter(*w3u3, host);
    CHECK(bytes_adapter.prepare_firmware(&bytes_view) == DTV_L3_OK);
    const dtv_l3_product_adapter* product = bytes_adapter.product_adapter();
    CHECK(product->firmware != nullptr);
    CHECK(product->firmware->source == DTV_L3_FIRMWARE_BYTES);
    CHECK(product->firmware->size == sizeof(bytes));
    CHECK(product->firmware->bytes != bytes);
    CHECK(std::memcmp(product->firmware->bytes, bytes, sizeof(bytes)) == 0);
    bytes[0] = 0xffU;
    CHECK(product->firmware->bytes[0] == 0x01U);

    const dtv_l3_firmware path_view{
        DTV_L3_FIRMWARE_PATH, "asicen-loader.bin", nullptr, 0U, nullptr, nullptr};
    asicen::l3::Adapter path_adapter(*w3u3, host);
    CHECK(path_adapter.prepare_firmware(&path_view) == DTV_L3_OK);
    CHECK(path_adapter.product_adapter()->firmware != nullptr);
    CHECK(path_adapter.product_adapter()->firmware->source == DTV_L3_FIRMWARE_PATH);
    CHECK(std::strcmp(path_adapter.product_adapter()->firmware->path, "asicen-loader.bin") == 0);

    const dtv_l3_firmware bad_bytes{DTV_L3_FIRMWARE_BYTES, nullptr, nullptr, 0U, nullptr, nullptr};
    asicen::l3::Adapter bad_adapter(*w3u3, host);
    CHECK(bad_adapter.prepare_firmware(&bad_bytes) == DTV_L3_INVALID);
    const dtv_l3_firmware bad_path{DTV_L3_FIRMWARE_PATH, nullptr, nullptr, 0U, nullptr, nullptr};
    CHECK(bad_adapter.prepare_firmware(&bad_path) == DTV_L3_INVALID);

    struct ReadContext {
        std::uint8_t data[2] = {0x0aU, 0x0bU};
    } read_context;
    const dtv_l3_firmware read_view{
        DTV_L3_FIRMWARE_READ_CALLBACK,
        nullptr,
        nullptr,
        0U,
        [](void* context, const std::uint8_t** data, std::size_t* size) -> dtv_l3_result {
            auto* read = static_cast<ReadContext*>(context);
            *data = read->data;
            *size = sizeof(read->data);
            return DTV_L3_OK;
        },
        &read_context};
    asicen::l3::Adapter read_adapter(*w3u3, host);
    CHECK(read_adapter.prepare_firmware(&read_view) == DTV_L3_OK);
    CHECK(read_adapter.product_adapter()->firmware != nullptr);
    CHECK(read_adapter.product_adapter()->firmware->size == 2U);
    CHECK(read_adapter.product_adapter()->firmware->bytes[1] == 0x0bU);
    return true;
}

bool test_factory_create_and_release()
{
    FakeHost host;
    const auto* w3u3 = asicen::find_profile(asicen::ModelId::W3u3);
    CHECK(w3u3 != nullptr);
    asicen::l3::Factory factory(*w3u3, host);
    CHECK(factory.claim_count() == 2U);

    std::uint8_t bytes[] = {0x10U, 0x20U};
    const dtv_l3_firmware firmware{DTV_L3_FIRMWARE_BYTES, nullptr, bytes,
                                   sizeof(bytes),         nullptr, nullptr};
    dtv_l3_open_config config{};
    config.model = static_cast<std::uint32_t>(asicen::ModelId::W3u3);
    config.selector = 0x1234U;
    config.firmware = &firmware;

    const dtv_l3_product_adapter* product = nullptr;
    void* owner = nullptr;
    CHECK(factory.core_factory()->create(factory.core_factory()->factory_context, &config, &product,
                                         &owner) == DTV_L3_OK);
    CHECK(product != nullptr && owner != nullptr);
    CHECK(product->capabilities.receiver_count == 4U);
    CHECK(product->capabilities.lnb_power == 1U);
    CHECK(product->firmware != nullptr && product->firmware->size == sizeof(bytes));
    CHECK(product->product_context == owner);
    auto* adapter = static_cast<asicen::l3::Adapter*>(owner);
    CHECK(adapter->profile().model_id == asicen::ModelId::W3u3);
    factory.core_factory()->release(owner);

    config.model = static_cast<std::uint32_t>(asicen::ModelId::S3u);
    product = nullptr;
    owner = nullptr;
    CHECK(factory.core_factory()->create(factory.core_factory()->factory_context, &config, &product,
                                         &owner) == DTV_L3_UNSUPPORTED);
    CHECK(product == nullptr && owner == nullptr);
    return true;
}

} // namespace

int main()
{
    if (!test_capabilities_and_routes())
        return 1;
    if (!test_tune_semantics())
        return 1;
    if (!test_product_adapter_callbacks())
        return 1;
    if (!test_reader_and_transmit())
        return 1;
    if (!test_firmware_handling())
        return 1;
    if (!test_factory_create_and_release())
        return 1;
    return 0;
}
