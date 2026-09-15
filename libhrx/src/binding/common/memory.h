// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_EXPERIMENTAL_STREAMING_MEMORY_H_
#define IREE_EXPERIMENTAL_STREAMING_MEMORY_H_

#include "iree/base/api.h"
#include "iree/hal/api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct iree_hal_streaming_buffer_t iree_hal_streaming_buffer_t;
typedef struct iree_hal_streaming_context_t iree_hal_streaming_context_t;
typedef struct iree_hal_streaming_retained_buffer_ref_t
    iree_hal_streaming_retained_buffer_ref_t;

typedef struct iree_hal_streaming_memory_range_request_t {
  // First device or host address in the requested range.
  uint64_t address;
  // Requested range length in bytes.
  iree_device_size_t length;
} iree_hal_streaming_memory_range_request_t;

typedef struct iree_hal_streaming_memory_range_match_t {
  // Index into the returned retained references.
  iree_host_size_t ref_index;
  // Byte offset of the request into its retained reference.
  iree_device_size_t offset;
} iree_hal_streaming_memory_range_match_t;

// Returns the HAL buffer representing |buffer| in |execution_context|.
// Cross-context device-local imports are admitted only when the caller has
// already established peer access. The returned buffer is borrowed from the
// allocation and remains valid while the allocation remains live.
iree_status_t iree_hal_streaming_memory_buffer_for_context(
    iree_hal_streaming_context_t* execution_context,
    iree_hal_streaming_buffer_t* buffer, bool allow_peer_device_allocation,
    iree_hal_buffer_t** out_buffer);

// Resolves every requested range and retains each unique allocation once.
// Cross-context device allocations require enabled peer access. On success,
// each match names one initialized reference in |out_refs|. The function
// releases all partially initialized references before returning an error.
iree_status_t iree_hal_streaming_memory_lookup_ranges_retain_for_context(
    iree_hal_streaming_context_t* execution_context,
    iree_host_size_t request_count,
    const iree_hal_streaming_memory_range_request_t* requests,
    iree_host_size_t ref_capacity,
    iree_hal_streaming_retained_buffer_ref_t* out_refs,
    iree_host_size_t* out_ref_count,
    iree_hal_streaming_memory_range_match_t* out_matches);

// Allocates queue-visible host staging memory.
iree_status_t iree_hal_streaming_memory_allocate_host_staging(
    iree_hal_streaming_context_t* context, iree_host_size_t size,
    iree_hal_streaming_buffer_t** out_buffer);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // IREE_EXPERIMENTAL_STREAMING_MEMORY_H_
