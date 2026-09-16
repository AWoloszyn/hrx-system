// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Stream reassembly utility for framed byte-stream receive paths.
//
// The frame accumulator handles the common pattern of reassembling
// length-prefixed frames from a byte stream, optimizing for the zero-copy
// case where complete frames arrive in a single buffer.
//
// ## Zero-copy optimization
//
// When one complete frame consumes the remaining bytes in a received buffer,
// the frame callback receives the lease directly and may retain it for deferred
// processing. Earlier frames packed into the same buffer receive a NULL lease:
// allowing one of them to move the shared lease would invalidate the parser's
// access to the later frames.
//
// When frames span multiple buffers, data is copied into the internal buffer
// and the callback receives a NULL lease (data must be consumed synchronously).
//
// ## Scope
//
// This utility is for CPU-accessible streams only. Device-memory payloads that
// cannot be inspected by the CPU require transport-specific framing.
//
// ## Protocol requirements
//
// The frame_length_fn callback MUST be able to determine the frame size from
// a compact header that fits within max_frame_size bytes. Protocols with
// arbitrarily-large headers before the length is known (e.g., HTTP/1.1 with
// unbounded headers) are not suitable for this accumulator.
//
// Suitable protocols include:
//   - Fixed-size length prefix (e.g., 4-byte little-endian length)
//   - Variable-length integer prefix (e.g., protobuf varint, QUIC varint)
//   - Fixed-size binary frame headers (e.g., HTTP/2's 9-byte header)
//
// If frame_length_fn cannot determine the frame size after the internal buffer
// fills, push_lease returns IREE_STATUS_RESOURCE_EXHAUSTED. This prevents
// denial-of-service attacks where malicious input stalls the receive pump.
//
// ## Usage
//
//   // Allocate storage for accumulator + internal buffer.
//   iree_host_size_t storage_size = 0;
//   IREE_RETURN_IF_ERROR(iree_net_frame_accumulator_calculate_storage_size(
//       max_frame_size, &storage_size));
//   void* storage = NULL;
//   IREE_RETURN_IF_ERROR(
//       iree_allocator_malloc(allocator, storage_size, &storage));
//   iree_net_frame_accumulator_t* accumulator =
//       (iree_net_frame_accumulator_t*)storage;
//
//   // Initialize.
//   IREE_RETURN_IF_ERROR(iree_net_frame_accumulator_initialize(
//       accumulator, max_frame_size, frame_length, on_frame_complete));
//
//   // Process received data (from io_uring completion, etc).
//   IREE_RETURN_IF_ERROR(iree_net_frame_accumulator_push_lease(
//       accumulator, &lease, bytes_received));
//
//   // Cleanup.
//   iree_net_frame_accumulator_deinitialize(accumulator);
//   iree_allocator_free(allocator, storage);

#ifndef IREE_NET_FRAME_ACCUMULATOR_H_
#define IREE_NET_FRAME_ACCUMULATOR_H_

#include "iree/async/buffer_pool.h"
#include "iree/base/api.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

//===----------------------------------------------------------------------===//
// Callback types
//===----------------------------------------------------------------------===//

// Resolves the total frame size (header + payload) from available stream data.
//
// The callback examines the available bytes, writes the complete frame size to
// |out_frame_size| when the header is available, and leaves it 0 when more
// bytes are needed. Malformed headers return a non-OK status. This callback is
// the protocol validation boundary for untrusted stream data.
//
// The returned frame size must include any header bytes that encode the length.
// For example, a 4-byte length-prefixed protocol returning a frame with 100
// bytes of payload should return 104 (4 header + 100 payload).
//
// IMPORTANT: This callback must be able to determine the frame size from a
// compact header. If it continues returning 0 after the accumulator's internal
// buffer fills (max_frame_size bytes), push_lease will fail with
// IREE_STATUS_RESOURCE_EXHAUSTED rather than loop indefinitely.
typedef iree_status_t(IREE_API_PTR* iree_net_frame_length_fn_t)(
    void* user_data, iree_const_byte_span_t available,
    iree_host_size_t* out_frame_size);

typedef struct iree_net_frame_length_callback_t {
  // Function that resolves a complete frame length from available bytes.
  iree_net_frame_length_fn_t fn;
  // Opaque value passed to |fn|.
  void* user_data;
} iree_net_frame_length_callback_t;

// Called when a complete frame is available for processing.
//
// |frame| contains the complete frame data (including any header bytes).
//
// |lease| is non-NULL only when this frame consumes the final bytes of a
// received buffer and may be moved for deferred processing. If |lease| is
// NULL, |frame| is borrowed and must be consumed synchronously before return.
// Borrowed frames may reference either the receive buffer or internal
// reassembly storage.
//
// ## Lease ownership
//
// The accumulator owns the lease and releases it after push_lease returns. A
// callback that needs to defer processing may move ownership by copying the
// lease and clearing the callback's lease value before returning. The moved
// lease must be released when processing completes.
//
// ## Error handling
//
// If the callback returns an error, frame processing stops and the error
// propagates to the push_lease caller. The receive stream is then terminal;
// reset or deinitialize the accumulator rather than continuing with later
// stream bytes.
typedef iree_status_t(IREE_API_PTR* iree_net_frame_complete_fn_t)(
    void* user_data, iree_const_byte_span_t frame,
    iree_async_buffer_lease_t* lease);

typedef struct iree_net_frame_complete_callback_t {
  // Function invoked for each complete frame.
  iree_net_frame_complete_fn_t fn;
  // Opaque value passed to |fn|.
  void* user_data;
} iree_net_frame_complete_callback_t;

//===----------------------------------------------------------------------===//
// Frame accumulator
//===----------------------------------------------------------------------===//

// Stream reassembly buffer with flexible array member for internal storage.
// Use iree_net_frame_accumulator_calculate_storage_size() to determine the
// required allocation size.
typedef struct iree_net_frame_accumulator_t {
  // Callback to determine frame length from partial data.
  iree_net_frame_length_callback_t frame_length;
  // Callback invoked when a complete frame is available.
  iree_net_frame_complete_callback_t on_frame_complete;
  // Capacity of the internal buffer (set from max_frame_size at init).
  iree_host_size_t buffer_capacity;
  // Bytes currently buffered (partial frame in progress).
  iree_host_size_t buffer_used;
  // Internal buffer for reassembling fragmented frames.
  // Sized by caller via storage_size().
  uint8_t buffer[];
} iree_net_frame_accumulator_t;

// Calculates the storage required for an accumulator and its frame buffer.
// Returns OUT_OF_RANGE if |max_frame_size| cannot be represented in one host
// allocation.
static inline iree_status_t iree_net_frame_accumulator_calculate_storage_size(
    iree_host_size_t max_frame_size, iree_host_size_t* out_storage_size) {
  IREE_ASSERT_ARGUMENT(out_storage_size);
  return IREE_STRUCT_LAYOUT(sizeof(iree_net_frame_accumulator_t),
                            out_storage_size,
                            IREE_STRUCT_FIELD_FAM(max_frame_size, uint8_t));
}

// Initializes an accumulator in pre-allocated memory.
//
// |accumulator| must point to at least the size returned by
// calculate_storage_size(max_frame_size).
// |max_frame_size| is the maximum frame size this accumulator can handle;
//   frames larger than this will return IREE_STATUS_RESOURCE_EXHAUSTED.
// |frame_length| determines frame boundaries from partial data.
// |on_frame_complete| is called for each complete frame.
IREE_API_EXPORT iree_status_t iree_net_frame_accumulator_initialize(
    iree_net_frame_accumulator_t* accumulator, iree_host_size_t max_frame_size,
    iree_net_frame_length_callback_t frame_length,
    iree_net_frame_complete_callback_t on_frame_complete);

// Deinitializes an accumulator. No deallocation occurs (caller owns memory).
IREE_API_EXPORT void iree_net_frame_accumulator_deinitialize(
    iree_net_frame_accumulator_t* accumulator);

// Discards any partial frame in progress and resets to initial state.
IREE_API_EXPORT void iree_net_frame_accumulator_reset(
    iree_net_frame_accumulator_t* accumulator);

// Processes received data from a buffer lease.
//
// Parses frames from the buffer and invokes on_frame_complete for each
// complete frame. When possible, passes the lease directly to the callback
// for zero-copy handling. Otherwise, copies data to the internal buffer.
//
// |lease| is the buffer lease from the receive operation. The accumulator
//   takes ownership and releases it on every return path unless a frame
//   callback moves it.
// |valid_bytes| is the number of valid bytes in the lease (may be less than
//   the lease's span length).
//
// Returns OK if all frames were processed successfully.
// Returns IREE_STATUS_OUT_OF_RANGE if |valid_bytes| exceeds the lease span.
// Returns IREE_STATUS_FAILED_PRECONDITION for CPU-inaccessible lease storage.
// Returns IREE_STATUS_RESOURCE_EXHAUSTED if a frame exceeds max_frame_size.
// Returns parser and frame callback errors without processing later bytes.
IREE_API_EXPORT iree_status_t iree_net_frame_accumulator_push_lease(
    iree_net_frame_accumulator_t* accumulator, iree_async_buffer_lease_t* lease,
    iree_host_size_t valid_bytes);

// Returns the number of bytes currently buffered (partial frame in progress).
static inline iree_host_size_t iree_net_frame_accumulator_buffered_bytes(
    const iree_net_frame_accumulator_t* accumulator) {
  return accumulator->buffer_used;
}

// Returns true if a partial frame is currently being accumulated.
static inline bool iree_net_frame_accumulator_has_partial_frame(
    const iree_net_frame_accumulator_t* accumulator) {
  return accumulator->buffer_used > 0;
}

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_NET_FRAME_ACCUMULATOR_H_
