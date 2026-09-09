// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/gpu/umd/kfd/target/gfx1151/pm4_queue.h"

#include <linux/kfd_ioctl.h>
#include <string.h>

enum {
  AMDF_GPU_KFD_GFX1151_PAGE_SIZE = 4096,
  AMDF_GPU_KFD_GFX1151_RING_BYTE_LENGTH = 4096,
  AMDF_GPU_KFD_GFX1151_END_OF_PIPE_BYTE_LENGTH = 4096,
  AMDF_GPU_KFD_GFX1151_DOORBELL_MAPPING_BYTE_LENGTH = 8192,
  AMDF_GPU_KFD_GFX1151_WAVE_COUNT_PER_COMPUTE_UNIT = 32,
  AMDF_GPU_KFD_GFX1151_DEBUG_BYTE_LENGTH_PER_WAVE = 32,
};

bool amdf_gpu_kfd_gfx1151_pm4_queue_is_supported(
    const amdf_gpu_kfd_topology_t* topology, size_t page_size,
    uint32_t cache_line_size) {
  if (topology == NULL || page_size != AMDF_GPU_KFD_GFX1151_PAGE_SIZE ||
      cache_line_size < sizeof(uint64_t) ||
      (cache_line_size & (cache_line_size - 1)) != 0 ||
      cache_line_size > page_size / 3 ||
      topology->properties.gfx_ip.major != 11 ||
      topology->properties.gfx_ip.minor != 5 ||
      topology->properties.gfx_ip.stepping != 1 ||
      topology->properties.compute.wavefront_size != 32 ||
      topology->properties.compute.compute_unit_count == 0 ||
      topology->properties.compute.maximum_wave_count_per_compute_unit !=
          AMDF_GPU_KFD_GFX1151_WAVE_COUNT_PER_COMPUTE_UNIT ||
      topology->properties.topology.xcc_count != 1 ||
      topology->compute_queue_count == 0 ||
      topology->context_save_restore_byte_length == 0 ||
      topology->context_save_restore_byte_length % page_size != 0 ||
      topology->control_stack_byte_length == 0 ||
      topology->control_stack_byte_length % page_size != 0 ||
      topology->control_stack_byte_length >
          topology->context_save_restore_byte_length ||
      topology->virtual_address.alignment != page_size) {
    return false;
  }
  const uint64_t debug_byte_length =
      (uint64_t)topology->properties.compute.compute_unit_count *
      AMDF_GPU_KFD_GFX1151_WAVE_COUNT_PER_COMPUTE_UNIT *
      AMDF_GPU_KFD_GFX1151_DEBUG_BYTE_LENGTH_PER_WAVE;
  return debug_byte_length <= UINT32_MAX &&
         topology->context_save_restore_byte_length <=
             SIZE_MAX - (size_t)debug_byte_length;
}

amdf_gpu_queue_family_properties_t
amdf_gpu_kfd_gfx1151_pm4_queue_family_properties(void) {
  return (amdf_gpu_queue_family_properties_t){
      .command_type = AMDF_QUEUE_COMMAND_TYPE_GPU_PM4,
      .format_version = AMDF_GPU_PM4_QUEUE_FORMAT_VERSION_1,
      .publication_modes = AMDF_QUEUE_PUBLICATION_MODE_USER,
      .roles = AMDF_QUEUE_ROLE_COMPUTE | AMDF_QUEUE_ROLE_TRANSFER |
               AMDF_QUEUE_ROLE_ATOMIC | AMDF_QUEUE_ROLE_CACHE_CONTROL,
      .user_queue_capabilities = AMDF_USER_QUEUE_CAPABILITY_HOST_PRODUCER,
      .producer_modes = AMDF_QUEUE_PRODUCER_MODE_BIT_SINGLE,
      .priority_capabilities = AMDF_QUEUE_PRIORITY_CAPABILITY_NORMAL,
      .minimum_ring_byte_length = AMDF_GPU_KFD_GFX1151_RING_BYTE_LENGTH,
      .maximum_ring_byte_length = AMDF_GPU_KFD_GFX1151_RING_BYTE_LENGTH,
      .ring_byte_length_alignment = AMDF_GPU_KFD_GFX1151_RING_BYTE_LENGTH,
  };
}

amdf_gpu_kfd_gfx1151_pm4_queue_layout_t amdf_gpu_kfd_gfx1151_pm4_queue_layout(
    const amdf_gpu_kfd_topology_t* topology, uint32_t cache_line_size) {
  const uint32_t debug_byte_length =
      topology->properties.compute.compute_unit_count *
      AMDF_GPU_KFD_GFX1151_WAVE_COUNT_PER_COMPUTE_UNIT *
      AMDF_GPU_KFD_GFX1151_DEBUG_BYTE_LENGTH_PER_WAVE;
  const size_t context_byte_length =
      topology->context_save_restore_byte_length + debug_byte_length;
  return (amdf_gpu_kfd_gfx1151_pm4_queue_layout_t){
      .ring_byte_length = AMDF_GPU_KFD_GFX1151_RING_BYTE_LENGTH,
      .control_byte_length = AMDF_GPU_KFD_GFX1151_PAGE_SIZE,
      .read_index_byte_offset = 0,
      .write_index_byte_offset = cache_line_size,
      .error_payload_byte_offset = 2u * cache_line_size,
      .end_of_pipe_byte_length = AMDF_GPU_KFD_GFX1151_END_OF_PIPE_BYTE_LENGTH,
      .context_save_restore_byte_length =
          topology->context_save_restore_byte_length,
      .debug_byte_offset = topology->context_save_restore_byte_length,
      .debug_byte_length = debug_byte_length,
      .context_allocation_byte_length =
          (context_byte_length + AMDF_GPU_KFD_GFX1151_PAGE_SIZE - 1) &
          ~(size_t)(AMDF_GPU_KFD_GFX1151_PAGE_SIZE - 1),
      .control_stack_byte_length = topology->control_stack_byte_length,
      .doorbell_mapping_byte_length =
          AMDF_GPU_KFD_GFX1151_DOORBELL_MAPPING_BYTE_LENGTH,
  };
}

void amdf_gpu_kfd_gfx1151_pm4_queue_initialize_context_header(
    void* context_address,
    const amdf_gpu_kfd_gfx1151_pm4_queue_layout_t* layout,
    uint64_t error_payload_address) {
  struct kfd_context_save_area_header* header =
      (struct kfd_context_save_area_header*)context_address;
  memset(header, 0, sizeof(*header));
  header->debug_offset = layout->debug_byte_offset;
  header->debug_size = layout->debug_byte_length;
  header->err_payload_addr = error_payload_address;
}
