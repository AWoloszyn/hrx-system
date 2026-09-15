// Copyright 2026 The HRX Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef HRX_BINDING_HIP_STAGED_COPY_H_
#define HRX_BINDING_HIP_STAGED_COPY_H_

#include "iree/hal/api.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

typedef struct iree_hal_streaming_context_t iree_hal_streaming_context_t;
typedef struct iree_hal_streaming_stream_t iree_hal_streaming_stream_t;

// One endpoint of a staged copy. Device endpoints set |context| and |buffer|;
// host endpoints set |host_pointer|. |offset| applies only to device endpoints.
typedef struct iree_hip_staged_copy_endpoint_t {
  // Context owning |buffer|, or NULL for a host endpoint.
  iree_hal_streaming_context_t* context;
  // Device buffer borrowed for the duration of the call.
  iree_hal_buffer_t* buffer;
  // Byte offset of the first copied element in |buffer|.
  iree_device_size_t offset;
  // First copied byte of a host endpoint, or NULL for a device endpoint.
  void* host_pointer;
  // Distance in bytes between adjacent rows.
  iree_device_size_t row_pitch;
  // Distance in bytes between adjacent depth slices.
  iree_device_size_t slice_pitch;
} iree_hip_staged_copy_endpoint_t;

// Copies a 3D region whose device endpoints may belong to different contexts.
// With |stream|, the calling thread waits for prior stream work, performs the
// transfer, and publishes completion back to that stream. Without |stream|, it
// performs only the transfer. Host pointers are borrowed for the call.
iree_status_t iree_hip_staged_copy_3d(
    iree_hal_streaming_stream_t* stream,
    const iree_hip_staged_copy_endpoint_t* destination,
    const iree_hip_staged_copy_endpoint_t* source, iree_device_size_t width,
    iree_host_size_t height, iree_host_size_t depth);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // HRX_BINDING_HIP_STAGED_COPY_H_
