// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Shared storage for independent bidirectional message endpoints.
//
// Each endpoint direction has a free-index stack, a consumed-byte receipt, a
// descriptor queue, and fixed-size payload slots. Returning a payload slot is
// independent of consuming its descriptor: a receiver can retain old slots
// while newer descriptors progress. Only the sending poll owner pops indices;
// receiving application threads may return uniquely owned indices concurrently.
//
// Geometry is exchanged once during bootstrap. The mapping contains no pointers
// or redundant offset table. Both processes derive the same layout, with
// separate cache lines for independently written state. Shared atomic fields
// require little-endian hosts and the current IREE atomic/MPSC queue ABI.

#ifndef IREE_NET_CARRIER_SHM_REGION_H_
#define IREE_NET_CARRIER_SHM_REGION_H_

#include "iree/base/api.h"
#include "iree/base/internal/atomic_freelist.h"
#include "iree/base/internal/mpsc_queue.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Alignment of independent shared state and each payload slot.
#define IREE_NET_SHM_REGION_ALIGNMENT 64u

// Resource bounds for one connection, supplied by its listener.
typedef struct iree_net_shm_region_options_t {
  // Number of independent bidirectional endpoint slots.
  uint32_t endpoint_count;
  // Payload slots per endpoint direction, in [1, 65534].
  uint32_t slot_count;
  // Usable bytes in each slot. Messages may span any number of slots.
  uint32_t slot_capacity;
} iree_net_shm_region_options_t;

// Process-local geometry derived from validated region options.
typedef struct iree_net_shm_region_layout_t {
  // Immutable dimensions used to derive this layout.
  iree_net_shm_region_options_t options;
  // Power-of-two data capacity of each MPSC descriptor queue, in bytes.
  uint32_t descriptor_capacity;
  // Number of direction views required by initialize/open (two per endpoint).
  iree_host_size_t direction_count;
  // Offset of the free-index links within each direction.
  iree_host_size_t links_offset;
  // Offset of the MPSC descriptor queue within each direction.
  iree_host_size_t descriptors_offset;
  // Offset of the first payload slot within each direction.
  iree_host_size_t payload_offset;
  // Aligned distance between successive payload slots, in bytes.
  iree_host_size_t slot_stride;
  // Aligned distance between successive directions, in bytes.
  iree_host_size_t direction_stride;
  // Mapping bytes used by the layout, before native page rounding.
  iree_host_size_t total_size;
} iree_net_shm_region_layout_t;

// One published payload chunk. All fields are little-endian native integers.
typedef struct iree_net_shm_descriptor_t {
  // Index of the exclusively owned payload slot in this direction.
  uint32_t slot;
  // Nonzero initialized bytes in the slot, at most slot_capacity.
  uint32_t length;
  // Cumulative end byte position, acknowledged after descriptor consumption.
  uint64_t end_position;
} iree_net_shm_descriptor_t;
static_assert(sizeof(iree_net_shm_descriptor_t) == 16,
              "SHM descriptor wire size must be stable");

// Borrowed view of one direction in an initialized/imported mapping.
typedef struct iree_net_shm_direction_t {
  // Shared free-index stack; the sender is the only popper.
  iree_atomic_freelist_t* free_slots;
  // Shared next-index links, one atomic entry per payload slot.
  iree_atomic_freelist_slot_t* links;
  // Receiver-published cumulative consumed byte position.
  iree_atomic_uint64_t* consumed_position;
  // Shared descriptor queue with one polling producer and consumer.
  iree_mpsc_queue_t descriptors;
  // First payload slot; successive slots are layout.slot_stride bytes apart.
  uint8_t* payload;
} iree_net_shm_direction_t;

// Validates dimensions and derives all offsets using checked host arithmetic.
// This does not allocate or map storage. |out_layout| is zero on failure.
iree_status_t iree_net_shm_region_calculate_layout(
    iree_net_shm_region_options_t options,
    iree_net_shm_region_layout_t* out_layout);

// Initializes metadata in a new mapping and fills |layout->direction_count|
// borrowed views in |out_directions|. Payload bytes are not touched.
// |layout| must come from calculate_layout; storage must be 64-byte aligned and
// contain at least layout.total_size bytes. No peer may access it until this
// call completes successfully and bootstrap publishes the mapping.
iree_status_t iree_net_shm_region_initialize(
    const iree_net_shm_region_layout_t* layout, iree_byte_span_t storage,
    iree_net_shm_direction_t* out_directions);

// Opens an initialized mapping with the already validated bootstrap geometry.
// Validates each descriptor queue without resetting shared state. Output views
// borrow storage; on failure they are incomplete and must not be used.
iree_status_t iree_net_shm_region_open(
    const iree_net_shm_region_layout_t* layout, iree_byte_span_t storage,
    iree_net_shm_direction_t* out_directions);

// Returns a borrowed connection-level notification epoch. |side| is 0 for the
// server's polling owner and 1 for the client's. The region must have been
// successfully initialized/opened; epochs and directions share its lifetime.
static inline iree_atomic_int32_t* iree_net_shm_region_epoch(void* storage,
                                                             uint32_t side) {
  return (iree_atomic_int32_t*)((uint8_t*)storage +
                                side * IREE_NET_SHM_REGION_ALIGNMENT);
}

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_NET_CARRIER_SHM_REGION_H_
