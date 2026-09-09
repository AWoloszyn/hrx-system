// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/xdna/umd/memory.h"

#include <drm/amdxdna_accel.h>
#include <limits.h>
#include <stdint.h>

#include "libamdf/src/allocator.h"
#include "libamdf/src/platform/linux/dma_buf.h"
#include "libamdf/src/platform/linux/host_cache.h"
#include "libamdf/src/xdna/umd/drm/device.h"
#include "libamdf/src/xdna/umd/drm/memory.h"

struct amdf_xdna_umd_host_mapping_t {
  // Host allocator copied for independent mapping teardown.
  amdf_allocator_t host_allocator;
  // View into the attachment's persistent mapping, never independently
  // unmapped.
  void* pointer;
  // Native cache-line length in bytes, qualified during device creation.
  uint32_t cache_line_size;
};

amdf_status_t amdf_xdna_umd_memory_destroy(amdf_xdna_umd_memory_t* memory) {
  const amdf_status_t status = amdf_linux_xdna_buffer_deinitialize(
      memory->device->descriptor, &memory->buffer);
  if (amdf_status_is_ok(status)) {
    amdf_free(memory->device->host_allocator, memory);
  }
  return status;
}

static amdf_status_t amdf_linux_xdna_memory_discard(
    amdf_xdna_umd_memory_t* memory) {
  const amdf_status_t status = amdf_linux_xdna_buffer_deinitialize(
      memory->device->descriptor, &memory->buffer);
  // Failed native release leaves its remaining mappings and GEM references
  // intact. Unpublished metadata does not become a parent-owned retry record.
  amdf_free(memory->device->host_allocator, memory);
  return status;
}

amdf_status_t amdf_xdna_umd_device_query_memory_profile(
    amdf_xdna_umd_device_t* device, uint32_t memory_profile_ordinal,
    amdf_memory_profile_t* out_profile) {
  (void)device;
  if (memory_profile_ordinal != 0) {
    return amdf_make_api_status(AMDF_STATUS_CODE_OUT_OF_RANGE);
  }
  amdf_memory_profile_t profile = {
      .type = AMDF_STRUCTURE_TYPE_MEMORY_PROFILE,
      .structure_size = out_profile->structure_size,
      .next = out_profile->next,
      .ordinal = 0,
      .memory_class = AMDF_MEMORY_CLASS_SYSTEM,
      .roles = AMDF_MEMORY_PROFILE_ROLE_CREATE |
               AMDF_MEMORY_PROFILE_ROLE_IMPORT |
               AMDF_MEMORY_PROFILE_ROLE_HOST_MAP,
      .guaranteed_flags =
          AMDF_MEMORY_FLAG_HOST_VISIBLE | AMDF_MEMORY_FLAG_DEVICE_ADDRESS,
      .supported_flags =
          AMDF_MEMORY_FLAG_HOST_VISIBLE | AMDF_MEMORY_FLAG_DEVICE_ADDRESS,
      .minimum_alignment = 1,
      .external_memory_support_count = 1,
  };
  profile.external_memory_support[0] = (amdf_external_memory_support_t){
      .type = AMDF_EXTERNAL_MEMORY_TYPE_DMA_BUF_FD,
      .flags = AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_IMPORT |
               AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_SOURCE_OFFSET |
               AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_CROSS_PROCESS |
               AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_FOREIGN_API,
      .source_offset_alignment = 1,
      .byte_length_alignment = 1,
  };
  *out_profile = profile;
  return AMDF_STATUS_OK;
}

amdf_status_t amdf_xdna_umd_memory_import(
    amdf_xdna_umd_device_t* device,
    const amdf_memory_import_info_t* import_info,
    const amdf_external_memory_t* external_memory,
    amdf_xdna_umd_memory_t** out_memory,
    amdf_xdna_umd_memory_result_t* out_result) {
  if (external_memory->type != AMDF_EXTERNAL_MEMORY_TYPE_DMA_BUF_FD) {
    return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
  }
  if (external_memory->payload.file_descriptor > INT_MAX) {
    return amdf_make_api_status(AMDF_STATUS_CODE_OUT_OF_RANGE);
  }
  const int descriptor = (int)external_memory->payload.file_descriptor;
  amdf_linux_dma_buf_info_t dma_buf_info;
  amdf_status_t status = amdf_linux_dma_buf_query(descriptor, &dma_buf_info);
  if (!amdf_status_is_ok(status)) return status;
  if (amdf_physical_memory_id_is_valid(&external_memory->physical_backing_id) &&
      !amdf_physical_memory_id_is_equal(&external_memory->physical_backing_id,
                                        &dma_buf_info.physical_backing_id)) {
    return amdf_make_api_status(AMDF_STATUS_CODE_FAILED_PRECONDITION);
  }
  if (external_memory->source_byte_offset > dma_buf_info.byte_length ||
      external_memory->byte_length >
          dma_buf_info.byte_length - external_memory->source_byte_offset ||
      dma_buf_info.byte_length > SIZE_MAX) {
    return amdf_make_api_status(AMDF_STATUS_CODE_OUT_OF_RANGE);
  }
  if (import_info->minimum_alignment != 0 &&
      external_memory->source_byte_offset % import_info->minimum_alignment !=
          0) {
    return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
  }
  const uint64_t alignment = import_info->minimum_alignment > device->page_size
                                 ? import_info->minimum_alignment
                                 : device->page_size;
  if (alignment > PTRDIFF_MAX ||
      dma_buf_info.byte_length > (uint64_t)PTRDIFF_MAX - alignment) {
    return amdf_make_api_status(AMDF_STATUS_CODE_OUT_OF_RANGE);
  }

  amdf_xdna_umd_memory_t* memory = NULL;
  status = amdf_calloc(device->host_allocator, sizeof(*memory),
                       _Alignof(amdf_xdna_umd_memory_t), (void**)&memory);
  if (!amdf_status_is_ok(status)) return status;
  memory->device = device;
  memory->source_byte_offset = external_memory->source_byte_offset;
  memory->physical_backing_id = dma_buf_info.physical_backing_id;
  status = amdf_linux_xdna_buffer_import_dma_buf(
      device->descriptor, descriptor, (size_t)dma_buf_info.byte_length,
      &memory->buffer);
  if (amdf_status_is_ok(status)) {
    status =
        amdf_linux_xdna_buffer_attach(device->descriptor, (size_t)alignment,
                                      device->page_size, NULL, &memory->buffer);
  }
  if (amdf_status_is_ok(status) &&
      memory->buffer.device_address >
          UINT64_MAX - external_memory->source_byte_offset) {
    status = amdf_make_api_status(AMDF_STATUS_CODE_INTERNAL);
  }
  if (amdf_status_is_ok(status)) {
    const uint64_t offset_alignment =
        external_memory->source_byte_offset == 0
            ? alignment
            : external_memory->source_byte_offset &
                  (UINT64_C(0) - external_memory->source_byte_offset);
    const uint64_t logical_alignment =
        offset_alignment < alignment ? offset_alignment : alignment;
    const amdf_xdna_umd_memory_result_t result = {
        .memory_profile_ordinal = 0,
        .memory_class = AMDF_MEMORY_CLASS_SYSTEM,
        .flags =
            AMDF_MEMORY_FLAG_HOST_VISIBLE | AMDF_MEMORY_FLAG_DEVICE_ADDRESS,
        .source_byte_offset = external_memory->source_byte_offset,
        .byte_length = external_memory->byte_length,
        .alignment = logical_alignment,
        .physical_backing_id = dma_buf_info.physical_backing_id,
        .device_address =
            memory->buffer.device_address + external_memory->source_byte_offset,
    };
    if (external_memory->release != NULL) {
      external_memory->release(external_memory->release_user_data,
                               external_memory->type, external_memory->payload);
    }
    *out_result = result;
    *out_memory = memory;
  } else {
    const amdf_status_t release_status = amdf_linux_xdna_memory_discard(memory);
    if (!amdf_status_is_ok(release_status)) {
      status = release_status;
    }
  }
  return status;
}

amdf_status_t amdf_xdna_umd_memory_export(
    amdf_xdna_umd_memory_t* memory,
    const amdf_memory_export_info_t* export_info,
    amdf_external_memory_t* out_value) {
  (void)memory;
  (void)export_info;
  (void)out_value;
  return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
}

amdf_status_t amdf_xdna_umd_memory_query_pair_info(
    amdf_xdna_umd_memory_t* memory, const amdf_memory_pair_query_t* query,
    amdf_memory_pair_info_t* out_info) {
  (void)memory;
  (void)query;
  (void)out_info;
  return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
}

amdf_status_t amdf_xdna_umd_memory_create(
    amdf_xdna_umd_device_t* device,
    const amdf_memory_create_info_t* create_info,
    amdf_xdna_umd_memory_t** out_memory,
    amdf_xdna_umd_memory_result_t* out_result) {
  const amdf_memory_flags_t flags =
      AMDF_MEMORY_FLAG_HOST_VISIBLE | AMDF_MEMORY_FLAG_DEVICE_ADDRESS;
  if (create_info->memory_class != AMDF_MEMORY_CLASS_SYSTEM ||
      (create_info->required_flags & ~flags) != 0) {
    return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
  }
  const uint64_t alignment = create_info->minimum_alignment > device->page_size
                                 ? create_info->minimum_alignment
                                 : device->page_size;
  // Bound both pointer arithmetic and the optional aligned mmap reservation.
  if (alignment > PTRDIFF_MAX ||
      create_info->byte_length >
          (uint64_t)PTRDIFF_MAX - alignment - (device->page_size - 1)) {
    return amdf_make_api_status(AMDF_STATUS_CODE_OUT_OF_RANGE);
  }
  const size_t byte_length =
      (create_info->byte_length + device->page_size - 1) &
      ~(device->page_size - 1);
  amdf_xdna_umd_memory_t* memory = NULL;
  amdf_status_t status =
      amdf_calloc(device->host_allocator, sizeof(*memory),
                  _Alignof(amdf_xdna_umd_memory_t), (void**)&memory);
  if (!amdf_status_is_ok(status)) return status;
  memory->device = device;
  memory->source_byte_offset = 0;
  status = amdf_linux_xdna_buffer_create(device->descriptor, AMDXDNA_BO_SHARE,
                                         byte_length, &memory->buffer);
  if (amdf_status_is_ok(status)) {
    status =
        amdf_linux_xdna_buffer_attach(device->descriptor, alignment,
                                      device->page_size, NULL, &memory->buffer);
  }
  if (amdf_status_is_ok(status)) {
    memory->physical_backing_id = (amdf_physical_memory_id_t){
        .words = {(uintptr_t)device, memory->buffer.handle},
    };
    amdf_xdna_umd_memory_result_t result = {0};
    result.memory_profile_ordinal = 0;
    result.memory_class = AMDF_MEMORY_CLASS_SYSTEM;
    result.flags = flags;
    result.source_byte_offset = 0;
    result.byte_length = byte_length;
    result.alignment = alignment;
    result.physical_backing_id = memory->physical_backing_id;
    result.device_address = memory->buffer.device_address;
    *out_result = result;
    *out_memory = memory;
  } else {
    const amdf_status_t release_status = amdf_linux_xdna_memory_discard(memory);
    if (!amdf_status_is_ok(release_status)) {
      status = release_status;
    }
  }
  return status;
}

amdf_status_t amdf_xdna_umd_memory_map(
    amdf_xdna_umd_memory_t* memory, const amdf_memory_map_info_t* map_info,
    amdf_xdna_umd_host_mapping_t** out_mapping,
    amdf_xdna_umd_host_mapping_result_t* out_result) {
  amdf_xdna_umd_host_mapping_t* mapping = NULL;
  amdf_status_t status =
      amdf_calloc(memory->device->host_allocator, sizeof(*mapping),
                  _Alignof(amdf_xdna_umd_host_mapping_t), (void**)&mapping);
  if (!amdf_status_is_ok(status)) return status;
  mapping->host_allocator = memory->device->host_allocator;
  mapping->pointer = (uint8_t*)memory->buffer.host_pointer +
                     memory->source_byte_offset + map_info->byte_offset;
  mapping->cache_line_size = memory->device->cache_line_size;
  amdf_xdna_umd_host_mapping_result_t result = {0};
  result.flags = map_info->flags;
  result.pointer = mapping->pointer;
  result.byte_length = map_info->byte_length;
  result.cacheability = AMDF_HOST_CACHEABILITY_WRITE_BACK;
  result.cache_line_size = mapping->cache_line_size;
  *out_result = result;
  *out_mapping = mapping;
  return AMDF_STATUS_OK;
}

amdf_status_t amdf_xdna_umd_host_mapping_cache_control(
    amdf_xdna_umd_host_mapping_t* mapping,
    amdf_host_cache_operation_t operation, uint64_t byte_offset,
    uint64_t byte_length) {
  // The public mapping boundary has already validated operation and range.
  (void)operation;
  amdf_linux_host_cache_transfer((uint8_t*)mapping->pointer + byte_offset,
                                 byte_length, mapping->cache_line_size);
  return AMDF_STATUS_OK;
}

amdf_status_t amdf_xdna_umd_host_mapping_destroy(
    amdf_xdna_umd_host_mapping_t* mapping) {
  amdf_free(mapping->host_allocator, mapping);
  return AMDF_STATUS_OK;
}
