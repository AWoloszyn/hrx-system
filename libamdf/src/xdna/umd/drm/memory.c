// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/xdna/umd/memory.h"

#include <drm/amdxdna_accel.h>
#include <stdint.h>

#include "libamdf/src/allocator.h"
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
  status = amdf_linux_xdna_buffer_create(device->descriptor, AMDXDNA_BO_SHARE,
                                         byte_length, &memory->buffer);
  if (amdf_status_is_ok(status)) {
    status =
        amdf_linux_xdna_buffer_attach(device->descriptor, alignment,
                                      device->page_size, NULL, &memory->buffer);
  }
  if (amdf_status_is_ok(status)) {
    amdf_xdna_umd_memory_result_t result = {0};
    result.memory_class = AMDF_MEMORY_CLASS_SYSTEM;
    result.flags = flags;
    result.byte_length = byte_length;
    result.alignment = alignment;
    result.physical_backing_id.words[0] = (uintptr_t)device;
    result.physical_backing_id.words[1] = memory->buffer.handle;
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
  mapping->pointer =
      (uint8_t*)memory->buffer.host_pointer + map_info->byte_offset;
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
