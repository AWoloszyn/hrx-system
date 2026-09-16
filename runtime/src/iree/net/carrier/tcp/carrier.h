// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// TCP carrier implementing reliable ordered byte-stream transport.
//
// One logical send is submitted to the socket at a time. This preserves
// stream ordering across partial platform sends while additional accepted
// sends wait in a bounded carrier-owned FIFO. Registered regions are retained
// from admission through logical completion; payload bytes remain caller-owned
// under the generic asynchronous carrier contract.
//
// Receive progress uses one pool-backed operation. A consumer may move a
// receive lease out of its callback, and returning that lease wakes receive
// progress if pool exhaustion paused it.

#ifndef IREE_NET_CARRIER_TCP_CARRIER_H_
#define IREE_NET_CARRIER_TCP_CARRIER_H_

#include "iree/async/buffer_pool.h"
#include "iree/async/proactor.h"
#include "iree/async/socket.h"
#include "iree/base/api.h"
#include "iree/net/carrier.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Default number of accepted sends and direct reservations per carrier.
#define IREE_NET_TCP_DEFAULT_MAX_SEND_OPERATIONS 64u

// Options controlling bounded TCP carrier resources.
typedef struct iree_net_tcp_carrier_options_t {
  // Maximum accepted ordinary sends and direct reservations.
  uint32_t max_send_operations;
} iree_net_tcp_carrier_options_t;

// Returns default TCP carrier options.
static inline iree_net_tcp_carrier_options_t
iree_net_tcp_carrier_options_default(void) {
  iree_net_tcp_carrier_options_t options;
  options.max_send_operations = IREE_NET_TCP_DEFAULT_MAX_SEND_OPERATIONS;
  return options;
}

// Creates a carrier over a connected TCP socket.
//
// |proactor| must own |socket| and the registered region backing
// |receive_pool|. The receive region must permit writes and contain at least
// one buffer. The pool is dedicated to this carrier while it is active; sharing
// its buffers with another I/O producer would violate lease exclusivity. The
// carrier retains all three resources without consuming the caller's
// references.
//
// The returned carrier begins in CREATED state. Install handlers and activate
// it before sending. The carrier accepts at most
// |options.max_send_operations| ordinary sends and direct reservations and at
// most IREE_ASYNC_SOCKET_SEND_MAX_BUFFERS spans per ordinary send.
IREE_API_EXPORT iree_status_t iree_net_tcp_carrier_create(
    iree_async_proactor_t* proactor, iree_async_socket_t* socket,
    iree_async_buffer_pool_t* receive_pool,
    const iree_net_tcp_carrier_options_t* options,
    iree_allocator_t host_allocator, iree_net_carrier_t** out_carrier);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_NET_CARRIER_TCP_CARRIER_H_
