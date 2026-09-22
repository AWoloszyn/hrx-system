// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Native shared-memory transport for independent processes on one host.

#ifndef IREE_NET_CARRIER_SHM_FACTORY_H_
#define IREE_NET_CARRIER_SHM_FACTORY_H_

#include "iree/net/carrier/shm/carrier.h"
#include "iree/net/carrier/shm/region.h"
#include "iree/net/transport_factory.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

typedef struct iree_net_shm_factory_options_t {
  // Exact shared geometry offered by listeners. Outbound connections accept
  // offers at or below each dimension. Slot size/count bound resident storage,
  // not message size; larger messages progress through repeated slot reuse.
  iree_net_shm_region_options_t region;
  // Per-endpoint private send admission and inline generated-prefix capacity.
  iree_net_shm_carrier_options_t carrier;
  // Maximum simultaneous native accepts/import handshakes per listener.
  // Published connections release their slot and do not count against this.
  uint32_t max_pending_connections;
} iree_net_shm_factory_options_t;

static inline iree_net_shm_factory_options_t
iree_net_shm_factory_options_default(void) {
  return (iree_net_shm_factory_options_t){
      /*.region=*/{4u, 16u, 64u * 1024u},
      /*.carrier=*/{16u, 16u * 1024u},
      /*.max_pending_connections=*/16u,
  };
}

// Creates a generic reliable, ordered transport without HAL dependencies.
// Returns UNAVAILABLE before allocating resources when native shared
// notifications are unsupported. macOS requires 14.4 or newer and a build SDK
// providing the public shared-address wait APIs; older deployment targets may
// still load the runtime and use other transports.
// Windows listener/connect admission returns UNAVAILABLE before acquiring
// resources if the proactor lacks WAIT_COMPLETION_PACKET support, which is
// required for persistent monitoring of the native control pipe.
//
// POSIX addresses are Unix socket filesystem paths, or Linux/Android abstract
// names prefixed with '@'. Listeners never remove an existing path on bind;
// stop removes only the pathname entry they created. Place filesystem sockets
// in a directory with the desired access policy. Windows addresses are local
// pipe names without path separators or the \\.\pipe\ prefix; the process's
// default security descriptor applies and remote pipe clients are rejected.
// A busy Windows pipe reports UNAVAILABLE without a blocking wait or retry.
//
// OFFER/ACCEPT/READY imports anonymous mappings and native wake resources
// before either public success callback. The native stream remains open for
// peer EOF. Shared receive leases can outlive endpoints, connections, and
// proactors. Private TX is copied into shared slots; this transport does not
// claim zero-copy TX. The generic receive_pool arguments are unused.
//
// Peer handshake failures retire only that accepted peer. A native listener
// setup error closes acceptance and reports an error callback; explicit stop
// still joins every outstanding operation before the listener can be freed.
IREE_API_EXPORT iree_status_t
iree_net_shm_factory_create(const iree_net_shm_factory_options_t* options,
                            iree_allocator_t host_allocator,
                            iree_net_transport_factory_t** out_factory);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_NET_CARRIER_SHM_FACTORY_H_
