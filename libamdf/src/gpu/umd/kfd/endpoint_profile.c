// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#define _GNU_SOURCE
#include "libamdf/src/gpu/umd/endpoint_profile.h"

#include <unistd.h>

#include "libamdf/src/gpu/umd/kfd/file.h"
#include "libamdf/src/gpu/umd/kfd/target/user_queue.h"
#include "libamdf/src/gpu/umd/kfd/topology.h"
#include "libamdf/src/platform/linux/host_cache.h"

amdf_status_t amdf_gpu_umd_query_endpoint_profile(
    amdf_platform_endpoint_t* endpoint, amdf_allocator_t host_allocator,
    amdf_gpu_endpoint_profile_t* out_profile, bool* out_available) {
  (void)host_allocator;
  amdf_gpu_kfd_topology_t topology = {0};
  amdf_status_t status = amdf_gpu_kfd_topology_query(endpoint, &topology);
  if (status == amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED)) {
    *out_available = false;
    return AMDF_STATUS_OK;
  }
  int descriptor = -1;
  struct kfd_ioctl_get_version_args version = {0};
  if (amdf_status_is_ok(status)) {
    status = amdf_gpu_kfd_file_open(&descriptor, &version);
  }
  const amdf_status_t close_status = amdf_linux_file_close(&descriptor);
  if (!amdf_status_is_ok(close_status)) status = close_status;
  if (!amdf_status_is_ok(status)) return status;
  // KFD 1.18 is the minimum supported native interface. CREATE_PROCESS arrived
  // in 1.19; its absence never changes an INSTANCE request into PROCESS use.
  if (version.major_version != 1 || version.minor_version < 18) {
    *out_available = false;
    return AMDF_STATUS_OK;
  }
  topology.properties.native_lifetimes[AMDF_NATIVE_LIFETIME_PROCESS] =
      (amdf_gpu_lifetime_properties_t){
          .supported = true,
          .features = topology.memory_features |
                      AMDF_GPU_DEVICE_FEATURE_HOST_REGISTRATION |
                      AMDF_GPU_DEVICE_FEATURE_DEVICE_RECREATION,
      };
  topology.properties.native_lifetimes[AMDF_NATIVE_LIFETIME_INSTANCE] =
      (amdf_gpu_lifetime_properties_t){
          .supported = version.minor_version >= 19,
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
  *out_profile = profile;
  *out_available = true;
  return AMDF_STATUS_OK;
}
