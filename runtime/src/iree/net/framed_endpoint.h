// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Length-prefixed payload-message endpoint over one ordered byte-stream
// carrier.
//
// Each nonempty message has an eight-byte wire header: the four bytes "IRN1"
// followed by a little-endian uint32_t total frame length including the header.
// This is a non-multiplexed stream with one endpoint per carrier; transports
// sharing a stream among endpoint ordinals supply their own framing instead.
//
// Complete frames can transfer the carrier's receive lease to the consumer.
// Borrowed chunks and fragmented frames use the framing adapter's independently
// owned message storage. Carrier chunk size is not a message size limit.

#ifndef IREE_NET_FRAMED_ENDPOINT_H_
#define IREE_NET_FRAMED_ENDPOINT_H_

#include "iree/base/api.h"
#include "iree/net/carrier.h"
#include "iree/net/endpoint_lifecycle.h"
#include "iree/net/message_endpoint.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

typedef struct iree_net_framed_endpoint_t iree_net_framed_endpoint_t;

// Allocates a payload-message endpoint over |carrier|.
//
// The endpoint adds and removes the wire header while preserving
// message boundaries across arbitrary carrier receive chunks. It takes
// ownership of |carrier| on success and leaves ownership with the caller on
// failure. The carrier must be in CREATED state and provide reliable, ordered
// delivery. Payload length is limited to UINT32_MAX minus the eight-byte
// header; this is a logical admission bound, not an allocation of resident
// storage.
//
// |proactor| must be the carrier's owning proactor. The endpoint retains it so
// locally terminated accepted sends preserve callback affinity.
// |max_send_operations| must match the carrier's configured operation limit.
// The endpoint preallocates the same number of framing records so adding a
// stable wire header introduces no additional send-admission limit.
// |connection_barrier| optionally binds this endpoint to its owning
// connection's drain. When provided it must be initialized and outlive the
// endpoint. Standalone endpoints pass NULL.
IREE_API_EXPORT iree_status_t iree_net_framed_endpoint_allocate(
    iree_net_carrier_t* carrier, iree_async_proactor_t* proactor,
    uint32_t max_send_operations,
    iree_net_endpoint_deactivation_barrier_t* connection_barrier,
    iree_allocator_t host_allocator, iree_net_framed_endpoint_t** out_endpoint);

// Frees a created or fully deactivated endpoint and its owned carrier stack.
IREE_API_EXPORT void iree_net_framed_endpoint_free(
    iree_net_framed_endpoint_t* endpoint);

// Joins connection deactivation to this endpoint's carrier drain.
//
// Generated prefixes are written synchronously inside send calls. Connection
// drain waits for each accepted send to reach its terminal completion.
IREE_API_EXPORT void iree_net_framed_endpoint_join_deactivation(
    iree_net_framed_endpoint_t* endpoint);

// Returns a borrowed payload-message endpoint view.
IREE_API_EXPORT iree_net_message_endpoint_t
iree_net_framed_endpoint_as_message_endpoint(
    iree_net_framed_endpoint_t* endpoint);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_NET_FRAMED_ENDPOINT_H_
