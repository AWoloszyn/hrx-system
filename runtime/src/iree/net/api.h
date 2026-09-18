// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Umbrella header for the iree/net/ networking API.
//
// This provides transport-independent networking primitives. The stack is
// layered:
//
//   Layer 4: Sessions - peer bootstrap and connection lifecycle
//   Layer 3: Connections and message endpoints - framed message multiplexing
//   Layer 2: Carriers - asynchronous byte and message transport
//   Layer 1: Proactor (iree/async/) - completion-based I/O
//
// The carrier abstraction allows transports to change without affecting
// message protocols. Scatter/gather spans preserve zero-copy paths through
// each layer.

#ifndef IREE_NET_API_H_
#define IREE_NET_API_H_

#include "iree/net/carrier.h"
#include "iree/net/connection.h"
#include "iree/net/message_endpoint.h"
#include "iree/net/session.h"
#include "iree/net/transport_factory.h"
#include "iree/net/transport_registry.h"

#endif  // IREE_NET_API_H_
