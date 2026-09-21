// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_NET_CARRIER_SHM_CONNECT_H_
#define IREE_NET_CARRIER_SHM_CONNECT_H_

#include "iree/net/carrier/shm/carrier.h"
#include "iree/net/carrier/shm/region.h"
#include "iree/net/transport_factory.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Admits native connection setup with the public operation mutex held. Copies
// validated options and the address before returning; retains the proactor.
// Cancellation and publication join the native connect, import handshake and
// any private connection drain before detaching the public binding.
iree_status_t iree_net_shm_connect(
    iree_string_view_t address, iree_async_proactor_t* proactor,
    const iree_net_shm_region_layout_t* limits,
    const iree_net_shm_carrier_options_t* carrier_options,
    iree_net_transport_connect_callback_t callback,
    iree_net_transport_connect_operation_t* operation,
    iree_allocator_t host_allocator);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_NET_CARRIER_SHM_CONNECT_H_
