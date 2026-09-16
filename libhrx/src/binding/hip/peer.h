// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LIBHRX_SRC_BINDING_HIP_PEER_H_
#define LIBHRX_SRC_BINDING_HIP_PEER_H_

#include <stdbool.h>
#include <stdint.h>

#include "iree/hal/topology.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Converts a HAL link type to the signed native HIP performance rank.
// UNKNOWN is the successful no-link sentinel -1. Returns false only for a
// topology link value outside the HAL contract.
bool iree_hip_peer_performance_rank_from_topology(
    iree_hal_topology_link_type_t topology_link_type, int* out_rank);

// Converts HAL link properties to the native HIP extension representation.
// UNKNOWN is the successful no-link sentinel UINT32_MAX and preserves the
// topology hop count (normally zero).
bool iree_hip_peer_link_info_from_topology(
    iree_hal_topology_link_type_t topology_link_type,
    uint32_t topology_hop_count, uint32_t* out_link_type,
    uint32_t* out_hop_count);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // LIBHRX_SRC_BINDING_HIP_PEER_H_
