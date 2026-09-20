// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_NET_CARRIER_SHM_HANDSHAKE_H_
#define IREE_NET_CARRIER_SHM_HANDSHAKE_H_

#include "iree/net/carrier/shm/bootstrap.h"
#include "iree/net/carrier/shm/connection.h"
#include "iree/net/carrier/shm/storage.h"
#include "iree/net/transport_factory.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

typedef enum iree_net_shm_handshake_role_e {
  IREE_NET_SHM_HANDSHAKE_ROLE_SERVER = 0,
  IREE_NET_SHM_HANDSHAKE_ROLE_CLIENT = 1,
} iree_net_shm_handshake_role_t;

typedef enum iree_net_shm_handshake_phase_e {
  IREE_NET_SHM_HANDSHAKE_PHASE_SERVER_BEGIN = 0,
  IREE_NET_SHM_HANDSHAKE_PHASE_SERVER_RECEIVE_ACCEPT,
  IREE_NET_SHM_HANDSHAKE_PHASE_SERVER_SEND_READY,
  IREE_NET_SHM_HANDSHAKE_PHASE_CLIENT_BEGIN,
  IREE_NET_SHM_HANDSHAKE_PHASE_CLIENT_SEND_ACCEPT,
  IREE_NET_SHM_HANDSHAKE_PHASE_CLIENT_RECEIVE_READY,
  IREE_NET_SHM_HANDSHAKE_PHASE_CLIENT_VALIDATE_READY,
  IREE_NET_SHM_HANDSHAKE_PHASE_PUBLISH,
  IREE_NET_SHM_HANDSHAKE_PHASE_DRAINING,
  IREE_NET_SHM_HANDSHAKE_PHASE_COMPLETE,
} iree_net_shm_handshake_phase_t;

// One owner-thread resource-import transaction, embedded in the native accept
// or connect owner. All holds retire before its callback; no separate free or
// deinitialize is needed after completion. Storage must remain stable until
// then. The callback may destroy or reuse this object.
typedef struct iree_net_shm_handshake_t {
  // Side determining the exact import/export protocol and handle ownership.
  iree_net_shm_handshake_role_t role;
  // Next action after the current exact transfer completes.
  iree_net_shm_handshake_phase_t phase;
  // Borrowed executor, retained by the containing accept/connect owner.
  iree_async_proactor_t* proactor;
  // Borrowed immutable server geometry or client per-dimension resource limits.
  const iree_net_shm_region_layout_t* limits;
  // Borrowed immutable process-local carrier admission configuration.
  const iree_net_shm_carrier_options_t* carrier_options;
  // Owned native stream, transferred to a successful connection after READY.
  iree_net_shm_connection_channel_t channel;
  // Owned mapping and exported wakes; source handles remain live through
  // ACCEPT.
  iree_net_shm_storage_t* storage;
  // Owned inactive connection, allocated before acknowledging readiness.
  iree_net_connection_t* connection;
  // Borrowed server exports or owned client imports; import consumes/clears
  // them.
  iree_async_primitive_t handles[IREE_NET_SHM_STORAGE_HANDLE_COUNT];
  // Reusable exact-record buffer, borrowed by one transfer at a time.
  uint8_t record[IREE_NET_SHM_BOOTSTRAP_OFFER_SIZE];
  // First failure retained through native stream deactivation.
  iree_status_t status;
  // Terminal callback receiving an owned live connection or a drained failure.
  iree_net_transport_connect_callback_t callback;
  // Allocator for connection and detached storage construction.
  iree_allocator_t host_allocator;
} iree_net_shm_handshake_t;

// Begins one handshake on the poll owner. Consumes and clears |channel|, whose
// connected helper must be healthy and idle. |limits| and |carrier_options|
// have already been validated and remain live through the callback. Completion
// is always deferred, including construction failure. Success publishes a
// fully usable connection with peer-EOF observation, not just imported handles.
void iree_net_shm_handshake_begin(
    iree_net_shm_handshake_t* handshake, iree_net_shm_handshake_role_t role,
    iree_async_proactor_t* proactor, const iree_net_shm_region_layout_t* limits,
    const iree_net_shm_carrier_options_t* carrier_options,
    iree_net_shm_connection_channel_t* channel,
    iree_net_transport_connect_callback_t callback,
    iree_allocator_t host_allocator);

// Cancels an incomplete transaction on its poll owner. Idempotent while drain
// is pending. The original callback joins all native resource ownership.
void iree_net_shm_handshake_cancel(iree_net_shm_handshake_t* handshake);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_NET_CARRIER_SHM_HANDSHAKE_H_
