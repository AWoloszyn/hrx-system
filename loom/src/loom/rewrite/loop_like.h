// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Generic arity-changing construction for LoopLike operations.

#ifndef LOOM_REWRITE_LOOP_LIKE_H_
#define LOOM_REWRITE_LOOP_LIKE_H_

#include "iree/base/api.h"
#include "iree/base/internal/arena.h"
#include "loom/ops/op_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

// State tuple used to build a replacement for an existing LoopLike operation.
typedef struct loom_loop_like_replacement_state_t {
  // New initial state values in recurrence order.
  loom_value_slice_t initial_values;

  // New recurring result types, one per initial value.
  //
  // Types may reference sibling replacement result IDs. Callers constructing
  // such a scheme reserve exactly |initial_values.count| builder results before
  // invoking loom_loop_like_build_replacement, matching generated loop builder
  // semantics.
  const loom_type_t* result_types;

  // Prefix offsets from each source state ordinal to its replacement range.
  // The array contains source_state_count + 1 monotonically increasing entries,
  // begins at zero, and ends at initial_values.count. A source state remains
  // one-to-one when offsets[i + 1] - offsets[i] is one. May be NULL when both
  // source and replacement state are empty.
  const uint16_t* source_state_offsets;
} loom_loop_like_replacement_state_t;

// Endpoints of a newly built LoopLike operation.
typedef struct loom_loop_like_replacement_t {
  // Newly built operation and its LoopLike interface metadata.
  loom_loop_like_t loop;

  // Entry block of the condition region, or NULL for counted loops.
  loom_block_t* condition_entry;

  // Complete condition-region state tuple, or an empty slice for counted loops.
  loom_value_slice_t condition_state;

  // Entry block of the primary body region.
  loom_block_t* body_entry;

  // Complete body-region state tuple, excluding a counted induction argument.
  loom_value_slice_t body_state;

  // Replacement result tuple.
  loom_value_slice_t results;
} loom_loop_like_replacement_t;

// Builds an empty-region replacement for |source| using |state|.
//
// The replacement has the same concrete operation kind. All non-state operand
// fields, per-instance operand segmentation, attributes, ownership ties,
// semantic and presentation flags, location, comments, and region/block
// presentation are preserved through the LoopLike interface. Result and entry
// argument types instantiate |state.result_types| as one simultaneous recurring
// tuple at each endpoint. One-to-one source results and region arguments retain
// their display names.
//
// The returned body and condition blocks are empty. The caller interprets the
// source terminators, populates replacement region operations, and completes
// replacement of source result uses. |scratch_arena| retains no output state
// and may be reset after this call.
iree_status_t loom_loop_like_build_replacement(
    loom_builder_t* builder, loom_loop_like_t source,
    const loom_loop_like_replacement_state_t* state,
    iree_arena_allocator_t* scratch_arena,
    loom_loop_like_replacement_t* out_replacement);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_REWRITE_LOOP_LIKE_H_
