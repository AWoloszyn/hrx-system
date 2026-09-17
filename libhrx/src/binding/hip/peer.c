// Copyright 2026 The HRX Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "binding/hip/peer.h"

#include "iree/base/api.h"

typedef enum iree_hip_peer_link_type_e {
  IREE_HIP_PEER_LINK_TYPE_UNKNOWN = -1,
  IREE_HIP_PEER_LINK_TYPE_HYPERTRANSPORT = 0,
  IREE_HIP_PEER_LINK_TYPE_QPI = 1,
  IREE_HIP_PEER_LINK_TYPE_PCIE = 2,
  IREE_HIP_PEER_LINK_TYPE_INFINIBAND = 3,
  IREE_HIP_PEER_LINK_TYPE_XGMI = 4,
} iree_hip_peer_link_type_t;

// HIP exposes native link ABI values, which intentionally differ from the
// generic HAL enum because UNKNOWN occupies zero in the HAL representation.
static bool iree_hip_peer_link_type_from_topology(
    iree_hal_topology_link_type_t topology_link_type,
    iree_hip_peer_link_type_t* out_link_type) {
  IREE_ASSERT_ARGUMENT(out_link_type);
  switch (topology_link_type) {
    case IREE_HAL_TOPOLOGY_LINK_TYPE_UNKNOWN:
      *out_link_type = IREE_HIP_PEER_LINK_TYPE_UNKNOWN;
      return true;
    case IREE_HAL_TOPOLOGY_LINK_TYPE_HYPERTRANSPORT:
      *out_link_type = IREE_HIP_PEER_LINK_TYPE_HYPERTRANSPORT;
      return true;
    case IREE_HAL_TOPOLOGY_LINK_TYPE_QPI:
      *out_link_type = IREE_HIP_PEER_LINK_TYPE_QPI;
      return true;
    case IREE_HAL_TOPOLOGY_LINK_TYPE_PCIE:
      *out_link_type = IREE_HIP_PEER_LINK_TYPE_PCIE;
      return true;
    case IREE_HAL_TOPOLOGY_LINK_TYPE_INFINIBAND:
      *out_link_type = IREE_HIP_PEER_LINK_TYPE_INFINIBAND;
      return true;
    case IREE_HAL_TOPOLOGY_LINK_TYPE_XGMI:
      *out_link_type = IREE_HIP_PEER_LINK_TYPE_XGMI;
      return true;
    default:
      return false;
  }
}

bool iree_hip_peer_performance_rank_from_topology(
    iree_hal_topology_link_type_t topology_link_type, int* out_rank) {
  IREE_ASSERT_ARGUMENT(out_rank);
  iree_hip_peer_link_type_t link_type;
  if (!iree_hip_peer_link_type_from_topology(topology_link_type, &link_type)) {
    return false;
  }
  *out_rank = (int)link_type;
  return true;
}

bool iree_hip_peer_link_info_from_topology(
    iree_hal_topology_link_type_t topology_link_type,
    uint32_t topology_hop_count, uint32_t* out_link_type,
    uint32_t* out_hop_count) {
  IREE_ASSERT_ARGUMENT(out_link_type);
  IREE_ASSERT_ARGUMENT(out_hop_count);
  iree_hip_peer_link_type_t link_type;
  if (!iree_hip_peer_link_type_from_topology(topology_link_type, &link_type)) {
    return false;
  }
  *out_link_type = (uint32_t)link_type;
  *out_hop_count = topology_hop_count;
  return true;
}
