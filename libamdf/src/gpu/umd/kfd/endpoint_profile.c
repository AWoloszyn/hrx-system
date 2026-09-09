// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#define _GNU_SOURCE
#include "libamdf/src/gpu/umd/endpoint_profile.h"

#include "libamdf/src/gpu/umd/kfd/file.h"
#include "libamdf/src/gpu/umd/kfd/topology.h"

amdf_status_t amdf_gpu_umd_query_endpoint_profile(
    amdf_platform_endpoint_t* endpoint,
    amdf_gpu_endpoint_profile_t* out_profile, bool* out_available) {
  *out_available = false;
  amdf_gpu_kfd_topology_t topology = {0};
  amdf_status_t status = amdf_gpu_kfd_topology_query(endpoint, &topology);
  if (status == amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED)) {
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
  // in 1.19; its absence never changes an independent request into primary use.
  if (version.major_version != 1 || version.minor_version < 18) {
    return AMDF_STATUS_OK;
  }
  topology.properties.device_modes[AMDF_GPU_DEVICE_MODE_PROCESS] =
      (amdf_gpu_device_mode_properties_t){
          .supported = true,
          .features = topology.memory_features |
                      AMDF_GPU_DEVICE_FEATURE_HOST_REGISTRATION,
      };
  topology.properties.device_modes[AMDF_GPU_DEVICE_MODE_INDEPENDENT] =
      (amdf_gpu_device_mode_properties_t){
          .supported = version.minor_version >= 19,
          .features = topology.memory_features |
                      AMDF_GPU_DEVICE_FEATURE_DEVICE_RECREATION,
      };
  if (!amdf_gpu_endpoint_profile_initialize(&topology.properties,
                                            out_profile)) {
    return amdf_linux_error(EPROTO);
  }
  *out_available = true;
  return AMDF_STATUS_OK;
}
