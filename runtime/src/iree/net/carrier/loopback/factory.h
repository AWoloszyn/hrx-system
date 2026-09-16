// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Named in-process transport factory backed by loopback carrier pairs.

#ifndef IREE_NET_CARRIER_LOOPBACK_FACTORY_H_
#define IREE_NET_CARRIER_LOOPBACK_FACTORY_H_

#include "iree/base/api.h"
#include "iree/net/carrier/loopback/carrier.h"
#include "iree/net/transport_factory.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Default number of ordinal message endpoints in each connection.
#define IREE_NET_LOOPBACK_DEFAULT_MAX_ENDPOINT_COUNT 4u

// Options controlling loopback connection and carrier admission limits.
typedef struct iree_net_loopback_factory_options_t {
  // Number of message endpoints eagerly allocated in each connection.
  uint32_t max_endpoint_count;

  // Admission limits applied independently to every endpoint carrier.
  iree_net_loopback_carrier_options_t carrier_options;
} iree_net_loopback_factory_options_t;

// Returns default loopback transport factory options.
static inline iree_net_loopback_factory_options_t
iree_net_loopback_factory_options_default(void) {
  iree_net_loopback_factory_options_t options;
  options.max_endpoint_count = IREE_NET_LOOPBACK_DEFAULT_MAX_ENDPOINT_COUNT;
  options.carrier_options = iree_net_loopback_carrier_options_default();
  return options;
}

// Creates a loopback factory routing connections to named in-process
// listeners.
//
// Each successful connection eagerly creates |max_endpoint_count| paired
// endpoint stacks. Client callbacks are dispatched by the proactor supplied to
// connect and accept callbacks are dispatched by the listener proactor.
//
// Capabilities: RELIABLE | ORDERED.
IREE_API_EXPORT iree_status_t iree_net_loopback_factory_create(
    const iree_net_loopback_factory_options_t* options,
    iree_allocator_t host_allocator,
    iree_net_transport_factory_t** out_factory);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_NET_CARRIER_LOOPBACK_FACTORY_H_
