// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_SRC_GPU_UMD_KFD_TOPOLOGY_H_
#define AMDF_SRC_GPU_UMD_KFD_TOPOLOGY_H_

#include "libamdf/src/gpu/endpoint_profile.h"
#include "libamdf/src/platform/endpoint.h"

// Native facts belonging to one identity-checked DRM endpoint.
typedef struct amdf_gpu_kfd_topology_t {
  // GPU target and compute geometry reported by KFD topology.
  amdf_gpu_endpoint_properties_t properties;
  // KFD node identity used by memory and queue ioctls.
  uint32_t gpu_id;
  // Physical placement features supported by the native device.
  amdf_gpu_device_features_t memory_features;
  // Ordinary GPU virtual-address interval reported by DRM.
  struct {
    // First usable GPU virtual byte address.
    uint64_t begin;
    // Exclusive end of the usable GPU virtual interval.
    uint64_t end;
    // Required GPU mapping alignment in bytes.
    uint32_t alignment;
  } virtual_address;
} amdf_gpu_kfd_topology_t;

#ifdef __cplusplus
extern "C" {
#endif

// Reads a coherent topology snapshot and native DRM memory/address facts.
// A missing KFD node returns UNSUPPORTED; malformed or changing state is an
// error.
amdf_status_t amdf_gpu_kfd_topology_query(
    const amdf_platform_endpoint_t* endpoint,
    amdf_gpu_kfd_topology_t* out_topology);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // AMDF_SRC_GPU_UMD_KFD_TOPOLOGY_H_
