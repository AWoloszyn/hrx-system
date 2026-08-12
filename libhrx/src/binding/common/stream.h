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
typedef struct iree_hal_streaming_event_t iree_hal_streaming_event_t;
typedef struct iree_hal_streaming_queue_scope_t
    iree_hal_streaming_queue_scope_t;

// Acquires a backend queue with immutable execution-unit configuration.
// Called synchronously by iree_hal_streaming_queue_scope_create; neither
// |user_data| nor |execution_unit_mask| may be retained by the callback. A
// successful callback returns a non-NULL lease and one exact queue affinity.
typedef iree_status_t(
    IREE_API_PTR* iree_hal_streaming_queue_scope_acquire_fn_t)(
    void* user_data, iree_hal_device_t* device,
    iree_hal_queue_affinity_t device_affinity,
    iree_host_size_t execution_unit_mask_bit_count,
    const uint32_t* execution_unit_mask, void** out_backend_queue,
    iree_hal_queue_affinity_t* out_queue_affinity);

// Releases a backend queue acquired for a streaming queue scope.
typedef void(IREE_API_PTR* iree_hal_streaming_queue_scope_release_fn_t)(
    void* backend_queue);

// Retains the stream's context for one operation. Returns false after context
// teardown has detached the stream. The caller releases |*out_context|.
bool iree_hal_streaming_stream_retain_context(
    iree_hal_streaming_stream_t* stream,
    iree_hal_streaming_context_t** out_context);

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

// Acquires a backend queue for |execution_unit_mask| and creates a virtual
// scope over it. Backends may share one immutable queue among scopes with the
// same configuration. A distinct mask may fail when backend capacity is
// exhausted.
iree_status_t iree_hal_streaming_queue_scope_create(
    iree_host_size_t device_ordinal,
    iree_host_size_t execution_unit_mask_bit_count,
    const uint32_t* execution_unit_mask,
    iree_hal_streaming_queue_scope_acquire_fn_t acquire,
    void* acquire_user_data,
    iree_hal_streaming_queue_scope_release_fn_t release,
    iree_allocator_t host_allocator,
    iree_hal_streaming_queue_scope_t** out_scope);

void iree_hal_streaming_queue_scope_retain(
    iree_hal_streaming_queue_scope_t* scope);
void iree_hal_streaming_queue_scope_release(
    iree_hal_streaming_queue_scope_t* scope);

// Prevents new streams from being attached to |scope|. Existing streams keep
// the backend queue lease live until their final release.
void iree_hal_streaming_queue_scope_detach(
    iree_hal_streaming_queue_scope_t* scope);

bool iree_hal_streaming_queue_scope_is_attached(
    const iree_hal_streaming_queue_scope_t* scope);

iree_hal_queue_affinity_t iree_hal_streaming_queue_scope_affinity(
    const iree_hal_streaming_queue_scope_t* scope);

iree_hal_streaming_context_t* iree_hal_streaming_queue_scope_context(
    const iree_hal_streaming_queue_scope_t* scope);

// Returns the number of immutable execution-unit mask bits stored by |scope|.
iree_host_size_t iree_hal_streaming_queue_scope_execution_unit_mask_bit_count(
    const iree_hal_streaming_queue_scope_t* scope);

// Copies the execution-unit mask from |scope| into the caller-provided words.
// Excess destination words are cleared and a short destination is truncated.
void iree_hal_streaming_queue_scope_copy_execution_unit_mask(
    const iree_hal_streaming_queue_scope_t* scope,
    iree_host_size_t execution_unit_mask_word_count,
    uint32_t* out_execution_unit_mask);

// Returns the queue scope retained by |stream|, or NULL for ordinary streams.
iree_hal_streaming_queue_scope_t* iree_hal_streaming_stream_queue_scope(
    const iree_hal_streaming_stream_t* stream);

// Returns the registry ordinal of the device owning |stream|.
iree_host_size_t iree_hal_streaming_stream_device_ordinal(
    const iree_hal_streaming_stream_t* stream);

// Creates a stream routed through the exclusive queue owned by |scope|.
iree_status_t iree_hal_streaming_stream_create_in_queue_scope(
    iree_hal_streaming_queue_scope_t* scope, uint64_t flags, int priority,
    iree_allocator_t host_allocator, iree_hal_streaming_stream_t** out_stream);

// Synchronizes all streams currently associated with |scope|.
iree_status_t iree_hal_streaming_queue_scope_synchronize(
    iree_hal_streaming_queue_scope_t* scope);

// Records |event| after work already enqueued on every stream in |scope|.
iree_status_t iree_hal_streaming_queue_scope_record_event(
    iree_hal_streaming_queue_scope_t* scope, iree_hal_streaming_event_t* event);

// Orders every current stream in |scope| after |event|.
iree_status_t iree_hal_streaming_queue_scope_wait_event(
    iree_hal_streaming_queue_scope_t* scope, iree_hal_streaming_event_t* event);

// Enqueues a HAL host call at the current stream timeline point.
// Synchronization: flushes pending stream commands before enqueueing.
iree_status_t iree_hal_streaming_queue_host_call(
    iree_hal_streaming_stream_t* stream, iree_hal_host_call_t call,
    const uint64_t args[4], iree_hal_host_call_flags_t flags);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // IREE_EXPERIMENTAL_STREAMING_STREAM_H_
