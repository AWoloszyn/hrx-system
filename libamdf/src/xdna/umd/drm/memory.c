// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/xdna/umd/memory.h"

#include <assert.h>
#include <drm/amdxdna_accel.h>
#include <limits.h>
#include <stdint.h>

#include "libamdf/src/allocator.h"
#include "libamdf/src/platform/linux/dma_buf.h"
#include "libamdf/src/platform/linux/file.h"
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

static amdf_status_t amdf_linux_xdna_memory_query_dma_buf(
    amdf_xdna_umd_memory_t* memory, amdf_linux_dma_buf_info_t* out_info) {
  int descriptor = -1;
  amdf_status_t status = amdf_linux_xdna_buffer_export_dma_buf(
      memory->device->descriptor, &memory->buffer, &descriptor);
  amdf_linux_dma_buf_info_t info;
  if (amdf_status_is_ok(status)) {
    status = amdf_linux_dma_buf_query(descriptor, &info);
  }
  if (amdf_status_is_ok(status) &&
      info.byte_length != memory->buffer.byte_length) {
    status = amdf_make_api_status(AMDF_STATUS_CODE_INTERNAL);
  }
  const amdf_status_t close_status = amdf_linux_file_close(&descriptor);
  if (!amdf_status_is_ok(close_status)) status = close_status;
  if (amdf_status_is_ok(status)) *out_info = info;
  return status;
}

amdf_status_t amdf_xdna_umd_device_query_memory_profile(
    amdf_xdna_umd_device_t* device, uint32_t memory_profile_ordinal,
    amdf_memory_profile_t* out_profile) {
  if (memory_profile_ordinal > 2) {
    return amdf_make_api_status(AMDF_STATUS_CODE_OUT_OF_RANGE);
  }
  const uint64_t maximum_byte_length =
      ((uint64_t)PTRDIFF_MAX / 2) & ~(uint64_t)(device->page_size - 1);
  // SHARE buffers use either caller SVA or a driver-assigned IOVA. Page
  // alignment is the strongest address guarantee common to both modes.
  amdf_memory_profile_t profile = {
      .type = AMDF_STRUCTURE_TYPE_MEMORY_PROFILE,
      .structure_size = out_profile->structure_size,
      .next = out_profile->next,
      .ordinal = memory_profile_ordinal,
      .guaranteed_flags =
          AMDF_MEMORY_FLAG_HOST_VISIBLE | AMDF_MEMORY_FLAG_DEVICE_ADDRESS,
      .supported_flags =
          AMDF_MEMORY_FLAG_HOST_VISIBLE | AMDF_MEMORY_FLAG_DEVICE_ADDRESS,
      .guaranteed_device_access =
          AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
      .supported_device_access =
          AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
      .device_address =
          {
              .address_domain_ordinal = 0,
              .address_bit_count = AMDF_MEMORY_ADDRESS_BIT_COUNT_UNKNOWN,
              .minimum_address = 0,
              .maximum_address = 0,
              .minimum_alignment = 1,
          },
      .host_mapping =
          {
              .maximum_byte_length = maximum_byte_length,
              .byte_offset_granularity = 1,
              .byte_length_granularity = 1,
              .supported_access =
                  AMDF_MEMORY_MAP_FLAG_READ | AMDF_MEMORY_MAP_FLAG_WRITE,
          },
  };
  if (memory_profile_ordinal == 0) {
    profile.memory_class = AMDF_MEMORY_CLASS_SYSTEM;
    profile.roles = AMDF_MEMORY_PROFILE_ROLE_CREATE |
                    AMDF_MEMORY_PROFILE_ROLE_EXPORT |
                    AMDF_MEMORY_PROFILE_ROLE_HOST_MAP;
    profile.guaranteed_flags |= AMDF_MEMORY_FLAG_SHAREABLE;
    profile.supported_flags |= AMDF_MEMORY_FLAG_SHAREABLE;
    profile.allocation = (amdf_memory_construction_capabilities_t){
        .maximum_byte_length = maximum_byte_length,
        .byte_length_granularity = 1,
        .minimum_alignment = device->page_size,
        .maximum_alignment = device->page_size,
        .native_byte_length_granularity = device->page_size,
    };
    profile.external_memory_support_count = 1;
    profile.external_memory_support[0] = (amdf_external_memory_support_t){
        .type = AMDF_EXTERNAL_MEMORY_TYPE_DMA_BUF_FD,
        .flags = AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_EXPORT |
                 AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_SOURCE_OFFSET |
                 AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_CROSS_PROCESS,
        .source_offset_alignment = 1,
        .byte_length_alignment = 1,
    };
  } else if (memory_profile_ordinal == 1) {
    profile.memory_class = AMDF_MEMORY_CLASS_SYSTEM;
    profile.roles =
        AMDF_MEMORY_PROFILE_ROLE_IMPORT | AMDF_MEMORY_PROFILE_ROLE_HOST_MAP;
    profile.import = (amdf_memory_construction_capabilities_t){
        .maximum_byte_length = maximum_byte_length,
        .byte_length_granularity = 1,
        .minimum_alignment = 1,
        .maximum_alignment = device->page_size,
        .native_byte_length_granularity = device->page_size,
    };
    profile.external_memory_support_count = 1;
    profile.external_memory_support[0] = (amdf_external_memory_support_t){
        .type = AMDF_EXTERNAL_MEMORY_TYPE_DMA_BUF_FD,
        .flags = AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_IMPORT |
                 AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_SOURCE_OFFSET |
                 AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_CROSS_PROCESS |
                 AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_FOREIGN_API,
        .source_offset_alignment = 1,
        .byte_length_alignment = 1,
    };
  } else {
    profile.memory_class = AMDF_MEMORY_CLASS_REGISTERED_HOST;
    profile.roles =
        AMDF_MEMORY_PROFILE_ROLE_REGISTER | AMDF_MEMORY_PROFILE_ROLE_HOST_MAP;
    profile.registration = (amdf_memory_construction_capabilities_t){
        .maximum_byte_length = maximum_byte_length,
        .byte_length_granularity = 1,
        .registered_host_pointer_alignment = 1,
        .minimum_alignment = 1,
        .maximum_alignment = device->page_size,
        .native_byte_length_granularity = device->page_size,
    };
  }
  *out_profile = profile;
  return AMDF_STATUS_OK;
}

amdf_status_t amdf_xdna_umd_memory_import(
    amdf_xdna_umd_device_t* device, const amdf_memory_profile_t* profile,
    const amdf_memory_import_info_t* import_info,
    const amdf_external_memory_t* external_memory,
    amdf_xdna_umd_memory_t** out_memory,
    amdf_xdna_umd_memory_result_t* out_result) {
  assert(external_memory->type == AMDF_EXTERNAL_MEMORY_TYPE_DMA_BUF_FD &&
         "selected XDNA import profile must consume DMA-BUF memory");
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
        .flags = profile->guaranteed_flags,
        .source_byte_offset = external_memory->source_byte_offset,
        .byte_length = external_memory->byte_length,
        .alignment = logical_alignment,
        .native_allocation_byte_length = dma_buf_info.byte_length,
        .native_allocation_granularity = device->page_size,
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
  (void)export_info;
  int descriptor = -1;
  amdf_status_t status = amdf_linux_xdna_buffer_export_dma_buf(
      memory->device->descriptor, &memory->buffer, &descriptor);
  amdf_linux_dma_buf_info_t info;
  if (amdf_status_is_ok(status)) {
    status = amdf_linux_dma_buf_query(descriptor, &info);
  }
  if (amdf_status_is_ok(status) &&
      (info.byte_length != memory->buffer.byte_length ||
       !amdf_physical_memory_id_is_equal(&info.physical_backing_id,
                                         &memory->physical_backing_id))) {
    status = amdf_make_api_status(AMDF_STATUS_CODE_INTERNAL);
  }
  if (amdf_status_is_ok(status)) {
    *out_value = (amdf_external_memory_t){
        .payload.file_descriptor = descriptor,
        .release = amdf_linux_dma_buf_release,
    };
  } else {
    const amdf_status_t close_status = amdf_linux_file_close(&descriptor);
    if (!amdf_status_is_ok(close_status)) status = close_status;
  }
  return status;
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
    amdf_xdna_umd_device_t* device, const amdf_memory_profile_t* profile,
    const amdf_memory_create_info_t* create_info,
    amdf_xdna_umd_memory_t** out_memory,
    amdf_xdna_umd_memory_result_t* out_result) {
  const bool registers_host =
      profile->memory_class == AMDF_MEMORY_CLASS_REGISTERED_HOST;
  uint64_t alignment = 0;
  size_t native_byte_length = 0;
  void* native_host_pointer = NULL;
  uint64_t source_byte_offset = 0;
  if (registers_host) {
    const uintptr_t host_address =
        (uintptr_t)create_info->registered_host_pointer;
    const uintptr_t host_page_address =
        host_address & ~(uintptr_t)(device->page_size - 1);
    source_byte_offset = host_address - host_page_address;
    if (create_info->byte_length > SIZE_MAX - source_byte_offset) {
      return amdf_make_api_status(AMDF_STATUS_CODE_OUT_OF_RANGE);
    }
    const size_t unaligned_native_byte_length =
        (size_t)(source_byte_offset + create_info->byte_length);
    if (unaligned_native_byte_length > SIZE_MAX - (device->page_size - 1)) {
      return amdf_make_api_status(AMDF_STATUS_CODE_OUT_OF_RANGE);
    }
    native_byte_length =
        (unaligned_native_byte_length + device->page_size - 1) &
        ~(device->page_size - 1);
    native_host_pointer = (void*)host_page_address;
    alignment = create_info->minimum_alignment == 0
                    ? 1
                    : create_info->minimum_alignment;
  } else {
    assert(profile->memory_class == AMDF_MEMORY_CLASS_SYSTEM &&
           "selected XDNA construction profile must be system or registered "
           "host memory");
    alignment = create_info->minimum_alignment > device->page_size
                    ? create_info->minimum_alignment
                    : device->page_size;
    native_byte_length = (create_info->byte_length + device->page_size - 1) &
                         ~(device->page_size - 1);
  }

  amdf_xdna_umd_memory_t* memory = NULL;
  amdf_status_t status =
      amdf_calloc(device->host_allocator, sizeof(*memory),
                  _Alignof(amdf_xdna_umd_memory_t), (void**)&memory);
  if (!amdf_status_is_ok(status)) return status;
  memory->device = device;
  memory->source_byte_offset = source_byte_offset;
  if (registers_host) {
    status = amdf_linux_xdna_buffer_register_host_pages(
        device->descriptor, native_host_pointer, native_byte_length,
        &memory->buffer);
    if (amdf_status_is_ok(status)) {
      status = amdf_linux_xdna_buffer_attach(device->descriptor, alignment,
                                             device->page_size, NULL,
                                             &memory->buffer);
    }
  } else {
    status = amdf_linux_xdna_buffer_create(device->descriptor, AMDXDNA_BO_SHARE,
                                           native_byte_length, &memory->buffer);
    if (amdf_status_is_ok(status)) {
      status = amdf_linux_xdna_buffer_attach(device->descriptor, alignment,
                                             device->page_size, NULL,
                                             &memory->buffer);
    }
  }
  if (amdf_status_is_ok(status) && !registers_host) {
    amdf_linux_dma_buf_info_t info;
    status = amdf_linux_xdna_memory_query_dma_buf(memory, &info);
    if (amdf_status_is_ok(status)) {
      memory->physical_backing_id = info.physical_backing_id;
    }
  }
  if (amdf_status_is_ok(status)) {
    const amdf_xdna_umd_memory_result_t result = {
        .flags = profile->guaranteed_flags | create_info->required_flags,
        .source_byte_offset = source_byte_offset,
        .byte_length =
            registers_host ? create_info->byte_length : native_byte_length,
        .alignment = alignment,
        .native_allocation_byte_length = native_byte_length,
        .native_allocation_granularity = device->page_size,
        .physical_backing_id = memory->physical_backing_id,
        .device_address = memory->buffer.device_address + source_byte_offset,
    };
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
    amdf_xdna_umd_memory_t* memory, const amdf_memory_profile_t* profile,
    const amdf_memory_map_info_t* map_info,
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
  result.flags = profile->host_mapping.supported_access;
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
