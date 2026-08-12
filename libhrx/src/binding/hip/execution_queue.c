// Copyright 2026 The HRX Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "binding/hip/execution_queue.h"

#include <stdbool.h>
#include <string.h>

#include "iree/hal/drivers/amdgpu/api.h"
#include "libhrx/src/binding/hip/api.h"

typedef struct hrx_hip_execution_queue_config_t {
  // Number of execution units represented by the HIP-visible mask.
  uint32_t hip_execution_unit_count;
  // Whether HIP reports paired native compute units as one execution unit.
  bool may_use_paired_native_units;
  // Allocator used for temporary native mask storage.
  iree_allocator_t host_allocator;
} hrx_hip_execution_queue_config_t;

static uint32_t hrx_hip_execution_queue_device_unit_count(
    iree_hal_device_t* device) {
  const iree_hal_device_dispatch_spec_t* dispatch =
      iree_hal_device_spec_dispatch(iree_hal_device_spec(device));
  const iree_hal_device_execution_spec_t* execution =
      dispatch ? &dispatch->execution : NULL;
  return execution ? execution->unit_count : 0;
}

static bool hrx_hip_execution_queue_mask_bit(iree_host_size_t mask_bit_count,
                                             const uint32_t* mask,
                                             uint32_t bit_ordinal) {
  return bit_ordinal < mask_bit_count &&
         iree_any_bit_set(mask[bit_ordinal / 32], UINT32_C(1)
                                                      << (bit_ordinal % 32));
}

static void hrx_hip_execution_queue_set_mask_bit(uint32_t bit_ordinal,
                                                 uint32_t* mask) {
  mask[bit_ordinal / 32] |= UINT32_C(1) << (bit_ordinal % 32);
}

static iree_status_t hrx_hip_execution_queue_acquire(
    void* user_data, iree_hal_device_t* device,
    iree_hal_queue_affinity_t device_affinity,
    iree_host_size_t execution_unit_mask_bit_count,
    const uint32_t* execution_unit_mask, void** out_backend_queue,
    iree_hal_queue_affinity_t* out_queue_affinity) {
  *out_backend_queue = NULL;
  *out_queue_affinity = IREE_HAL_QUEUE_AFFINITY_ANY;
  const hrx_hip_execution_queue_config_t* config =
      (const hrx_hip_execution_queue_config_t*)user_data;
  const uint32_t native_execution_unit_count =
      hrx_hip_execution_queue_device_unit_count(device);
  if (IREE_UNLIKELY(native_execution_unit_count == 0)) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "AMDGPU device reports no execution units");
  }

  bool expand_execution_units = false;
  if (config->may_use_paired_native_units &&
      config->hip_execution_unit_count <= UINT32_MAX / 2 &&
      native_execution_unit_count == config->hip_execution_unit_count * 2) {
    expand_execution_units = true;
  } else if (IREE_UNLIKELY(native_execution_unit_count !=
                           config->hip_execution_unit_count)) {
    return iree_make_status(
        IREE_STATUS_FAILED_PRECONDITION,
        "HIP exposes %u execution units but AMDGPU reports %u native units",
        config->hip_execution_unit_count, native_execution_unit_count);
  }

  const iree_host_size_t native_mask_bit_count =
      iree_host_align((iree_host_size_t)native_execution_unit_count, 32);
  const iree_host_size_t native_mask_size = native_mask_bit_count / 8;
  uint32_t* native_mask = NULL;
  IREE_RETURN_IF_ERROR(iree_allocator_malloc(
      config->host_allocator, native_mask_size, (void**)&native_mask));
  memset(native_mask, 0, native_mask_size);

  bool has_enabled_unit = false;
  for (uint32_t i = 0; i < config->hip_execution_unit_count; ++i) {
    if (!hrx_hip_execution_queue_mask_bit(execution_unit_mask_bit_count,
                                          execution_unit_mask, i)) {
      continue;
    }
    has_enabled_unit = true;
    const uint32_t native_ordinal = expand_execution_units ? i * 2 : i;
    hrx_hip_execution_queue_set_mask_bit(native_ordinal, native_mask);
    if (expand_execution_units) {
      hrx_hip_execution_queue_set_mask_bit(native_ordinal + 1, native_mask);
    }
  }

  // An empty HIP mask selects the device default instead of disabling every
  // execution unit. Materialize the default before acquiring the immutable
  // backend queue so equivalent empty and full masks share one lease.
  if (!has_enabled_unit) {
    for (uint32_t i = 0; i < native_execution_unit_count; ++i) {
      hrx_hip_execution_queue_set_mask_bit(i, native_mask);
    }
  }

  iree_hal_amdgpu_execution_queue_t* execution_queue = NULL;
  iree_status_t status = iree_hal_amdgpu_execution_queue_acquire(
      device, device_affinity, native_mask_bit_count, native_mask,
      &execution_queue);
  iree_allocator_free(config->host_allocator, native_mask);
  if (iree_status_is_ok(status)) {
    *out_backend_queue = execution_queue;
    *out_queue_affinity =
        iree_hal_amdgpu_execution_queue_affinity(execution_queue);
  }
  return status;
}

static void hrx_hip_execution_queue_release(void* backend_queue) {
  iree_hal_amdgpu_execution_queue_release(
      (iree_hal_amdgpu_execution_queue_t*)backend_queue);
}

iree_status_t hrx_hip_execution_queue_scope_create(
    iree_host_size_t device_ordinal,
    iree_host_size_t execution_unit_mask_bit_count,
    const uint32_t* execution_unit_mask, iree_allocator_t host_allocator,
    iree_hal_streaming_queue_scope_t** out_scope) {
  hipDeviceProp_t properties;
  hipError_t result = hipGetDeviceProperties(&properties, (int)device_ordinal);
  if (result != hipSuccess) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "cannot query HIP device properties");
  }
  if (IREE_UNLIKELY(properties.multiProcessorCount <= 0)) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "HIP device reports no execution units");
  }
  const hrx_hip_execution_queue_config_t config = {
      .hip_execution_unit_count = (uint32_t)properties.multiProcessorCount,
      .may_use_paired_native_units =
          properties.major >= 10 && properties.warpSize == 32,
      .host_allocator = host_allocator,
  };
  return iree_hal_streaming_queue_scope_create(
      device_ordinal, execution_unit_mask_bit_count, execution_unit_mask,
      hrx_hip_execution_queue_acquire, (void*)&config,
      hrx_hip_execution_queue_release, host_allocator, out_scope);
}
