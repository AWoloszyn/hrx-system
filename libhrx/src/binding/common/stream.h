// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_EXPERIMENTAL_STREAMING_STREAM_H_
#define IREE_EXPERIMENTAL_STREAMING_STREAM_H_

#include "iree/hal/api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct iree_hal_streaming_stream_t iree_hal_streaming_stream_t;
typedef struct iree_hal_streaming_context_t iree_hal_streaming_context_t;

// Retains the stream's context for one operation. Returns false after context
// teardown has detached the stream. The caller releases |*out_context|.
bool iree_hal_streaming_stream_retain_context(
    iree_hal_streaming_stream_t* stream,
    iree_hal_streaming_context_t** out_context);

// Queries backend dispatch properties that are not represented by the generic
// HAL device specification. Called once for each device during initialization.
typedef iree_status_t(
    IREE_API_PTR* iree_hal_streaming_query_dispatch_properties_fn_t)(
    iree_hal_device_t* device, bool* out_supports_cooperative_dispatch);

// Enqueues a cooperative kernel dispatch on a backend queue that reserves all
// participating workgroups concurrently. A non-NULL |synchronization_buffer|
// identifies a multi-grid launch and is shared by |grid_count| devices.
typedef iree_status_t(
    IREE_API_PTR* iree_hal_streaming_queue_dispatch_cooperative_fn_t)(
    iree_hal_device_t* device, iree_hal_queue_affinity_t queue_affinity,
    const iree_hal_semaphore_list_t wait_semaphore_list,
    const iree_hal_semaphore_list_t signal_semaphore_list,
    iree_hal_executable_t* executable,
    iree_hal_executable_function_t export_ordinal,
    const iree_hal_dispatch_config_t config, iree_const_byte_span_t constants,
    const iree_hal_buffer_ref_list_t bindings,
    iree_hal_buffer_t* synchronization_buffer, uint32_t grid_ordinal,
    uint32_t grid_count, iree_hal_dispatch_flags_t flags);

// Optional backend operations required by APIs layered on streaming.
typedef struct iree_hal_streaming_backend_operations_t {
  // Cold-path dispatch property query, or NULL when no properties are exposed.
  iree_hal_streaming_query_dispatch_properties_fn_t query_dispatch_properties;
  // Cooperative queue dispatch operation, or NULL when unsupported.
  iree_hal_streaming_queue_dispatch_cooperative_fn_t queue_dispatch_cooperative;
} iree_hal_streaming_backend_operations_t;

// Orders future work on |stream| after work already enqueued on
// |source_stream|. Both streams are flushed on the calling thread, but the
// dependency itself is submitted to the device queue without waiting for
// completion. Both streams must belong to one context and must not be
// capturing.
iree_status_t iree_hal_streaming_stream_wait_stream(
    iree_hal_streaming_stream_t* stream,
    iree_hal_streaming_stream_t* source_stream);

// Orders future work on |stream| after all work already enqueued on |sources|
// with one queue barrier. Source streams are flushed before their timeline
// points are captured; |stream| is flushed before the barrier is appended. All
// streams must belong to one context and must not be capturing.
iree_status_t iree_hal_streaming_stream_wait_streams(
    iree_hal_streaming_stream_t* stream,
    iree_hal_streaming_stream_t* const* sources, iree_host_size_t source_count);

// Enqueues a HAL host call at the current stream timeline point.
// Synchronization: flushes pending stream commands before enqueueing.
iree_status_t iree_hal_streaming_queue_host_call(
    iree_hal_streaming_stream_t* stream, iree_hal_host_call_t call,
    const uint64_t args[4], iree_hal_host_call_flags_t flags);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // IREE_EXPERIMENTAL_STREAMING_STREAM_H_
