/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Experimental ASICEN L3 adapter candidate. This is not a release API. It uses
 * the Core candidate dtv_l3_usb_provider / dtv_l3_product_adapter /
 * dtv_l3_adapter_factory split and only adds the ASICEN product seam.
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

typedef struct asicen_l3_route {
    uint32_t valid;
    uint32_t function_index;
    uint32_t local_lane;
    uint32_t rf_function_index;
    uint32_t frontend_source;
    uint32_t system;
} asicen_l3_route;

/*
 * Product-semantics seam to the existing ASICEN backend. Core's USB-only
 * provider stays separate; this seam carries tune, stream, CARD, and LNB work
 * that the existing backend already implements. Optional callbacks are NULL
 * when unavailable.
 */
typedef struct asicen_l3_product_ops {
    void* context;
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
} asicen_l3_product_ops;

typedef struct asicen_l3_factory asicen_l3_factory;

/* Static capability table derived from device_profile. */
dtv_l3_result asicen_l3_capabilities_for_model(asicen_l3_model model,
                                               dtv_l3_capabilities* capabilities);
int asicen_l3_model_for_ids(uint16_t vid, uint16_t pid, asicen_l3_model* model);

/*
 * Creates the ASICEN product adapter factory. Core calls its create callback
 * per open with a dtv_l3_open_config (model, selector, firmware). claim_count
 * is the number of logical USB functions the host must claim; the existing
 * backend may satisfy them from one enclosure claim.
 */
dtv_l3_result asicen_l3_factory_create(const asicen_l3_product_ops* ops, uint16_t vid, uint16_t pid,
                                       asicen_l3_factory** factory_out);
void asicen_l3_factory_destroy(asicen_l3_factory* factory);
const dtv_l3_adapter_factory* asicen_l3_factory_adapter_factory(const asicen_l3_factory* factory);
uint32_t asicen_l3_factory_claim_count(const asicen_l3_factory* factory);
int asicen_l3_factory_route(const asicen_l3_factory* factory, uint32_t receiver,
                            asicen_l3_route* route);

/* Product-controlled explicit-system tune, including the S3U T/S case. */
dtv_l3_result asicen_l3_product_tune(const dtv_l3_product_adapter* adapter, uint32_t receiver,
                                     uint32_t system, uint64_t frequency_hz, uint32_t timeout_ms,
                                     uint64_t* programmed_hz);

#ifdef __cplusplus
}
#endif

#endif /* ASICEN_USERLAND_L3_H */
