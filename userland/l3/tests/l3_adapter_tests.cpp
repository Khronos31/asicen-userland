// SPDX-License-Identifier: GPL-2.0-or-later
#include "adapter.h"

#include "asicen/device_profile.h"
#include "asicen/l3.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

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
    dtv_l3_claim_result claim(std::uint32_t claim_index) override
    {
        claim_calls = claim_index + 1U;
        return {DTV_L3_OK, DTV_L3_CLAIM_RETAINED};
    }

    dtv_l3_release_result release_claim(std::uint32_t claim_index) override
    {
        release_calls = claim_index + 1U;
        return {DTV_L3_RELEASE_QUIESCED, DTV_L3_CLAIM_NONE, DTV_L3_RESTORE_NOT_ATTEMPTED,
                DTV_L3_OK};
    }

    dtv_l3_result submit(std::uint64_t, const dtv_l3_transfer_request&, dtv_l3_transfer_complete_fn,
                         void*) override
    {
        ++submit_calls;
        return DTV_L3_UNSUPPORTED;
    }

    dtv_l3_result cancel(std::uint64_t) override
    {
        return DTV_L3_UNSUPPORTED;
    }

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
        return stop_result;
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

    dtv_l3_result stop_result = DTV_L3_OK;
    std::uint32_t claim_calls = 0U;
    std::uint32_t release_calls = 0U;
    std::uint32_t submit_calls = 0U;
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

bool test_capability_and_routes()
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

    CHECK(s3u_adapter.claim_count() == 1U);
    CHECK(s3u_adapter.capability().public_receivers == 1U);
    CHECK(s3u_adapter.capability().combined_isdb_ts == 1U);
    CHECK(s3u_adapter.capability().lnb_receiver_mask == 0U);
    CHECK(expect_route(s3u_adapter, 0U, 0U, 0U, 0U, 0U, ASICEN_L3_SYSTEM_SATELLITE));

    CHECK(s3u2_adapter.claim_count() == 1U);
    CHECK(s3u2_adapter.capability().public_receivers == 2U);
    CHECK(expect_route(s3u2_adapter, 0U, 0U, 0U, 0U, 0U, ASICEN_L3_SYSTEM_SATELLITE));
    CHECK(expect_route(s3u2_adapter, 1U, 0U, 1U, 0U, 1U, ASICEN_L3_SYSTEM_TERRESTRIAL));

    CHECK(w3u3_adapter.claim_count() == 2U);
    CHECK(w3u3_adapter.capability().enclosure_receivers == 4U);
    CHECK(w3u3_adapter.capability().public_receivers == 4U);
    CHECK(w3u3_adapter.capability().usb_functions == 2U);
    CHECK(w3u3_adapter.capability().multi_endpoint == 1U);
    CHECK(w3u3_adapter.capability().lnb_receiver_mask == 0x5U);
    CHECK(w3u3_adapter.capability().readers == 1U);
    CHECK(w3u3_adapter.capability().loader_firmware == 1U);
    CHECK(w3u3_adapter.capability().max_concurrent_streams == 0U);
    CHECK(expect_route(w3u3_adapter, 0U, 0U, 0U, 0U, 0U, ASICEN_L3_SYSTEM_SATELLITE));
    CHECK(expect_route(w3u3_adapter, 1U, 0U, 1U, 0U, 1U, ASICEN_L3_SYSTEM_TERRESTRIAL));
    CHECK(expect_route(w3u3_adapter, 2U, 1U, 0U, 1U, 0U, ASICEN_L3_SYSTEM_SATELLITE));
    CHECK(expect_route(w3u3_adapter, 3U, 1U, 1U, 1U, 1U, ASICEN_L3_SYSTEM_TERRESTRIAL));
    CHECK(!w3u3_adapter.route(4U, nullptr));

    /* V2 assigns all four receivers to the shared RF function 0 and flips the
     * public source order. */
    CHECK(v2_adapter.claim_count() == 2U);
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

    /* Combined S3U has one receiver that accepts both systems, so a bare
     * provider tune cannot choose one. */
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

bool test_provider_callbacks()
{
    FakeHost host;
    const auto* s3u = asicen::find_profile(asicen::ModelId::S3u);
    const auto* w3u3 = asicen::find_profile(asicen::ModelId::W3u3);
    CHECK(s3u != nullptr && w3u3 != nullptr);
    asicen::l3::Adapter s3u_adapter(*s3u, host);
    asicen::l3::Adapter w3u3_adapter(*w3u3, host);

    const dtv_l3_provider* provider = w3u3_adapter.provider();
    CHECK(provider != nullptr && provider->tune != nullptr && provider->bridge_start != nullptr &&
          provider->stream_attach != nullptr);
    std::uint64_t programmed = 0U;
    CHECK(provider->tune(provider->provider_context, nullptr, 0U, 11000000000ULL, &programmed) ==
          DTV_L3_OK);
    CHECK(host.last_system == ASICEN_L3_SYSTEM_SATELLITE);
    CHECK(provider->tune(provider->provider_context, nullptr, 1U, 527143000ULL, &programmed) ==
          DTV_L3_OK);
    CHECK(host.last_system == ASICEN_L3_SYSTEM_TERRESTRIAL);

    dtv_l3_scope_info scope{};
    CHECK(provider->scope_query(provider->provider_context, 2U, &scope) == DTV_L3_OK);
    CHECK(scope.bridge_id == 1U && scope.compatibility == DTV_L3_LEASE_SHAREABLE);
    CHECK(provider->scope_query(provider->provider_context, 9U, &scope) == DTV_L3_UNSUPPORTED);

    CHECK(provider->bridge_start(provider->provider_context, nullptr, 0U, 1U) == DTV_L3_OK);
    CHECK(host.begin_calls == 1U && host.last_receiver == 1U &&
          host.last_system == ASICEN_L3_SYSTEM_TERRESTRIAL && host.last_bridge == 0U);
    CHECK(provider->stream_attach(provider->provider_context, nullptr, 77U, nullptr, nullptr) ==
          DTV_L3_OK);
    CHECK(host.attach_calls == 1U && host.last_stream == 77U && host.last_receiver == 1U &&
          host.last_system == ASICEN_L3_SYSTEM_TERRESTRIAL);
    CHECK(provider->stream_stop(provider->provider_context, nullptr, 77U) == DTV_L3_OK);
    CHECK(host.stop_calls == 1U);
    CHECK(provider->stream_stop(provider->provider_context, nullptr, 77U) == DTV_L3_STALE);
    CHECK(provider->bridge_start(provider->provider_context, nullptr, 1U, 1U) ==
          DTV_L3_UNSUPPORTED);
    CHECK(provider->bridge_start(provider->provider_context, nullptr, 0U, 3U) ==
          DTV_L3_UNSUPPORTED);

    const dtv_l3_provider* s3u_provider = s3u_adapter.provider();
    CHECK(s3u_provider->tune(s3u_provider->provider_context, nullptr, 0U, 11000000000ULL,
                             &programmed) == DTV_L3_UNSUPPORTED);
    return true;
}

bool test_reader_and_lnb()
{
    FakeHost host;
    const auto* s3u = asicen::find_profile(asicen::ModelId::S3u);
    const auto* s3u2 = asicen::find_profile(asicen::ModelId::S3u2);
    const auto* w3u3 = asicen::find_profile(asicen::ModelId::W3u3);
    CHECK(s3u != nullptr && s3u2 != nullptr && w3u3 != nullptr);
    asicen::l3::Adapter s3u_adapter(*s3u, host);
    asicen::l3::Adapter s3u2_adapter(*s3u2, host);
    asicen::l3::Adapter w3u3_adapter(*w3u3, host);

    CHECK(w3u3_adapter.reader_open(0U) == DTV_L3_OK);
    CHECK(host.reader_open_calls == 1U && host.last_reader == 0U);
    CHECK(w3u3_adapter.reader_open(1U) == DTV_L3_UNSUPPORTED);
    CHECK(host.reader_open_calls == 1U);
    CHECK(w3u3_adapter.reader_reset(0U) == DTV_L3_OK);
    CHECK(host.reader_reset_calls == 1U);
    const std::uint8_t command[] = {0x00U, 0xA4U, 0x00U, 0x00U};
    std::uint8_t response[8]{};
    std::size_t response_size = 0U;
    CHECK(w3u3_adapter.reader_transmit(0U, command, sizeof(command), response, sizeof(response),
                                       &response_size) == DTV_L3_OK);
    CHECK(host.reader_transmit_calls == 1U && response_size == sizeof(command) &&
          std::memcmp(response, command, sizeof(command)) == 0);
    CHECK(w3u3_adapter.reader_transmit(1U, command, sizeof(command), response, sizeof(response),
                                       &response_size) == DTV_L3_INVALID);
    CHECK(w3u3_adapter.reader_close(0U) == DTV_L3_OK);
    CHECK(host.reader_close_calls == 1U);

    CHECK(w3u3_adapter.lnb_set(0U, 15U) == DTV_L3_OK);
    CHECK(host.lnb_calls == 1U && host.last_receiver == 0U && host.last_lnb_mode == 15U);
    CHECK(w3u3_adapter.lnb_set(1U, 15U) == DTV_L3_UNSUPPORTED);
    CHECK(w3u3_adapter.lnb_set(2U, 0U) == DTV_L3_OK);
    CHECK(host.lnb_calls == 2U);
    CHECK(w3u3_adapter.lnb_set(9U, 15U) == DTV_L3_UNSUPPORTED);
    CHECK(s3u_adapter.lnb_set(0U, 15U) == DTV_L3_UNSUPPORTED);
    CHECK(s3u2_adapter.lnb_set(0U, 15U) == DTV_L3_UNSUPPORTED);
    CHECK(host.lnb_calls == 2U);
    return true;
}

bool test_provider_claim_delegation()
{
    FakeHost host;
    const auto* w3u3 = asicen::find_profile(asicen::ModelId::W3u3);
    CHECK(w3u3 != nullptr);
    asicen::l3::Adapter adapter(*w3u3, host);
    const dtv_l3_provider* provider = adapter.provider();
    const auto claimed = provider->claim(provider->provider_context, nullptr, 1U);
    CHECK(claimed.result == DTV_L3_OK && claimed.residual_state == DTV_L3_CLAIM_RETAINED);
    CHECK(host.claim_calls == 2U);
    const auto released = provider->release_claim(provider->provider_context, nullptr, 1U);
    CHECK(released.quiescence == DTV_L3_RELEASE_QUIESCED &&
          released.residual_state == DTV_L3_CLAIM_NONE && released.error == DTV_L3_OK);
    CHECK(host.release_calls == 2U);
    return true;
}

} // namespace

int main()
{
    if (!test_capability_and_routes())
        return 1;
    if (!test_tune_semantics())
        return 1;
    if (!test_provider_callbacks())
        return 1;
    if (!test_reader_and_lnb())
        return 1;
    if (!test_provider_claim_delegation())
        return 1;
    return 0;
}
