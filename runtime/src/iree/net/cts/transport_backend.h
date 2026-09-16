// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Link-time transport backend contract for the network CTS.

#ifndef IREE_NET_CTS_TRANSPORT_BACKEND_H_
#define IREE_NET_CTS_TRANSPORT_BACKEND_H_

#include <string>

#include "iree/base/api.h"
#include "iree/net/transport_factory.h"

namespace iree::net::cts {

// Creates the transport factory exercised by the CTS.
using CreateFactoryFn =
    iree_status_t (*)(iree_allocator_t host_allocator,
                      iree_net_transport_factory_t** out_factory);

// Produces one transport-specific address for a CTS operation.
using MakeAddressFn = iree_status_t (*)(std::string* out_address);

// Immutable backend configuration linked into one CTS executable.
struct TransportBackend {
  // Human-readable transport name used in diagnostics.
  const char* name;

  // Capabilities every factory instance must report.
  iree_net_transport_capabilities_t required_capabilities;

  // Factory constructor under test.
  CreateFactoryFn create_factory;

  // Produces an address suitable for listener creation.
  MakeAddressFn make_bind_address;

  // Produces an address with no listening peer.
  MakeAddressFn make_unreachable_address;
};

// Returns the backend descriptor supplied by the linked transport package.
const TransportBackend& GetTransportBackend();

}  // namespace iree::net::cts

#endif  // IREE_NET_CTS_TRANSPORT_BACKEND_H_
