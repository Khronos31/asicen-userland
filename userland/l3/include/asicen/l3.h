/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Experimental ASICEN L3 adapter candidate. This is not a release API. The
 * factory and capability ABI below stand in for the Core factory/capability
 * types that are still candidates in dtv/l3_owner.h, so they may move or be
 * replaced before the L3 API is fixed.
 */
#ifndef ASICEN_USERLAND_L3_H
#define ASICEN_USERLAND_L3_H

#include <dtv/l3_owner.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum asicen_l3_model {
    ASICEN_L3_MODEL_S3U = 0,
    ASICEN_L3_MODEL_S3U2 = 1,
    ASICEN_L3_MODEL_W3U2 = 2,
    ASICEN_L3_MODEL_W3U3 = 3,
    ASICEN_L3_MODEL_W3U3_V2 = 4
} asicen_l3_model;

typedef enum asicen_l3_system {
    ASICEN_L3_SYSTEM_TERRESTRIAL = 1,
    ASICEN_L3_SYSTEM_SATELLITE = 2
} asicen_l3_system;

/*
 * Product capability as seen by the existing backend. Enclosure capacity and
 * routable/public receivers are reported separately. max_concurrent_streams is
 * zero ("not established") until the runtime capture limit is measured; it is
 * not a claim that every listed receiver can stream at once.
 */
typedef struct asicen_l3_capability {
    uint32_t struct_size;
    uint32_t model;
    const char* model_key;
    uint16_t vid;
    uint16_t pid;
    uint32_t enclosure_receivers;
    uint32_t public_receivers;
    uint32_t usb_functions;
    uint32_t local_lanes;
    uint32_t combined_isdb_ts;
    /* Bitmask of ASICEN_L3_SYSTEM_* per receiver, index 0..3. */
    uint32_t receiver_systems[4];
    uint32_t readers;
    /* Bitmask of receivers with a model-specific LNB power path. */
    uint32_t lnb_receiver_mask;
    uint32_t loader_firmware;
    uint32_t multi_endpoint;
    uint32_t max_concurrent_streams;
    uint32_t reserved[3];
} asicen_l3_capability;

typedef struct asicen_l3_route {
    uint32_t valid;
    uint32_t function_index;
    uint32_t local_lane;
    uint32_t rf_function_index;
    uint32_t frontend_source;
    uint32_t system;
} asicen_l3_route;

/*
 * Seam to the existing ASICEN backend. The L3 adapter owns routing, scope, and
 * close-retention rules; the host performs the operations the current backend
 * already implements. Optional callbacks are NULL when unavailable.
 */
typedef struct asicen_l3_host_ops {
    void* context;
    dtv_l3_claim_result (*claim)(void* context, uint32_t claim_index);
    dtv_l3_release_result (*release_claim)(void* context, uint32_t claim_index);
    dtv_l3_result (*submit)(void* context, uint64_t transfer_id,
                            const dtv_l3_transfer_request* request,
                            dtv_l3_transfer_complete_fn complete, void* callback_context);
    dtv_l3_result (*cancel)(void* context, uint64_t transfer_id);
    dtv_l3_result (*tune)(void* context, uint32_t receiver, uint32_t system, uint64_t frequency_hz,
                          uint32_t timeout_ms, uint64_t* programmed_hz);
    dtv_l3_result (*stream_begin)(void* context, uint32_t receiver, uint32_t system,
                                  uint32_t bridge_id);
    dtv_l3_result (*stream_attach)(void* context, uint64_t stream_id, uint32_t receiver,
                                   uint32_t system, dtv_l3_ts_deliver_fn deliver,
                                   void* callback_context);
    dtv_l3_result (*stream_stop)(void* context, uint64_t stream_id);
    dtv_l3_result (*reader_open)(void* context, uint32_t reader_id);
    dtv_l3_result (*reader_reset)(void* context, uint32_t reader_id);
    dtv_l3_result (*reader_transmit)(void* context, uint32_t reader_id, const uint8_t* command,
                                     size_t command_size, uint8_t* response,
                                     size_t response_capacity, size_t* response_size);
    dtv_l3_result (*reader_close)(void* context, uint32_t reader_id);
    dtv_l3_result (*lnb_set)(void* context, uint32_t receiver, uint32_t mode);
} asicen_l3_host_ops;

typedef struct asicen_l3_factory asicen_l3_factory;

/* Static capability tables derived from device_profile and enclosure routing. */
const asicen_l3_capability* asicen_l3_capability_for_model(asicen_l3_model model);
const asicen_l3_capability* asicen_l3_capability_for_ids(uint16_t vid, uint16_t pid);
int asicen_l3_model_for_ids(uint16_t vid, uint16_t pid, asicen_l3_model* model);

/*
 * Creates the candidate provider for one enclosure. Claim indices are logical
 * USB functions (0..usb_functions-1). The existing backend may satisfy both
 * from one physical enclosure claim; the host decides how a claim index maps to
 * its own ownership.
 */
dtv_l3_result asicen_l3_factory_create(const asicen_l3_host_ops* host, uint16_t vid, uint16_t pid,
                                       asicen_l3_factory** factory_out);
void asicen_l3_factory_destroy(asicen_l3_factory* factory);
const dtv_l3_provider* asicen_l3_factory_provider(const asicen_l3_factory* factory);
const asicen_l3_capability* asicen_l3_factory_capability(const asicen_l3_factory* factory);
uint32_t asicen_l3_factory_claim_count(const asicen_l3_factory* factory);
int asicen_l3_factory_route(const asicen_l3_factory* factory, uint32_t receiver,
                            asicen_l3_route* route);

/*
 * Product-semantics entry points used by the library adapter. The provider
 * callbacks above remain the candidate USB-facing contract; these calls keep
 * the receiver/system and reader distinctions that the candidate provider
 * cannot yet express.
 */
dtv_l3_result asicen_l3_factory_tune(asicen_l3_factory* factory, uint32_t receiver, uint32_t system,
                                     uint64_t frequency_hz, uint32_t timeout_ms,
                                     uint64_t* programmed_hz);
dtv_l3_result asicen_l3_factory_reader_open(asicen_l3_factory* factory, uint32_t reader_id);
dtv_l3_result asicen_l3_factory_reader_reset(asicen_l3_factory* factory, uint32_t reader_id);
dtv_l3_result asicen_l3_factory_reader_transmit(asicen_l3_factory* factory, uint32_t reader_id,
                                                const uint8_t* command, size_t command_size,
                                                uint8_t* response, size_t response_capacity,
                                                size_t* response_size);
dtv_l3_result asicen_l3_factory_reader_close(asicen_l3_factory* factory, uint32_t reader_id);
dtv_l3_result asicen_l3_factory_lnb_set(asicen_l3_factory* factory, uint32_t receiver,
                                        uint32_t mode);

#ifdef __cplusplus
}
#endif

#endif /* ASICEN_USERLAND_L3_H */
