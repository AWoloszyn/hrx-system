// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/gpu/memory.h"

#include <stddef.h>

#include "libamdf/src/allocator.h"
#include "libamdf/src/device.h"
#include "libamdf/src/endpoint.h"
#include "libamdf/src/gpu/device.h"
#include "libamdf/src/gpu/umd/memory.h"
#include "libamdf/src/host_mapping.h"
#include "libamdf/src/memory.h"

typedef struct amdf_gpu_memory_t {
  // Generic memory state shared by every engine implementation.
  amdf_memory_t base;
  // Exact native physical backing and GPU attachment.
  amdf_gpu_umd_memory_t* umd;
} amdf_gpu_memory_t;

typedef struct amdf_gpu_host_mapping_t {
  // Generic host mapping state shared by every engine implementation.
  amdf_host_mapping_t base;
  // Exact native host mapping state.
  amdf_gpu_umd_host_mapping_t* umd;
} amdf_gpu_host_mapping_t;

_Static_assert(offsetof(amdf_gpu_memory_t, base) == 0,
               "GPU memory base must be the first field");
_Static_assert(offsetof(amdf_gpu_host_mapping_t, base) == 0,
               "GPU mapping base must be the first field");

static amdf_status_t amdf_gpu_memory_export(
    amdf_memory_t* base_memory, const amdf_memory_export_info_t* export_info,
    amdf_external_memory_t* out_value) {
  amdf_gpu_memory_t* memory = (amdf_gpu_memory_t*)base_memory;
  return amdf_gpu_umd_memory_export(memory->umd, export_info, out_value);
}

static amdf_status_t amdf_gpu_memory_describe_site(
    amdf_memory_t* base_memory, uint32_t queue_family_ordinal,
    amdf_memory_site_description_t* out_description) {
  amdf_queue_family_info_t queue_family_info = {
      .type = AMDF_STRUCTURE_TYPE_QUEUE_FAMILY_INFO,
      .structure_size = sizeof(queue_family_info),
  };
  const amdf_status_t status = amdf_endpoint_query_queue_family_info(
      base_memory->device->endpoint, queue_family_ordinal, &queue_family_info);
  if (!amdf_status_is_ok(status)) return status;
  const amdf_memory_site_query_t query = {
      .memory_info = &base_memory->info,
      .queue_family_info = &queue_family_info,
  };
  amdf_gpu_memory_t* memory = (amdf_gpu_memory_t*)base_memory;
  return amdf_gpu_umd_memory_describe_site(memory->umd, &query,
                                           out_description);
}

static amdf_status_t amdf_gpu_host_mapping_cache_control(
    amdf_host_mapping_t* base_mapping, amdf_host_cache_operation_t operation,
    uint64_t byte_offset, uint64_t byte_length) {
  amdf_gpu_host_mapping_t* mapping = (amdf_gpu_host_mapping_t*)base_mapping;
  return amdf_gpu_umd_host_mapping_cache_control(
      mapping->umd, operation,
      base_mapping->info.memory_byte_offset + byte_offset, byte_length);
}

static amdf_status_t amdf_gpu_host_mapping_destroy_native(
    amdf_host_mapping_t* base_mapping) {
  amdf_gpu_host_mapping_t* mapping = (amdf_gpu_host_mapping_t*)base_mapping;
  const amdf_status_t status = amdf_gpu_umd_host_mapping_destroy(mapping->umd);
  if (amdf_status_is_ok(status)) {
    mapping->umd = NULL;
  }
  return status;
}

static const amdf_host_mapping_vtable_t amdf_gpu_host_mapping_vtable = {
    .cache_control = amdf_gpu_host_mapping_cache_control,
    .destroy_native = amdf_gpu_host_mapping_destroy_native,
};

static amdf_status_t amdf_gpu_memory_map(amdf_memory_t* base_memory,
                                         const amdf_memory_profile_t* profile,
                                         const amdf_memory_map_info_t* map_info,
                                         amdf_host_mapping_t** out_mapping) {
  amdf_gpu_memory_t* memory = (amdf_gpu_memory_t*)base_memory;
  const amdf_allocator_t host_allocator =
      amdf_memory_host_allocator(base_memory);
  amdf_gpu_host_mapping_t* mapping = NULL;
  amdf_status_t status =
      amdf_calloc(host_allocator, sizeof(*mapping),
                  amdf_alignof(amdf_gpu_host_mapping_t), (void**)&mapping);
  if (!amdf_status_is_ok(status)) return status;
  status = amdf_host_mapping_initialize(
      &mapping->base, &amdf_gpu_host_mapping_vtable, base_memory);

  amdf_gpu_umd_host_mapping_result_t result = {0};
  if (amdf_status_is_ok(status)) {
    status = amdf_gpu_umd_memory_map(memory->umd, profile, map_info,
                                     &mapping->umd, &result);
  }
  if (amdf_status_is_ok(status)) {
    amdf_assert((result.flags & map_info->flags) == map_info->flags &&
                (result.flags & ~profile->host_mapping.supported_access) == 0 &&
                result.pointer != NULL &&
                result.byte_length == map_info->byte_length &&
                "GPU host mapping must achieve the selected profile request");
    mapping->base.info.type = AMDF_STRUCTURE_TYPE_HOST_MAPPING_INFO;
    mapping->base.info.structure_size = sizeof(mapping->base.info);
    mapping->base.info.flags = result.flags;
    mapping->base.info.cacheability = result.cacheability;
    mapping->base.info.pointer = result.pointer;
    mapping->base.info.memory_byte_offset = map_info->byte_offset;
    mapping->base.info.byte_length = result.byte_length;
    mapping->base.info.byte_offset_granularity =
        profile->host_mapping.byte_offset_granularity;
    mapping->base.info.byte_length_granularity =
        profile->host_mapping.byte_length_granularity;
    mapping->base.info.cache_line_size = result.cache_line_size;
    mapping->base.info.flush = result.flush;
    mapping->base.info.invalidate = result.invalidate;
    *out_mapping = &mapping->base;
  } else {
    if (mapping->base.memory != NULL) {
      amdf_host_mapping_deinitialize(&mapping->base);
    }
    amdf_free(host_allocator, mapping);
  }
  return status;
}

static amdf_status_t amdf_gpu_memory_destroy_native(
    amdf_memory_t* base_memory) {
  amdf_gpu_memory_t* memory = (amdf_gpu_memory_t*)base_memory;
  const amdf_status_t status = amdf_gpu_umd_memory_destroy(memory->umd);
  if (amdf_status_is_ok(status)) {
    memory->umd = NULL;
  }
  return status;
}

static const amdf_memory_vtable_t amdf_gpu_memory_vtable = {
    .export_external = amdf_gpu_memory_export,
    .describe_site = amdf_gpu_memory_describe_site,
    .map = amdf_gpu_memory_map,
    .destroy_native = amdf_gpu_memory_destroy_native,
};

amdf_status_t amdf_gpu_device_query_memory_profile(
    amdf_device_t* device, uint32_t memory_profile_ordinal,
    amdf_memory_profile_t* out_profile) {
  return amdf_gpu_umd_device_query_memory_profile(
      amdf_gpu_device_get_umd(device), memory_profile_ordinal, out_profile);
}

static void amdf_gpu_memory_set_info(amdf_gpu_memory_t* memory,
                                     amdf_device_t* device,
                                     const amdf_memory_profile_t* profile,
                                     amdf_memory_access_t device_access,
                                     amdf_gpu_umd_memory_result_t result) {
  memory->base.info.type = AMDF_STRUCTURE_TYPE_MEMORY_INFO;
  memory->base.info.structure_size = sizeof(memory->base.info);
  memory->base.info.memory_profile_ordinal = profile->ordinal;
  memory->base.info.memory_class = profile->memory_class;
  memory->base.info.device_access = device_access;
  memory->base.info.atomic_operations_32 = result.atomic_operations_32;
  memory->base.info.atomic_operations_64 = result.atomic_operations_64;
  memory->base.info.address_domain_ordinal =
      profile->device_address.address_domain_ordinal;
  memory->base.info.device_id = amdf_gpu_device_get_info(device)->id;
  memory->base.info.flags = result.flags;
  memory->base.info.source_byte_offset = result.source_byte_offset;
  memory->base.info.byte_length = result.byte_length;
  memory->base.info.alignment = result.alignment;
  memory->base.info.native_allocation_byte_length =
      result.native_allocation_byte_length;
  memory->base.info.native_allocation_granularity =
      result.native_allocation_granularity;
  memory->base.info.physical_backing_id = result.physical_backing_id;
  memory->base.addresses[AMDF_MEMORY_ADDRESS_GPU] = result.device_address;
  memory->base.info.address_kinds =
      (result.flags & AMDF_MEMORY_FLAG_DEVICE_ADDRESS) != 0
          ? UINT64_C(1) << AMDF_MEMORY_ADDRESS_GPU
          : 0;
  memory->base.info.reset_epoch = amdf_gpu_device_query_reset_epoch(device);
}

// Rollback belongs to the constructing memory owner. A terminal native error
// preserves any referenced backing but never retains unpublished bookkeeping.
static amdf_status_t amdf_gpu_memory_discard(amdf_gpu_memory_t* memory) {
  amdf_status_t status = AMDF_STATUS_OK;
  if (memory->umd != NULL) {
    status = amdf_gpu_umd_memory_destroy(memory->umd);
    if (!amdf_status_is_ok(status)) {
      amdf_gpu_umd_memory_abandon(memory->umd);
    }
  }
  amdf_free(amdf_memory_host_allocator(&memory->base), memory);
  return status;
}

amdf_status_t amdf_gpu_memory_create(
    amdf_device_t* device, const amdf_memory_profile_t* profile,
    const amdf_memory_create_info_t* create_info, amdf_memory_t** out_memory) {
  const amdf_allocator_t host_allocator = amdf_device_host_allocator(device);
  amdf_gpu_memory_t* memory = NULL;
  amdf_status_t status =
      amdf_calloc(host_allocator, sizeof(*memory),
                  amdf_alignof(amdf_gpu_memory_t), (void**)&memory);
  if (!amdf_status_is_ok(status)) return status;
  amdf_memory_initialize(&memory->base, &amdf_gpu_memory_vtable, device);

  amdf_gpu_umd_memory_result_t result = {0};
  status = amdf_gpu_umd_memory_prepare(amdf_gpu_device_get_umd(device), profile,
                                       create_info, &memory->umd, &result);
  if (amdf_status_is_ok(status)) {
    amdf_gpu_memory_set_info(memory, device, profile,
                             create_info->device_access, result);
    *out_memory = &memory->base;
  } else {
    const amdf_status_t release_status = amdf_gpu_memory_discard(memory);
    if (!amdf_status_is_ok(release_status)) status = release_status;
  }
  return status;
}

amdf_status_t amdf_gpu_memory_import(
    amdf_device_t* device, const amdf_memory_profile_t* profile,
    const amdf_memory_import_info_t* import_info,
    const amdf_external_memory_t* external_memory, amdf_memory_t** out_memory) {
  const amdf_allocator_t host_allocator = amdf_device_host_allocator(device);
  amdf_gpu_memory_t* memory = NULL;
  amdf_status_t status =
      amdf_calloc(host_allocator, sizeof(*memory),
                  amdf_alignof(amdf_gpu_memory_t), (void**)&memory);
  if (!amdf_status_is_ok(status)) return status;
  amdf_memory_initialize(&memory->base, &amdf_gpu_memory_vtable, device);

  amdf_gpu_umd_memory_result_t result = {0};
  status = amdf_gpu_umd_memory_prepare_import(
      amdf_gpu_device_get_umd(device), profile, import_info, external_memory,
      &memory->umd, &result);
  if (amdf_status_is_ok(status)) {
    amdf_gpu_memory_set_info(memory, device, profile,
                             import_info->device_access, result);
    if (external_memory->release != NULL) {
      external_memory->release(external_memory->release_user_data,
                               external_memory->type, external_memory->payload);
    }
    *out_memory = &memory->base;
  } else {
    const amdf_status_t release_status = amdf_gpu_memory_discard(memory);
    if (!amdf_status_is_ok(release_status)) status = release_status;
  }
  return status;
}

amdf_gpu_umd_memory_t* amdf_gpu_memory_get_umd(amdf_memory_t* base_memory) {
  return ((amdf_gpu_memory_t*)base_memory)->umd;
}
