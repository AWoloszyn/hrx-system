// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Ordered byte movement over independently recyclable shared payload slots.
//
// Sends borrow private sources through consumption of their final descriptor.
// Fixed-size slots bound resident storage, not logical message size. Receivers
// may retain up to N-1 native slots; the remaining slot delivers borrowed bytes
// so retention cannot stop progress. Native leases retain mapping/wake
// ownership independently of the carrier, its connection, and its proactor.

#ifndef IREE_NET_CARRIER_SHM_CARRIER_H_
#define IREE_NET_CARRIER_SHM_CARRIER_H_

#include "iree/async/notification.h"
#include "iree/net/carrier.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

typedef struct iree_net_shm_storage_t iree_net_shm_storage_t;

// Borrowed source spans accepted per send; a generated prefix is additional.
#define IREE_NET_SHM_MAX_SEND_SPANS 64u

// Bounded process-local admission resources, independent of shared geometry.
typedef struct iree_net_shm_carrier_options_t {
  // Maximum accepted sends per endpoint, including prefix writers in flight.
  uint32_t max_send_operations;
  // Generated-prefix bytes per send without allocation. Larger prefixes use
  // completion-scoped storage; this is not a logical message size limit.
  uint32_t generated_prefix_capacity;
} iree_net_shm_carrier_options_t;

static inline iree_net_shm_carrier_options_t
iree_net_shm_carrier_options_default(void) {
  return (iree_net_shm_carrier_options_t){16u, 16u * 1024u};
}

// Creates an inactive carrier over one endpoint of validated/imported storage.
// Retains |proactor|, |storage| and its single local polling |notification|.
// The connection shares this notification across all of its local endpoints;
// there must be exactly one carrier for each endpoint on each side. Creation
// accepts no async work. Install handlers and activate before use.
iree_status_t iree_net_shm_carrier_create(
    iree_async_proactor_t* proactor, iree_net_shm_storage_t* storage,
    iree_async_notification_t* notification, uint32_t endpoint_ordinal,
    const iree_net_shm_carrier_options_t* options,
    iree_allocator_t host_allocator, iree_net_carrier_t** out_carrier);

// Delivers a connection-wide terminal failure on the owning poll thread.
// Consumes |status|. Created endpoints retain the failure so activation cannot
// race past fanout, but receive no callback. Fully drained endpoints ignore it.
// The connection must retain the carrier throughout this call.
void iree_net_shm_carrier_fail(iree_net_carrier_t* carrier,
                               iree_status_t status);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_NET_CARRIER_SHM_CARRIER_H_
