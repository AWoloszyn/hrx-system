// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#define _GNU_SOURCE
#include "libamdf/src/gpu/umd/endpoint_profile.h"

#include <unistd.h>

#include "libamdf/src/gpu/umd/kfd/memory_profile.h"
#include "libamdf/src/gpu/umd/kfd/target/user_queue.h"
#include "libamdf/src/gpu/umd/kfd/topology.h"
#include "libamdf/src/platform/linux/file.h"
#include "libamdf/src/platform/linux/host_cache.h"

amdf_status_t amdf_gpu_umd_query_endpoint_profile(
    amdf_platform_endpoint_t* endpoint, amdf_native_lifetime_t native_lifetime,
    amdf_allocator_t host_allocator, amdf_gpu_endpoint_profile_t* out_profile,
    bool* out_available) {
  (void)host_allocator;
  amdf_gpu_kfd_topology_t topology = {0};
  amdf_status_t status = amdf_gpu_kfd_topology_query(endpoint, &topology);
  if (status == amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED)) {
    *out_available = false;
    return AMDF_STATUS_OK;
  }
  if (!amdf_status_is_ok(status)) return status;
  // These are expected implemented policies, not installed-ABI qualification.
  // Opening /dev/kfd creates process state. Explicit device creation qualifies
  // the actual connection before selecting its requested lifetime.
  topology.properties.native_lifetimes[AMDF_NATIVE_LIFETIME_PROCESS] =
      (amdf_gpu_lifetime_properties_t){
          .supported = true,
          .features = topology.memory_features |
                      AMDF_GPU_DEVICE_FEATURE_HOST_REGISTRATION |
                      AMDF_GPU_DEVICE_FEATURE_DEVICE_RECREATION,
      };
  topology.properties.native_lifetimes[AMDF_NATIVE_LIFETIME_INSTANCE] =
      (amdf_gpu_lifetime_properties_t){
          .supported = true,
          .features = topology.memory_features |
                      AMDF_GPU_DEVICE_FEATURE_DEVICE_RECREATION,
      };
  const long page_size = sysconf(_SC_PAGESIZE);
  uint32_t cache_line_size = 0;
  if (page_size <= 0 || (page_size & (page_size - 1)) != 0) {
    return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
  }
  status = amdf_linux_host_cache_query_line_size(&cache_line_size);
  if (!amdf_status_is_ok(status)) return status;
  amdf_gpu_kfd_user_queue_plans_t queue_plans;
  amdf_gpu_kfd_target_user_queue_plans_initialize(
      &topology, (size_t)page_size, cache_line_size, &queue_plans);
  for (uint32_t i = 0; i < queue_plans.count; ++i) {
    topology.properties
        .queue_families[topology.properties.queue_family_count++] =
        queue_plans.values[i].family;
  }
  amdf_gpu_endpoint_profile_t profile;
  if (!amdf_gpu_endpoint_profile_initialize(&topology.properties, &profile)) {
    return amdf_linux_error(EPROTO);
  }
  profile.memory.count =
      1 +
      ((topology.memory_features & AMDF_GPU_DEVICE_FEATURE_LOCAL_MEMORY) != 0) +
      (native_lifetime == AMDF_NATIVE_LIFETIME_PROCESS);
  for (uint32_t i = 0; i < profile.memory.count; ++i) {
    const amdf_status_t memory_status = amdf_gpu_kfd_query_memory_profile(
        &topology, (size_t)page_size, native_lifetime, i,
        &profile.memory.values[i]);
    amdf_assert(amdf_status_is_ok(memory_status));
    (void)memory_status;
  }
  *out_profile = profile;
  *out_available = true;
  return AMDF_STATUS_OK;
}
