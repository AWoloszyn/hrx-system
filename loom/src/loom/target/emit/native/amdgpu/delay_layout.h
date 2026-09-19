// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Exact native placement of AMDGPU dependency hints.

#ifndef LOOM_TARGET_EMIT_NATIVE_AMDGPU_DELAY_LAYOUT_H_
#define LOOM_TARGET_EMIT_NATIVE_AMDGPU_DELAY_LAYOUT_H_

#include "iree/base/api.h"
#include "iree/base/internal/arena.h"

#ifdef __cplusplus
extern "C" {
#endif

// Packet boundaries enclosing one hint and its two native consumers.
typedef struct loom_amdgpu_delay_layout_span_t {
  // Packet containing the retained hint; an island may precede this packet.
  uint32_t first_packet_index;
  // Packet containing the second consumer; interior islands would change SKIP.
  uint32_t last_packet_index;
} loom_amdgpu_delay_layout_span_t;

// Immutable encoding decisions indexed by the semantic wait-state plan.
typedef struct loom_amdgpu_delay_layout_t {
  // Immediate for each S_DELAY_ALU action, or zero when its selector is encoded
  // by an earlier hint. Entries for other actions are not consumed.
  const uint16_t* immediates;
  // Nonoverlapping paired spans in packet order.
  const loom_amdgpu_delay_layout_span_t* spans;
  // Number of entries in |spans|.
  iree_host_size_t span_count;
} loom_amdgpu_delay_layout_t;

// Mutable state used only during the native encoder's first sizing traversal.
typedef struct loom_amdgpu_delay_layout_builder_t {
  // Arena-owned decisions indexed by semantic wait-state ordinal.
  uint16_t* immediates;
  // Arena-owned paired spans, with capacity for half the wait-state count.
  loom_amdgpu_delay_layout_span_t* spans;
  // Number of paired spans recorded so far.
  iree_host_size_t span_count;
  // Wait-state ordinal of the unpaired hint, or IREE_HOST_SIZE_MAX.
  iree_host_size_t pending_state_index;
  // Native instruction ordinal immediately following the unpaired hint.
  uint64_t pending_consumer_index;
  // Packet containing the unpaired hint.
  uint32_t pending_packet_index;
} loom_amdgpu_delay_layout_builder_t;

// Allocates bounded storage for a function with |wait_state_count| actions.
iree_status_t loom_amdgpu_delay_layout_builder_initialize(
    iree_host_size_t wait_state_count, iree_arena_allocator_t* arena,
    loom_amdgpu_delay_layout_builder_t* out_builder);

// Records one semantic hint at its exact position in the native stream. Returns
// the immediate to size here, or zero when this hint is folded into its
// predecessor. Final emission consumes |immediates| after the traversal ends.
uint16_t loom_amdgpu_delay_layout_record(
    loom_amdgpu_delay_layout_builder_t* builder, iree_host_size_t state_index,
    uint32_t packet_index, uint64_t instruction_index, uint16_t immediate);

// Ends the pending pair at a control, block, or ambiguous instruction boundary.
static inline void loom_amdgpu_delay_layout_end_span(
    loom_amdgpu_delay_layout_builder_t* builder) {
  builder->pending_state_index = IREE_HOST_SIZE_MAX;
}

// Consumes paired spans in packet order and returns whether an island may be
// inserted before |packet_index|. The caller initializes |span_cursor| to zero.
bool loom_amdgpu_delay_layout_allows_island(
    const loom_amdgpu_delay_layout_t* layout, uint32_t packet_index,
    iree_host_size_t* span_cursor);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_EMIT_NATIVE_AMDGPU_DELAY_LAYOUT_H_
