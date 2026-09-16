// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Paired loopback connections with eager ordinal endpoint stacks.

#ifndef IREE_NET_CARRIER_LOOPBACK_CONNECTION_H_
#define IREE_NET_CARRIER_LOOPBACK_CONNECTION_H_

#include "iree/async/proactor.h"
#include "iree/base/api.h"
#include "iree/net/carrier/loopback/carrier.h"
#include "iree/net/connection.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Creates a connected pair with |max_endpoint_count| eagerly initialized
// message endpoints.
//
// The returned connections each have one reference and are unpublished:
// callers owning connection establishment must call
// iree_net_loopback_connection_publish() immediately before transferring each
// connection through its callback. An unpublished connection may be released
// synchronously on establishment failure. A published connection follows the
// public deactivate-before-release contract.
IREE_API_EXPORT iree_status_t iree_net_loopback_connection_create_pair(
    iree_async_proactor_t* client_proactor,
    iree_async_proactor_t* server_proactor, uint32_t max_endpoint_count,
    const iree_net_loopback_carrier_options_t* carrier_options,
    iree_allocator_t host_allocator, iree_net_connection_t** out_client,
    iree_net_connection_t** out_server);

// Marks an unpublished connection as externally owned.
//
// Call immediately before transferring the connection through connect or
// accept. Once published, callers must deactivate the connection before its
// final release.
IREE_API_EXPORT void iree_net_loopback_connection_publish(
    iree_net_connection_t* connection);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_NET_CARRIER_LOOPBACK_CONNECTION_H_
