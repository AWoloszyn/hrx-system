// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_EXPERIMENTAL_STREAMING_MEMORY_H_
#define IREE_EXPERIMENTAL_STREAMING_MEMORY_H_

#include "iree/base/api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct iree_hal_streaming_buffer_t iree_hal_streaming_buffer_t;
typedef struct iree_hal_streaming_context_t iree_hal_streaming_context_t;
typedef struct iree_hal_streaming_stream_t iree_hal_streaming_stream_t;

typedef uint64_t iree_hal_streaming_deviceptr_t;

// Allocates queue-visible host staging memory.
iree_status_t iree_hal_streaming_memory_allocate_host_staging(
    iree_hal_streaming_context_t* context, iree_host_size_t size,
    iree_hal_streaming_buffer_t** out_buffer);

// Enqueues a pitched H2D copy as one command-buffer transaction.
// Synchronization: stream-ordered.
iree_status_t iree_hal_streaming_memcpy_host_to_device_2d(
    iree_hal_streaming_context_t* context, iree_hal_streaming_deviceptr_t dst,
    iree_device_size_t dst_pitch, const void* src, iree_device_size_t src_pitch,
    iree_device_size_t width, iree_host_size_t height,
    iree_hal_streaming_stream_t* stream);

// Enqueues a pitched D2H copy through queue-visible staging. A stream-ordered
// host call scatters the packed staging rows into |dst| after the device copies
// complete.
// Synchronization: stream-ordered.
iree_status_t iree_hal_streaming_memcpy_device_to_host_2d(
    iree_hal_streaming_context_t* context, void* dst,
    iree_device_size_t dst_pitch, iree_hal_streaming_deviceptr_t src,
    iree_device_size_t src_pitch, iree_device_size_t width,
    iree_host_size_t height, iree_hal_streaming_stream_t* stream);

// Enqueues a pitched D2D copy as one command-buffer transaction.
// Synchronization: stream-ordered. If recording fails after accepting any
// rows, the accepted prefix completes before the original error is returned.
iree_status_t iree_hal_streaming_memcpy_device_to_device_2d(
    iree_hal_streaming_context_t* context, iree_hal_streaming_deviceptr_t dst,
    iree_device_size_t dst_pitch, iree_hal_streaming_deviceptr_t src,
    iree_device_size_t src_pitch, iree_device_size_t width,
    iree_host_size_t height, iree_hal_streaming_stream_t* stream);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // IREE_EXPERIMENTAL_STREAMING_MEMORY_H_
