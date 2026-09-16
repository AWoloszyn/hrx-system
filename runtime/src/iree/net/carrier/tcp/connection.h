// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Multiplexed message connection over one TCP carrier.

#ifndef IREE_NET_CARRIER_TCP_CONNECTION_H_
#define IREE_NET_CARRIER_TCP_CONNECTION_H_

#include "iree/async/buffer_pool.h"
#include "iree/async/proactor.h"
#include "iree/async/socket.h"
#include "iree/base/api.h"
#include "iree/net/carrier/tcp/carrier.h"
#include "iree/net/connection.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Default number of ordinal message endpoints exposed by a connection.
#define IREE_NET_TCP_DEFAULT_MAX_ENDPOINT_COUNT 4u

// Default total wire-frame limit. This is an admission bound and does not
// contribute to resident connection storage.
#define IREE_NET_TCP_DEFAULT_MAX_FRAME_SIZE UINT32_MAX

// Default number of frames retained for one endpoint before local activation.
#define IREE_NET_TCP_DEFAULT_MAX_PENDING_FRAMES_PER_ENDPOINT 8u

// Options controlling TCP connection framing and bounded resources.
typedef struct iree_net_tcp_connection_options_t {
  // Number of ordinal message endpoints preallocated by the connection.
  uint32_t max_endpoint_count;

  // Maximum total wire-frame extent including the transport header.
  uint32_t max_frame_size;

  // Maximum frames retained per endpoint before local activation.
  uint32_t max_pending_frames_per_endpoint;

  // Admission limits for the shared raw TCP carrier.
  iree_net_tcp_carrier_options_t carrier_options;
} iree_net_tcp_connection_options_t;

// Returns default TCP connection options.
static inline iree_net_tcp_connection_options_t
iree_net_tcp_connection_options_default(void) {
  iree_net_tcp_connection_options_t options;
  options.max_endpoint_count = IREE_NET_TCP_DEFAULT_MAX_ENDPOINT_COUNT;
  options.max_frame_size = IREE_NET_TCP_DEFAULT_MAX_FRAME_SIZE;
  options.max_pending_frames_per_endpoint =
      IREE_NET_TCP_DEFAULT_MAX_PENDING_FRAMES_PER_ENDPOINT;
  options.carrier_options = iree_net_tcp_carrier_options_default();
  return options;
}

// Creates a published connection over a connected TCP socket.
//
// |proactor| must own |socket| and the registered region backing
// |receive_pool|. The pool must be dedicated to this connection while active.
// The connection retains all three resources through its shared carrier
// stack without consuming the caller's references.
//
// The shared receive path is activated before this function returns. Peer
// frames arriving before their ordinal endpoint is activated are retained up
// to |options.max_pending_frames_per_endpoint| and delivered in wire order
// after activation. Retention moves the framing adapter's existing lease
// without copying payload data; a direct receive lease therefore reduces the
// dedicated pool's available capacity until the endpoint activates or drains.
//
// The returned connection must be deactivated before its final release.
IREE_API_EXPORT iree_status_t iree_net_tcp_connection_create(
    iree_async_proactor_t* proactor, iree_async_socket_t* socket,
    iree_async_buffer_pool_t* receive_pool,
    const iree_net_tcp_connection_options_t* options,
    iree_allocator_t host_allocator, iree_net_connection_t** out_connection);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_NET_CARRIER_TCP_CONNECTION_H_
