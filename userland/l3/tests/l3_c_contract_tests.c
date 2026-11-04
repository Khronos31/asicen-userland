// SPDX-License-Identifier: GPL-2.0-or-later
#define _POSIX_C_SOURCE 200809L

#include "asicen/l3.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define CHECK(condition)                                                                           \
    do                                                                                             \
    {                                                                                              \
        if (!(condition))                                                                          \
        {                                                                                          \
            fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #condition);                  \
            return 0;                                                                              \
        }                                                                                          \
    } while (0)

typedef struct fake_host {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int block_release;
    int release_allowed;
    int release_entered;
    int release_retained;
    int claim_fail_index;
    int claim_calls;
    int claim_indices[8];
    int release_calls;
    int release_indices[8];
    int tune_calls;
    unsigned int tune_receiver;
    unsigned int tune_system;
    unsigned long long tune_hz;
    int begin_calls;
    int attach_calls;
    int stop_calls;
    int reader_open_calls;
    int reader_reset_calls;
    int reader_transmit_calls;
    int reader_close_calls;
    unsigned int last_reader;
    int lnb_calls;
    unsigned int lnb_receiver;
    unsigned int lnb_mode;
    dtv_l3_ts_deliver_fn deliver;
    void* deliver_context;
} fake_host;

static void sleep_ms(long milliseconds)
{
    struct timespec delay;
    delay.tv_sec = milliseconds / 1000L;
    delay.tv_nsec = (milliseconds % 1000L) * 1000000L;
    nanosleep(&delay, NULL);
}

static dtv_l3_claim_result usb_claim(void* context, dtv_l3_provider_binding* binding,
                                     uint32_t claim_index)
{
    fake_host* fake = (fake_host*)context;
    dtv_l3_claim_result result = {DTV_L3_OK, DTV_L3_CLAIM_RETAINED};
    (void)binding;
    pthread_mutex_lock(&fake->mutex);
    if (fake->claim_calls < 8)
    {
        fake->claim_indices[fake->claim_calls] = (int)claim_index;
    }
    ++fake->claim_calls;
    if (fake->claim_fail_index == (int)claim_index)
    {
        result.result = DTV_L3_FAILED;
        result.residual_state = DTV_L3_CLAIM_NONE;
    }
    pthread_mutex_unlock(&fake->mutex);
    return result;
}

static dtv_l3_release_result usb_release_claim(void* context, dtv_l3_provider_binding* binding,
                                               uint32_t claim_index)
{
    fake_host* fake = (fake_host*)context;
    dtv_l3_release_result result = {DTV_L3_RELEASE_QUIESCED, DTV_L3_CLAIM_NONE,
                                    DTV_L3_RESTORE_NOT_ATTEMPTED, DTV_L3_OK};
    (void)binding;
    pthread_mutex_lock(&fake->mutex);
    if (fake->release_calls < 8)
    {
        fake->release_indices[fake->release_calls] = (int)claim_index;
    }
    ++fake->release_calls;
    fake->release_entered = 1;
    pthread_cond_broadcast(&fake->condition);
    while (fake->block_release != 0 && fake->release_allowed == 0)
    {
        pthread_cond_wait(&fake->condition, &fake->mutex);
    }
    if (fake->release_retained != 0)
    {
        result.quiescence = DTV_L3_RELEASE_QUIESCENCE_UNKNOWN;
        result.residual_state = DTV_L3_CLAIM_RETAINED;
        result.restoration = DTV_L3_RESTORE_UNKNOWN;
        result.error = DTV_L3_FAILED;
    }
    pthread_mutex_unlock(&fake->mutex);
    return result;
}

static dtv_l3_result usb_submit(void* context, dtv_l3_provider_binding* binding,
                                uint64_t transfer_id, const dtv_l3_transfer_request* request,
                                dtv_l3_transfer_complete_fn complete, void* callback_context)
{
    (void)context;
    (void)binding;
    (void)transfer_id;
    (void)request;
    (void)complete;
    (void)callback_context;
    return DTV_L3_UNSUPPORTED;
}

static dtv_l3_result usb_cancel(void* context, dtv_l3_provider_binding* binding,
                                uint64_t transfer_id)
{
    (void)context;
    (void)binding;
    (void)transfer_id;
    return DTV_L3_UNSUPPORTED;
}

static dtv_l3_result product_tune(void* context, uint32_t receiver, uint32_t system,
                                  uint64_t frequency_hz, uint32_t timeout_ms,
                                  uint64_t* programmed_hz)
{
    fake_host* fake = (fake_host*)context;
    (void)timeout_ms;
    ++fake->tune_calls;
    fake->tune_receiver = receiver;
    fake->tune_system = system;
    fake->tune_hz = frequency_hz;
    *programmed_hz = frequency_hz;
    return DTV_L3_OK;
}

static dtv_l3_result product_stream_begin(void* context, uint32_t receiver, uint32_t system,
                                          uint32_t bridge_id)
{
    fake_host* fake = (fake_host*)context;
    (void)receiver;
    (void)system;
    (void)bridge_id;
    ++fake->begin_calls;
    return DTV_L3_OK;
}

static dtv_l3_result product_stream_attach(void* context, uint64_t stream_id, uint32_t receiver,
                                           uint32_t system, dtv_l3_ts_deliver_fn deliver,
                                           void* callback_context)
{
    fake_host* fake = (fake_host*)context;
    (void)stream_id;
    (void)receiver;
    (void)system;
    ++fake->attach_calls;
    fake->deliver = deliver;
    fake->deliver_context = callback_context;
    return DTV_L3_OK;
}

static dtv_l3_result product_stream_stop(void* context, uint64_t stream_id)
{
    fake_host* fake = (fake_host*)context;
    (void)stream_id;
    ++fake->stop_calls;
    return DTV_L3_OK;
}

static dtv_l3_result product_reader_open(void* context, uint32_t reader_id)
{
    fake_host* fake = (fake_host*)context;
    ++fake->reader_open_calls;
    fake->last_reader = reader_id;
    return DTV_L3_OK;
}

static dtv_l3_result product_reader_reset(void* context, uint32_t reader_id)
{
    fake_host* fake = (fake_host*)context;
    ++fake->reader_reset_calls;
    fake->last_reader = reader_id;
    return DTV_L3_OK;
}

static dtv_l3_result product_reader_transmit(void* context, uint32_t reader_id,
                                             const uint8_t* command, size_t command_size,
                                             uint8_t* response, size_t response_capacity,
                                             size_t* response_size)
{
    fake_host* fake = (fake_host*)context;
    (void)response_capacity;
    ++fake->reader_transmit_calls;
    fake->last_reader = reader_id;
    if (response != NULL && command != NULL)
    {
        memcpy(response, command, command_size);
    }
    *response_size = command_size;
    return DTV_L3_OK;
}

static dtv_l3_result product_reader_close(void* context, uint32_t reader_id)
{
    fake_host* fake = (fake_host*)context;
    ++fake->reader_close_calls;
    fake->last_reader = reader_id;
    return DTV_L3_OK;
}

static dtv_l3_result product_lnb_set(void* context, uint32_t receiver, uint32_t mode)
{
    fake_host* fake = (fake_host*)context;
    ++fake->lnb_calls;
    fake->lnb_receiver = receiver;
    fake->lnb_mode = mode;
    return DTV_L3_OK;
}

static void fake_init(fake_host* fake)
{
    memset(fake, 0, sizeof(*fake));
    pthread_mutex_init(&fake->mutex, NULL);
    pthread_cond_init(&fake->condition, NULL);
    fake->claim_fail_index = -1;
    fake->last_reader = 0xffffffffU;
}

static void fake_destroy(fake_host* fake)
{
    pthread_cond_destroy(&fake->condition);
    pthread_mutex_destroy(&fake->mutex);
}

static dtv_l3_usb_provider usb_provider_for(fake_host* fake)
{
    dtv_l3_usb_provider usb;
    memset(&usb, 0, sizeof(usb));
    usb.provider_context = fake;
    usb.claim = usb_claim;
    usb.release_claim = usb_release_claim;
    usb.submit = usb_submit;
    usb.cancel = usb_cancel;
    usb.cancel_scope = DTV_L3_CANCEL_SCOPE_TRANSFER;
    return usb;
}

static asicen_l3_product_ops product_ops_for(fake_host* fake)
{
    asicen_l3_product_ops ops;
    memset(&ops, 0, sizeof(ops));
    ops.context = fake;
    ops.tune = product_tune;
    ops.stream_begin = product_stream_begin;
    ops.stream_attach = product_stream_attach;
    ops.stream_stop = product_stream_stop;
    ops.reader_open = product_reader_open;
    ops.reader_reset = product_reader_reset;
    ops.reader_transmit = product_reader_transmit;
    ops.reader_close = product_reader_close;
    ops.lnb_set = product_lnb_set;
    return ops;
}

static int wait_release_entered(fake_host* fake, int milliseconds)
{
    int waited = 0;
    while (waited < milliseconds)
    {
        pthread_mutex_lock(&fake->mutex);
        const int entered = fake->release_entered;
        pthread_mutex_unlock(&fake->mutex);
        if (entered != 0)
        {
            return 1;
        }
        sleep_ms(1);
        ++waited;
    }
    return 0;
}

static int wait_open_result(dtv_l3_context* owner, dtv_l3_open_status* status, int milliseconds)
{
    int waited = 0;
    while (waited < milliseconds)
    {
        memset(status, 0, sizeof(*status));
        if (dtv_l3_open_status_get(owner, status) == DTV_L3_OK &&
            status->state != DTV_L3_OPEN_OPENING)
        {
            return 1;
        }
        sleep_ms(1);
        ++waited;
    }
    return 0;
}

static int wait_releasable(dtv_l3_context* owner, dtv_l3_close_status* status, int milliseconds)
{
    int waited = 0;
    while (waited < milliseconds)
    {
        memset(status, 0, sizeof(*status));
        if (dtv_l3_close_status_get(owner, status) == DTV_L3_OK && status->releasable != 0)
        {
            return 1;
        }
        sleep_ms(1);
        ++waited;
    }
    return 0;
}

static int release_owner(dtv_l3_context* owner, int milliseconds)
{
    int waited = 0;
    while (waited < milliseconds)
    {
        const dtv_l3_result result = dtv_l3_context_release(owner);
        if (result == DTV_L3_OK)
        {
            return 1;
        }
        if (result != DTV_L3_BUSY)
        {
            return 0;
        }
        sleep_ms(1);
        ++waited;
    }
    return 0;
}

static dtv_l3_open_config config_for(asicen_l3_model model, const dtv_l3_firmware* firmware)
{
    dtv_l3_open_config config;
    memset(&config, 0, sizeof(config));
    config.model = (uint32_t)model;
    config.selector = 0x1234U;
    config.firmware = firmware;
    return config;
}

static int test_capabilities_and_routes(void)
{
    dtv_l3_capabilities capabilities;
    asicen_l3_model model = ASICEN_L3_MODEL_S3U;
    asicen_l3_factory* factory = NULL;
    asicen_l3_product_ops ops;
    asicen_l3_route route;
    fake_host fake;

    fake_init(&fake);
    ops = product_ops_for(&fake);

    CHECK(asicen_l3_capabilities_for_model(ASICEN_L3_MODEL_S3U, &capabilities) == DTV_L3_OK);
    CHECK(capabilities.receiver_count == 1 && capabilities.reader_count == 1);
    CHECK(capabilities.lnb_power == 0 && capabilities.serial == 0);
    CHECK(capabilities.cancel_scope == DTV_L3_CANCEL_SCOPE_TRANSFER);
    CHECK(asicen_l3_capabilities_for_model(ASICEN_L3_MODEL_S3U2, &capabilities) == DTV_L3_OK);
    CHECK(capabilities.receiver_count == 2 && capabilities.lnb_power == 0);
    CHECK(asicen_l3_capabilities_for_model(ASICEN_L3_MODEL_W3U3, &capabilities) == DTV_L3_OK);
    CHECK(capabilities.receiver_count == 4 && capabilities.reader_count == 1);
    CHECK(capabilities.lnb_power == 1);
    CHECK(asicen_l3_capabilities_for_model((asicen_l3_model)99, &capabilities) ==
          DTV_L3_UNSUPPORTED);
    CHECK(asicen_l3_capabilities_for_model(ASICEN_L3_MODEL_S3U, NULL) == DTV_L3_INVALID);

    CHECK(asicen_l3_model_for_ids(0x0b06, 0x0005, &model) == 1 && model == ASICEN_L3_MODEL_W3U3);
    CHECK(asicen_l3_model_for_ids(0x1234, 0x5678, &model) == 0);
    CHECK(asicen_l3_factory_create(NULL, 0x0b06, 0x0005, &factory) == DTV_L3_INVALID);
    CHECK(asicen_l3_factory_create(&ops, 0x1234, 0x5678, &factory) == DTV_L3_UNSUPPORTED);
    CHECK(factory == NULL);
    CHECK(asicen_l3_factory_create(&ops, 0x0b06, 0x0005, &factory) == DTV_L3_OK);
    CHECK(factory != NULL);
    CHECK(asicen_l3_factory_claim_count(factory) == 2);
    CHECK(asicen_l3_factory_adapter_factory(factory) != NULL);
    CHECK(asicen_l3_factory_route(factory, 0, &route) == 1 && route.valid == 1);
    CHECK(route.function_index == 0 && route.local_lane == 0 && route.rf_function_index == 0 &&
          route.system == ASICEN_L3_SYSTEM_SATELLITE);
    CHECK(asicen_l3_factory_route(factory, 3, &route) == 1);
    CHECK(route.function_index == 1 && route.local_lane == 1 && route.rf_function_index == 1 &&
          route.system == ASICEN_L3_SYSTEM_TERRESTRIAL);
    CHECK(asicen_l3_factory_route(factory, 4, &route) == 0);
    asicen_l3_factory_destroy(factory);
    fake_destroy(&fake);
    return 1;
}

static int test_partial_claim_and_close_retention(void)
{
    fake_host fake;
    asicen_l3_product_ops ops;
    asicen_l3_factory* factory = NULL;
    dtv_l3_usb_provider usb;
    dtv_l3_open_config config;
    dtv_l3_open_result opened;
    dtv_l3_open_status open_status;
    dtv_l3_close_status close_status;
    dtv_l3_op_result op_result;

    fake_init(&fake);
    fake.claim_fail_index = 1;
    fake.block_release = 1;
    ops = product_ops_for(&fake);
    usb = usb_provider_for(&fake);
    CHECK(asicen_l3_factory_create(&ops, 0x0b06, 0x0005, &factory) == DTV_L3_OK);
    config = config_for(ASICEN_L3_MODEL_W3U3, NULL);

    opened = dtv_l3_open_factory(&usb, asicen_l3_factory_adapter_factory(factory), &config,
                                 asicen_l3_factory_claim_count(factory));
    CHECK(opened.result == DTV_L3_ACCEPTED && opened.owner != NULL && opened.op != NULL);

    CHECK(wait_release_entered(&fake, 5000) == 1);
    memset(&open_status, 0, sizeof(open_status));
    CHECK(dtv_l3_open_status_get(opened.owner, &open_status) == DTV_L3_OK);
    CHECK(open_status.state == DTV_L3_OPEN_FAILED);
    CHECK(open_status.result == DTV_L3_FAILED);
    CHECK(open_status.claims_acquired == 1 && open_status.claims_attempted == 2);
    CHECK(open_status.cleanup_claims_remaining == 1);
    CHECK(fake.claim_calls == 2 && fake.claim_indices[0] == 0 && fake.claim_indices[1] == 1);
    CHECK(fake.release_calls == 1 && fake.release_indices[0] == 0);

    pthread_mutex_lock(&fake.mutex);
    fake.release_allowed = 1;
    pthread_cond_broadcast(&fake.condition);
    pthread_mutex_unlock(&fake.mutex);

    CHECK(wait_open_result(opened.owner, &open_status, 5000) == 1);
    CHECK(dtv_l3_op_wait(opened.op, 5000) == DTV_L3_OK);
    CHECK(dtv_l3_op_poll(opened.op, &op_result) == DTV_L3_OK);
    CHECK(op_result.state == DTV_L3_OP_FAILED && op_result.result == DTV_L3_FAILED);
    CHECK(dtv_l3_op_release(opened.op) == DTV_L3_OK);
    CHECK(wait_releasable(opened.owner, &close_status, 5000) == 1);
    CHECK(close_status.physical_quiesced == 1 && close_status.cleanup_complete == 1);
    CHECK(close_status.all_ops_recovered == 1 && close_status.releasable == 1);
    CHECK(release_owner(opened.owner, 5000) == 1);

    asicen_l3_factory_destroy(factory);
    fake_destroy(&fake);
    return 1;
}

static int test_product_semantics_through_core(void)
{
    fake_host fake;
    asicen_l3_product_ops ops;
    asicen_l3_factory* factory = NULL;
    dtv_l3_usb_provider usb;
    dtv_l3_open_config config;
    dtv_l3_open_result opened;
    dtv_l3_close_status close_status;
    dtv_l3_capabilities capabilities;
    dtv_l3_lease* lease = NULL;
    dtv_l3_stream* stream = NULL;
    dtv_l3_reader* reader = NULL;
    dtv_l3_lease_status lease_status;
    uint64_t programmed = 0U;
    uint64_t revision = 0U;
    uint8_t ts_data[376];
    uint8_t out[188];
    size_t bytes = 0U;
    dtv_l3_read_state read_state = DTV_L3_READ_WAIT;

    fake_init(&fake);
    ops = product_ops_for(&fake);
    usb = usb_provider_for(&fake);
    CHECK(asicen_l3_factory_create(&ops, 0x0b06, 0x0005, &factory) == DTV_L3_OK);
    config = config_for(ASICEN_L3_MODEL_W3U3, NULL);
    opened = dtv_l3_open_factory(&usb, asicen_l3_factory_adapter_factory(factory), &config, 2);
    CHECK(opened.result == DTV_L3_ACCEPTED && opened.owner != NULL && opened.op != NULL);
    CHECK(dtv_l3_op_wait(opened.op, 5000) == DTV_L3_OK);
    CHECK(dtv_l3_op_release(opened.op) == DTV_L3_OK);
    CHECK(dtv_l3_capabilities_get(opened.owner, &capabilities) == DTV_L3_OK);
    CHECK(capabilities.receiver_count == 4 && capabilities.reader_count == 1);
    CHECK(capabilities.lnb_power == 1 && capabilities.cancel_scope == DTV_L3_CANCEL_SCOPE_TRANSFER);

    CHECK(dtv_l3_lease_acquire(opened.owner, 0U, &lease) == DTV_L3_OK && lease != NULL);
    CHECK(dtv_l3_lease_tune(lease, 11000000000ULL, &programmed, &revision) == DTV_L3_OK);
    CHECK(fake.tune_calls == 1 && fake.tune_receiver == 0U &&
          fake.tune_system == ASICEN_L3_SYSTEM_SATELLITE && fake.tune_hz == 11000000000ULL &&
          programmed == 11000000000ULL);
    CHECK(dtv_l3_lease_start(lease) == DTV_L3_OK);
    CHECK(dtv_l3_lease_attach(lease, &stream) == DTV_L3_OK && stream != NULL);
    CHECK(fake.begin_calls == 1 && fake.attach_calls == 1 && fake.deliver != NULL);
    memset(ts_data, 0x47, sizeof(ts_data));
    fake.deliver(fake.deliver_context, ts_data, sizeof(ts_data));
    CHECK(dtv_l3_stream_read_ts(stream, out, sizeof(out), &bytes, &read_state) == DTV_L3_OK);
    CHECK(read_state == DTV_L3_READ_DATA && bytes == sizeof(out));
    CHECK(dtv_l3_lease_status_get(lease, &lease_status) == DTV_L3_OK);
    CHECK(dtv_l3_stream_stop(stream, 0) == DTV_L3_OK);
    CHECK(fake.stop_calls == 1);
    CHECK(dtv_l3_stream_release(stream, DTV_L3_STREAM_RELEASE_DISCARD_UNREAD) == DTV_L3_OK);
    CHECK(dtv_l3_stream_destroy(stream) == DTV_L3_OK);
    CHECK(dtv_l3_lease_release(lease) == DTV_L3_OK);

    CHECK(dtv_l3_reader_acquire(opened.owner, 0U, &reader) == DTV_L3_OK && reader != NULL);
    CHECK(fake.reader_open_calls == 1 && fake.last_reader == 0U);
    CHECK(dtv_l3_lnb_set(opened.owner, 0U, 15U) == DTV_L3_OK);
    CHECK(fake.lnb_calls == 1 && fake.lnb_receiver == 0U && fake.lnb_mode == 15U);
    CHECK(dtv_l3_lnb_set(opened.owner, 1U, 15U) == DTV_L3_UNSUPPORTED);
    CHECK(fake.lnb_calls == 1);

    CHECK(dtv_l3_close(opened.owner, 2000) == DTV_L3_OK);
    CHECK(wait_releasable(opened.owner, &close_status, 5000) == 1);
    CHECK(release_owner(opened.owner, 5000) == 1);
    asicen_l3_factory_destroy(factory);
    fake_destroy(&fake);
    return 1;
}

static int test_retention_and_retry(void)
{
    fake_host fake;
    asicen_l3_product_ops ops;
    asicen_l3_factory* factory = NULL;
    dtv_l3_usb_provider usb;
    dtv_l3_open_config config;
    dtv_l3_open_result opened;
    dtv_l3_open_status open_status;
    dtv_l3_close_status close_status;

    fake_init(&fake);
    fake.release_retained = 1;
    ops = product_ops_for(&fake);
    usb = usb_provider_for(&fake);
    CHECK(asicen_l3_factory_create(&ops, 0x0b06, 0x0005, &factory) == DTV_L3_OK);
    config = config_for(ASICEN_L3_MODEL_W3U3, NULL);
    opened = dtv_l3_open_factory(&usb, asicen_l3_factory_adapter_factory(factory), &config, 2);
    CHECK(opened.result == DTV_L3_ACCEPTED && opened.owner != NULL && opened.op != NULL);
    CHECK(dtv_l3_op_wait(opened.op, 5000) == DTV_L3_OK);
    memset(&open_status, 0, sizeof(open_status));
    CHECK(dtv_l3_open_status_get(opened.owner, &open_status) == DTV_L3_OK);
    CHECK(open_status.state == DTV_L3_OPEN_READY && open_status.result == DTV_L3_OK);
    CHECK(open_status.claims_acquired == 2);
    CHECK(dtv_l3_op_release(opened.op) == DTV_L3_OK);

    CHECK(dtv_l3_close(opened.owner, 0) == DTV_L3_TIMEOUT);
    int waited = 0;
    while (waited < 5000)
    {
        pthread_mutex_lock(&fake.mutex);
        const int calls = fake.release_calls;
        pthread_mutex_unlock(&fake.mutex);
        if (calls >= 2)
        {
            break;
        }
        sleep_ms(1);
        ++waited;
    }
    CHECK(waited < 5000);
    memset(&close_status, 0, sizeof(close_status));
    CHECK(dtv_l3_close_status_get(opened.owner, &close_status) == DTV_L3_OK);
    CHECK(close_status.physical_quiesced == 0 && close_status.cleanup_complete == 0);
    CHECK(close_status.releasable == 0);
    CHECK(dtv_l3_context_release(opened.owner) == DTV_L3_BUSY);

    pthread_mutex_lock(&fake.mutex);
    fake.release_retained = 0;
    pthread_mutex_unlock(&fake.mutex);
    CHECK(dtv_l3_cleanup_retry(opened.owner) == DTV_L3_ACCEPTED);
    CHECK(wait_releasable(opened.owner, &close_status, 5000) == 1);
    CHECK(close_status.physical_quiesced == 1 && close_status.cleanup_complete == 1);
    CHECK(dtv_l3_close(opened.owner, 0) == DTV_L3_OK);
    CHECK(release_owner(opened.owner, 5000) == 1);

    CHECK(fake.claim_calls == 2 && fake.claim_indices[0] == 0 && fake.claim_indices[1] == 1);
    CHECK(fake.release_calls >= 4 && fake.release_indices[0] == 1 && fake.release_indices[1] == 0);
    asicen_l3_factory_destroy(factory);
    fake_destroy(&fake);
    return 1;
}

static int test_explicit_system_tune(void)
{
    fake_host fake;
    asicen_l3_product_ops ops;
    asicen_l3_factory* factory = NULL;
    const dtv_l3_adapter_factory* adapter_factory = NULL;
    const dtv_l3_product_adapter* product = NULL;
    void* owner = NULL;
    dtv_l3_open_config config;
    uint64_t programmed = 0U;

    fake_init(&fake);
    ops = product_ops_for(&fake);
    CHECK(asicen_l3_factory_create(&ops, 0x0b06, 0x0001, &factory) == DTV_L3_OK);
    adapter_factory = asicen_l3_factory_adapter_factory(factory);
    CHECK(adapter_factory != NULL && adapter_factory->create != NULL);
    config = config_for(ASICEN_L3_MODEL_S3U, NULL);
    CHECK(adapter_factory->create(adapter_factory->factory_context, &config, &product, &owner) ==
          DTV_L3_OK);
    CHECK(product != NULL && owner != NULL);
    CHECK(asicen_l3_product_tune(product, 0U, ASICEN_L3_SYSTEM_TERRESTRIAL, 527143000ULL, 2500U,
                                 &programmed) == DTV_L3_OK);
    CHECK(fake.tune_calls == 1U && fake.tune_system == ASICEN_L3_SYSTEM_TERRESTRIAL &&
          fake.tune_hz == 527143000ULL && programmed == 527143000ULL);
    CHECK(asicen_l3_product_tune(product, 0U, 0U, 11000000000ULL, 2500U, &programmed) ==
          DTV_L3_UNSUPPORTED);
    CHECK(fake.tune_calls == 1U);
    CHECK(asicen_l3_product_tune(NULL, 0U, ASICEN_L3_SYSTEM_TERRESTRIAL, 527143000ULL, 2500U,
                                 &programmed) == DTV_L3_INVALID);
    adapter_factory->release(owner);
    asicen_l3_factory_destroy(factory);
    fake_destroy(&fake);
    return 1;
}

static int test_factory_rejects_wrong_model(void)
{
    fake_host fake;
    asicen_l3_product_ops ops;
    asicen_l3_factory* factory = NULL;
    dtv_l3_usb_provider usb;
    dtv_l3_open_config config;
    dtv_l3_open_result opened;

    fake_init(&fake);
    ops = product_ops_for(&fake);
    usb = usb_provider_for(&fake);
    CHECK(asicen_l3_factory_create(&ops, 0x0b06, 0x0005, &factory) == DTV_L3_OK);
    config = config_for(ASICEN_L3_MODEL_S3U, NULL);
    opened = dtv_l3_open_factory(&usb, asicen_l3_factory_adapter_factory(factory), &config, 2);
    CHECK(opened.result == DTV_L3_UNSUPPORTED);
    CHECK(opened.owner == NULL && opened.op == NULL);
    CHECK(fake.claim_calls == 0);
    asicen_l3_factory_destroy(factory);
    fake_destroy(&fake);
    return 1;
}

int main(void)
{
    if (!test_capabilities_and_routes())
        return 1;
    if (!test_partial_claim_and_close_retention())
        return 1;
    if (!test_product_semantics_through_core())
        return 1;
    if (!test_retention_and_retry())
        return 1;
    if (!test_explicit_system_tune())
        return 1;
    if (!test_factory_rejects_wrong_model())
        return 1;
    return 0;
}
