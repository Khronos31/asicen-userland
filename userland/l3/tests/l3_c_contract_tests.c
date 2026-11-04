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
} fake_host;

static void sleep_ms(long milliseconds)
{
    struct timespec delay;
    delay.tv_sec = milliseconds / 1000L;
    delay.tv_nsec = (milliseconds % 1000L) * 1000000L;
    nanosleep(&delay, NULL);
}

static dtv_l3_claim_result fake_claim(void* context, uint32_t claim_index)
{
    fake_host* fake = (fake_host*)context;
    dtv_l3_claim_result result = {DTV_L3_OK, DTV_L3_CLAIM_RETAINED};
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

static dtv_l3_release_result fake_release(void* context, uint32_t claim_index)
{
    fake_host* fake = (fake_host*)context;
    dtv_l3_release_result result = {DTV_L3_RELEASE_QUIESCED, DTV_L3_CLAIM_NONE,
                                    DTV_L3_RESTORE_NOT_ATTEMPTED, DTV_L3_OK};
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

static dtv_l3_result fake_submit(void* context, uint64_t transfer_id,
                                 const dtv_l3_transfer_request* request,
                                 dtv_l3_transfer_complete_fn complete, void* callback_context)
{
    (void)context;
    (void)transfer_id;
    (void)request;
    (void)complete;
    (void)callback_context;
    return DTV_L3_UNSUPPORTED;
}

static dtv_l3_result fake_cancel(void* context, uint64_t transfer_id)
{
    (void)context;
    (void)transfer_id;
    return DTV_L3_UNSUPPORTED;
}

static void fake_init(fake_host* fake)
{
    memset(fake, 0, sizeof(*fake));
    pthread_mutex_init(&fake->mutex, NULL);
    pthread_cond_init(&fake->condition, NULL);
    fake->claim_fail_index = -1;
}

static void fake_destroy(fake_host* fake)
{
    pthread_cond_destroy(&fake->condition);
    pthread_mutex_destroy(&fake->mutex);
}

static asicen_l3_host_ops fake_ops(fake_host* fake)
{
    asicen_l3_host_ops ops;
    memset(&ops, 0, sizeof(ops));
    ops.context = fake;
    ops.claim = fake_claim;
    ops.release_claim = fake_release;
    ops.submit = fake_submit;
    ops.cancel = fake_cancel;
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

static int test_capabilities(void)
{
    const asicen_l3_capability* s3u = asicen_l3_capability_for_model(ASICEN_L3_MODEL_S3U);
    const asicen_l3_capability* s3u2 = asicen_l3_capability_for_model(ASICEN_L3_MODEL_S3U2);
    const asicen_l3_capability* w3u3 = asicen_l3_capability_for_model(ASICEN_L3_MODEL_W3U3);
    const asicen_l3_capability* v2 = asicen_l3_capability_for_model(ASICEN_L3_MODEL_W3U3_V2);
    CHECK(s3u != NULL && s3u2 != NULL && w3u3 != NULL && v2 != NULL);
    CHECK(s3u->struct_size == sizeof(asicen_l3_capability));
    CHECK(strcmp(s3u->model_key, "s3u") == 0 && s3u->vid == 0x0b06 && s3u->pid == 0x0001);
    CHECK(s3u->enclosure_receivers == 1 && s3u->public_receivers == 1);
    CHECK(s3u->usb_functions == 1 && s3u->combined_isdb_ts == 1);
    CHECK(s3u->receiver_systems[0] == (ASICEN_L3_SYSTEM_TERRESTRIAL | ASICEN_L3_SYSTEM_SATELLITE));
    CHECK(s3u->lnb_receiver_mask == 0 && s3u->multi_endpoint == 0);
    CHECK(s3u2->enclosure_receivers == 2 && s3u2->public_receivers == 2);
    CHECK(s3u2->receiver_systems[0] == ASICEN_L3_SYSTEM_SATELLITE);
    CHECK(s3u2->receiver_systems[1] == ASICEN_L3_SYSTEM_TERRESTRIAL);
    CHECK(s3u2->lnb_receiver_mask == 0);
    CHECK(w3u3->enclosure_receivers == 4 && w3u3->public_receivers == 4);
    CHECK(w3u3->usb_functions == 2 && w3u3->multi_endpoint == 1);
    CHECK(w3u3->receiver_systems[0] == ASICEN_L3_SYSTEM_SATELLITE);
    CHECK(w3u3->receiver_systems[1] == ASICEN_L3_SYSTEM_TERRESTRIAL);
    CHECK(w3u3->receiver_systems[2] == ASICEN_L3_SYSTEM_SATELLITE);
    CHECK(w3u3->receiver_systems[3] == ASICEN_L3_SYSTEM_TERRESTRIAL);
    CHECK(w3u3->lnb_receiver_mask == 0x5U && w3u3->readers == 1);
    CHECK(w3u3->loader_firmware == 1 && w3u3->max_concurrent_streams == 0);
    CHECK(strcmp(v2->model_key, "w3u3-v2") == 0 && v2->pid == 0x0006);
    CHECK(asicen_l3_capability_for_ids(0x0b06, 0x0005) ==
          asicen_l3_capability_for_ids(0x0b06, 0x0005));
    CHECK(asicen_l3_capability_for_ids(0x1234, 0x5678) == NULL);
    return 1;
}

static int test_routes_and_factory_rejects(void)
{
    asicen_l3_model model = ASICEN_L3_MODEL_S3U;
    asicen_l3_factory* factory = NULL;
    asicen_l3_route route;
    fake_host fake;
    asicen_l3_host_ops ops;
    fake_init(&fake);
    ops = fake_ops(&fake);

    CHECK(asicen_l3_model_for_ids(0x0b06, 0x0005, &model) == 1 && model == ASICEN_L3_MODEL_W3U3);
    CHECK(asicen_l3_model_for_ids(0x1234, 0x5678, &model) == 0);
    CHECK(asicen_l3_factory_create(NULL, 0x0b06, 0x0005, &factory) == DTV_L3_INVALID);
    CHECK(asicen_l3_factory_create(&ops, 0x1234, 0x5678, &factory) == DTV_L3_UNSUPPORTED);
    CHECK(factory == NULL);

    CHECK(asicen_l3_factory_create(&ops, 0x0b06, 0x0005, &factory) == DTV_L3_OK);
    CHECK(factory != NULL);
    CHECK(asicen_l3_factory_claim_count(factory) == 2);
    CHECK(asicen_l3_factory_route(factory, 0, &route) == 1 && route.valid == 1);
    CHECK(route.function_index == 0 && route.local_lane == 0 && route.rf_function_index == 0 &&
          route.system == ASICEN_L3_SYSTEM_SATELLITE);
    CHECK(asicen_l3_factory_route(factory, 3, &route) == 1);
    CHECK(route.function_index == 1 && route.local_lane == 1 && route.rf_function_index == 1 &&
          route.system == ASICEN_L3_SYSTEM_TERRESTRIAL);
    CHECK(asicen_l3_factory_route(factory, 4, &route) == 0);
    CHECK(asicen_l3_factory_provider(factory) != NULL);
    CHECK(asicen_l3_factory_capability(factory)->pid == 0x0005);
    asicen_l3_factory_destroy(factory);
    fake_destroy(&fake);
    return 1;
}

static int test_partial_claim_and_close_retention(void)
{
    fake_host fake;
    asicen_l3_host_ops ops;
    asicen_l3_factory* factory = NULL;
    const dtv_l3_provider* provider = NULL;
    dtv_l3_open_result opened;
    dtv_l3_open_status open_status;
    dtv_l3_close_status close_status;
    dtv_l3_op_result op_result;

    fake_init(&fake);
    fake.claim_fail_index = 1;
    fake.block_release = 1;
    ops = fake_ops(&fake);
    CHECK(asicen_l3_factory_create(&ops, 0x0b06, 0x0005, &factory) == DTV_L3_OK);
    provider = asicen_l3_factory_provider(factory);
    CHECK(provider != NULL);

    opened = dtv_l3_open(provider, asicen_l3_factory_claim_count(factory));
    CHECK(opened.result == DTV_L3_ACCEPTED && opened.owner != NULL && opened.op != NULL);

    /* The first successful claim is held while cleanup is blocked, so the
     * partial open is observable before the claim is reversed. */
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
    CHECK(dtv_l3_context_release(opened.owner) == DTV_L3_OK);

    asicen_l3_factory_destroy(factory);
    fake_destroy(&fake);
    return 1;
}

static int test_retention_and_retry(void)
{
    fake_host fake;
    asicen_l3_host_ops ops;
    asicen_l3_factory* factory = NULL;
    const dtv_l3_provider* provider = NULL;
    dtv_l3_open_result opened;
    dtv_l3_open_status open_status;
    dtv_l3_close_status close_status;

    fake_init(&fake);
    fake.release_retained = 1;
    ops = fake_ops(&fake);
    CHECK(asicen_l3_factory_create(&ops, 0x0b06, 0x0005, &factory) == DTV_L3_OK);
    provider = asicen_l3_factory_provider(factory);

    opened = dtv_l3_open(provider, asicen_l3_factory_claim_count(factory));
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
    CHECK(dtv_l3_context_release(opened.owner) == DTV_L3_OK);

    CHECK(fake.claim_calls == 2 && fake.claim_indices[0] == 0 && fake.claim_indices[1] == 1);
    CHECK(fake.release_calls >= 4 && fake.release_indices[0] == 1 && fake.release_indices[1] == 0);
    asicen_l3_factory_destroy(factory);
    fake_destroy(&fake);
    return 1;
}

int main(void)
{
    if (!test_capabilities())
        return 1;
    if (!test_routes_and_factory_rejects())
        return 1;
    if (!test_partial_claim_and_close_retention())
        return 1;
    if (!test_retention_and_retry())
        return 1;
    return 0;
}
