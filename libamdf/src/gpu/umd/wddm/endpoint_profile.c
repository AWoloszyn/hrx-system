// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/gpu/umd/endpoint_profile.h"

#include <stddef.h>

#include "libamdf/src/allocator.h"
#include "libamdf/src/gpu/umd/wddm/wkmi/adapter.h"
#include "libamdf/src/gpu/umd/wddm/wkmi/endpoint_properties.h"
#include "libamdf/src/platform/windows/endpoint.h"

amdf_status_t amdf_gpu_umd_create_endpoint_profile(
    amdf_platform_endpoint_t* platform_endpoint,
    amdf_allocator_t host_allocator,
    amdf_gpu_endpoint_profile_t** out_profile) {
  amdf_gpu_wddm_wkmi_loader_t loader = {0};
  amdf_gpu_wddm_wkmi_adapter_t adapter = {0};
  bool profile_available = false;
  amdf_gpu_endpoint_profile_t profile = {0};
  amdf_wkmi_bridge_gpu_properties_t provider_properties = {0};
  amdf_status_t status =
      amdf_gpu_wddm_wkmi_loader_initialize(host_allocator, &loader);
  if (amdf_status_is_ok(status)) {
    status = amdf_gpu_wddm_wkmi_adapter_initialize(
        &loader, platform_endpoint->adapter,
        platform_endpoint->physical_adapter_index, host_allocator, &adapter,
        &provider_properties);
    if (status == amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED)) {
      status = AMDF_STATUS_OK;
    }
  }

  if (amdf_status_is_ok(status) && adapter.native != NULL) {
    amdf_gpu_endpoint_properties_t properties = {0};
    if (amdf_gpu_wddm_wkmi_endpoint_properties_translate(&provider_properties,
                                                         &properties)) {
      if (amdf_gpu_endpoint_profile_initialize(&properties, &profile)) {
        profile_available = true;
      } else {
        status = amdf_make_api_status(AMDF_STATUS_CODE_INTERNAL);
      }
    } else {
      status = amdf_make_api_status(AMDF_STATUS_CODE_INTERNAL);
    }
  }

  amdf_status_t release_status = AMDF_STATUS_OK;
  if (adapter.native != NULL) {
    release_status = amdf_gpu_wddm_wkmi_adapter_deinitialize(&adapter);
  }
  if (amdf_status_is_ok(release_status) && loader.module != NULL) {
    release_status = amdf_gpu_wddm_wkmi_loader_deinitialize(&loader);
  }
  if (!amdf_status_is_ok(release_status)) {
    status = release_status;
  }

  if (amdf_status_is_ok(status) && !profile_available) {
    status = amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
  }
  amdf_gpu_endpoint_profile_t* owned_profile = NULL;
  if (amdf_status_is_ok(status)) {
    status = amdf_malloc(host_allocator, sizeof(profile),
                         amdf_alignof(amdf_gpu_endpoint_profile_t),
                         (void**)&owned_profile);
  }
  if (amdf_status_is_ok(status)) {
    *owned_profile = profile;
    *out_profile = owned_profile;
  }
  return status;
}
