// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// In-process carrier used as the reference implementation and test transport.
//
// A connected pair may use different proactors. Each carrier dispatches all
// receive, send-completion, terminal-error, and deactivation callbacks from its
// own proactor. Cross-proactor notification operations wake bounded pair-owned
// queues without a private worker thread.
//
// Ordinary sends retain caller span descriptors and registered regions without
// copying payload bytes. The caller's byte storage must remain valid until the
// send completion callback fires, as required by the generic carrier contract.

#ifndef IREE_NET_CARRIER_LOOPBACK_CARRIER_H_
#define IREE_NET_CARRIER_LOOPBACK_CARRIER_H_

#include "iree/async/proactor.h"
#include "iree/base/api.h"
#include "iree/net/carrier.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Default maximum number of accepted send operations per carrier.
#define IREE_NET_LOOPBACK_DEFAULT_MAX_SEND_OPERATIONS 64u

// Default maximum number of scatter/gather spans in one send operation.
#define IREE_NET_LOOPBACK_DEFAULT_MAX_SEND_SPANS 64u

// Options controlling loopback carrier admission limits.
typedef struct iree_net_loopback_carrier_options_t {
  // Maximum accepted ordinary sends and direct-write reservations per carrier.
  uint32_t max_send_operations;

  // Maximum scatter/gather spans accepted by one ordinary send.
  iree_host_size_t max_send_spans;
} iree_net_loopback_carrier_options_t;

// Returns default loopback carrier options.
static inline iree_net_loopback_carrier_options_t
iree_net_loopback_carrier_options_default(void) {
  iree_net_loopback_carrier_options_t options;
  options.max_send_operations = IREE_NET_LOOPBACK_DEFAULT_MAX_SEND_OPERATIONS;
  options.max_send_spans = IREE_NET_LOOPBACK_DEFAULT_MAX_SEND_SPANS;
  return options;
}

// Creates a connected pair of loopback carriers.
//
// Each carrier retains its corresponding proactor and dispatches callbacks
// only while that proactor is polled. The proactors may be the same object.
//
// The returned carriers begin in CREATED state. Install handlers and activate
// each independently. An active side may send before its peer activates; those
// sends remain queued until peer activation or complete with UNAVAILABLE if the
// peer is destroyed first.
IREE_API_EXPORT iree_status_t iree_net_loopback_carrier_create_pair(
    iree_async_proactor_t* client_proactor,
    iree_async_proactor_t* server_proactor,
    const iree_net_loopback_carrier_options_t* options,
    iree_allocator_t host_allocator, iree_net_carrier_t** out_client,
    iree_net_carrier_t** out_server);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_NET_CARRIER_LOOPBACK_CARRIER_H_
