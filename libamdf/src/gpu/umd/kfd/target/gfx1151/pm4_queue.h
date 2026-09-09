// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_SRC_GPU_UMD_KFD_TARGET_GFX1151_PM4_QUEUE_H_
#define AMDF_SRC_GPU_UMD_KFD_TARGET_GFX1151_PM4_QUEUE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "libamdf/src/gpu/endpoint_profile.h"
#include "libamdf/src/gpu/umd/kfd/topology.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Complete KFD-owned side storage for one gfx1151 PM4 queue.
typedef struct amdf_gpu_kfd_gfx1151_pm4_queue_layout_t {
  // Fixed primary PM4 ring length in bytes.
  size_t ring_byte_length;
  // Host-mapped queue-control page length in bytes.
  size_t control_byte_length;
  // Consumer-owned read-index byte offset in the control page.
  size_t read_index_byte_offset;
  // Producer-owned write-index byte offset in the control page.
  size_t write_index_byte_offset;
  // KFD exception-payload byte offset in the control page.
  size_t error_payload_byte_offset;
  // End-of-pipe ring length in bytes.
  size_t end_of_pipe_byte_length;
  // Context-save/restore bytes reported to KFD.
  uint32_t context_save_restore_byte_length;
  // Debug-state byte offset in the context-save allocation.
  uint32_t debug_byte_offset;
  // Debug-state byte length required by the active compute units.
  uint32_t debug_byte_length;
  // Complete page-covered context-save allocation length in bytes.
  size_t context_allocation_byte_length;
  // Control-stack bytes reported to KFD.
  uint32_t control_stack_byte_length;
  // Native doorbell aperture mapping length in bytes.
  size_t doorbell_mapping_byte_length;
} amdf_gpu_kfd_gfx1151_pm4_queue_layout_t;

// Returns whether the native and host facts implement the qualified gfx1151
// PM4 queue contract.
bool amdf_gpu_kfd_gfx1151_pm4_queue_is_supported(
    const amdf_gpu_kfd_topology_t* topology, size_t page_size,
    uint32_t cache_line_size);

// Returns the exact public queue family implemented by this target module.
amdf_gpu_queue_family_properties_t
amdf_gpu_kfd_gfx1151_pm4_queue_family_properties(void);

// Derives the infallible native storage layout from qualified device facts.
amdf_gpu_kfd_gfx1151_pm4_queue_layout_t amdf_gpu_kfd_gfx1151_pm4_queue_layout(
    const amdf_gpu_kfd_topology_t* topology, uint32_t cache_line_size);

// Initializes the KFD context-save header in already-zeroed queue storage.
void amdf_gpu_kfd_gfx1151_pm4_queue_initialize_context_header(
    void* context_address,
    const amdf_gpu_kfd_gfx1151_pm4_queue_layout_t* layout,
    uint64_t error_payload_address);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // AMDF_SRC_GPU_UMD_KFD_TARGET_GFX1151_PM4_QUEUE_H_
