// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Native executable backed by qualified XDNA images and immutable streams.

#ifndef IREE_EXPERIMENTAL_XDNA_EXECUTABLE_H_
#define IREE_EXPERIMENTAL_XDNA_EXECUTABLE_H_

#include "iree/base/api.h"
#include "iree/base/byte_sequence.h"
#include "iree/hal/drivers/amd/xdna/image/aie2p/native_image.h"
#include "iree/hal/executable.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Reference-counted, immutable decoded image with no native device resources.
// This standalone adapter has no HAL device or queue-family identity. Function
// metadata uses HAL reflection value types without creating a HAL executable.
typedef struct iree_hal_amd_xdna_executable_t iree_hal_amd_xdna_executable_t;

// Native objects and immutable metadata selected by one executable entry.
//
// All pointers borrow storage from the executable and remain valid until it is
// destroyed. Native bytes are instantiated into caller-owned instruction
// memory.
typedef struct iree_hal_amd_xdna_executable_entry_t {
  // Initialization ARRAY transaction shared by entries with the same placement.
  iree_const_byte_span_t array;
  // Immutable CONTROL transaction and its cold binding relocation records.
  iree_hal_amd_xdna_aie2p_native_entry_t native;
  // Number of dense buffer bindings accepted by this entry.
  uint32_t binding_count;
} iree_hal_amd_xdna_executable_entry_t;

// Creates a native XDNA executable from one canonical image.
//
// The image is completely decoded, target-qualified, and lowered without
// activating a device or allocating native driver resources.
// |source_sequence| and |target| are borrowed during the call. The executable
// retains the source storage it needs. Success publishes one owning reference;
// on failure, |out_executable| is unchanged.
iree_status_t iree_hal_amd_xdna_executable_create(
    iree_byte_sequence_t* source_sequence,
    const iree_hal_amd_xdna_aie2p_target_t* target,
    iree_allocator_t host_allocator,
    iree_hal_amd_xdna_executable_t** out_executable);

// Retains a reference to |executable|, which may be NULL.
void iree_hal_amd_xdna_executable_retain(
    iree_hal_amd_xdna_executable_t* executable);

// Releases a reference to |executable|, which may be NULL.
void iree_hal_amd_xdna_executable_release(
    iree_hal_amd_xdna_executable_t* executable);

// Copies function metadata into |out_info|. Names borrow executable storage.
// Only index function IDs are accepted. On failure, |out_info| is unchanged.
iree_status_t iree_hal_amd_xdna_executable_function_info(
    const iree_hal_amd_xdna_executable_t* executable,
    iree_hal_executable_function_t function,
    iree_hal_executable_function_info_t* out_info);

// Finds an exported function by exact name. On failure, |out_function| is
// unchanged. The returned index identifies a function within this executable.
iree_status_t iree_hal_amd_xdna_executable_lookup_function_by_name(
    const iree_hal_amd_xdna_executable_t* executable, iree_string_view_t name,
    iree_hal_executable_function_t* out_function);

// Copies the native entry selected by |function| into |out_entry|. On failure,
// |out_entry| is unchanged.
iree_status_t iree_hal_amd_xdna_executable_query_entry(
    const iree_hal_amd_xdna_executable_t* executable,
    iree_hal_executable_function_t function,
    iree_hal_amd_xdna_executable_entry_t* out_entry);

// Copies one entry-relative binding contract into |out_binding|. On failure,
// |out_binding| is unchanged.
iree_status_t iree_hal_amd_xdna_executable_query_binding(
    const iree_hal_amd_xdna_executable_t* executable,
    iree_hal_executable_function_t function, iree_host_size_t binding_ordinal,
    iree_hal_amd_xdna_elf_binding_record_t* out_binding);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_EXPERIMENTAL_XDNA_EXECUTABLE_H_
