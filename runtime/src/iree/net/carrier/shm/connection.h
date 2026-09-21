// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_NET_CARRIER_SHM_CONNECTION_H_
#define IREE_NET_CARRIER_SHM_CONNECTION_H_

#include "iree/async/socket.h"
#include "iree/async/util/local_stream.h"
#include "iree/net/carrier/shm/carrier.h"
#include "iree/net/connection.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Bootstrap channel ownership, transferred to the connection after READY.
// The stream borrows the native owner below through its complete drain.
typedef struct iree_net_shm_connection_channel_t {
  // Owned helper; transfers and deactivation run only on its poll owner.
  iree_async_local_stream_t* stream;
#if defined(IREE_PLATFORM_WINDOWS)
  // Owned overlapped local pipe, never associated with an IOCP.
  iree_async_primitive_t pipe;
#else
  // Owned managed Unix stream socket; also owns its native descriptor.
  iree_async_socket_t* socket;
#endif
} iree_net_shm_connection_channel_t;

// Destroys and clears a channel after its helper has fully deactivated.
// An empty bundle, including a NULL helper, is permitted.
void iree_net_shm_connection_channel_deinitialize(
    iree_net_shm_connection_channel_t* channel);

// Allocates all endpoint stacks and their shared notification before
// publication. Imported resources must already have been confirmed by READY.
// Retains |proactor| and |storage|. Accepts no async work; an unpublished
// connection can be released directly. Published connections require the normal
// connection deactivation contract before release.
iree_status_t iree_net_shm_connection_create(
    iree_async_proactor_t* proactor, iree_net_shm_storage_t* storage,
    const iree_net_shm_carrier_options_t* options,
    iree_allocator_t host_allocator, iree_net_connection_t** out_connection);

// Transfers and clears |channel| after READY, then begins peer EOF observation.
// Called exactly once on the owning poll thread before the public connection
// callback. The channel must be connected and its helper idle and healthy.
// All fallible allocation/import work precedes this trusted ownership handoff.
void iree_net_shm_connection_publish(
    iree_net_connection_t* connection,
    iree_net_shm_connection_channel_t* channel);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_NET_CARRIER_SHM_CONNECTION_H_
