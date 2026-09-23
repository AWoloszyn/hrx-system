// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// LoopLike recurrence topology for composed boundary projections.

#ifndef LOOM_TRANSFORMS_BOUNDARY_PROJECTION_LOOP_H_
#define LOOM_TRANSFORMS_BOUNDARY_PROJECTION_LOOP_H_

#include "loom/transforms/boundary/projection_plan.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_boundary_projection_loop_state_t {
  // Original logical result defining the recurring type scheme.
  loom_value_id_t result_value_id;
  // Original condition-region entry argument, or INVALID for counted loops.
  loom_value_id_t condition_value_id;
  // Original body-region entry argument.
  loom_value_id_t body_value_id;
  // Candidate index for result_value_id, or IREE_HOST_SIZE_MAX when no rule
  // may claim this recurrence column.
  iree_host_size_t result_candidate;
  // Candidate index for condition_value_id, or IREE_HOST_SIZE_MAX.
  iree_host_size_t condition_candidate;
  // Candidate index for body_value_id, or IREE_HOST_SIZE_MAX.
  iree_host_size_t body_candidate;
  // Planned projection of the loop's initial value.
  loom_boundary_projection_source_t initial_source;
  // Planned projection forwarded by the condition terminator, when present.
  loom_boundary_projection_source_t condition_source;
  // Planned projection supplied by the body backedge.
  loom_boundary_projection_source_t backedge_source;
} loom_boundary_projection_loop_state_t;

struct loom_boundary_projection_loop_t {
  // Original operation and its LoopLike interface metadata.
  loom_loop_like_t loop;
  // Original condition-region terminator, or NULL for counted loops.
  loom_op_t* condition_terminator;
  // Original body-region terminator.
  loom_op_t* body_terminator;
  // Original recurrence columns in source order.
  loom_boundary_projection_loop_state_t* states;
  // Prefix offsets from source columns to final physical state ordinals.
  uint16_t* state_offsets;
  // Number of source recurrence columns.
  uint16_t state_count;
  // Number of final physical recurrence values.
  uint16_t final_state_count;
  // Whether at least one recurrence column remains selected.
  bool selected;
};

// Retains one structurally eligible LoopLike operation and provisional
// endpoint slots. Calls are expected in operation postorder.
iree_status_t loom_boundary_projection_collect_loop(
    loom_boundary_projection_plan_t* plan,
    loom_boundary_projection_function_t* function, loom_loop_like_t loop);

// Resolves retained endpoint identities after candidate sorting.
void loom_boundary_projection_index_loops(
    loom_boundary_projection_function_t* function);

// Selects recurrence schemas and plans all initial, condition, and backedge
// source coordinates after function-local rule preparation.
iree_status_t loom_boundary_projection_plan_loops(
    loom_boundary_projection_plan_t* plan,
    loom_boundary_projection_function_t* function);

// Rejects projected columns that cannot preserve the recurring type scheme.
void loom_boundary_projection_preflight_loops(
    loom_boundary_projection_plan_t* plan,
    loom_boundary_projection_function_t* function);

// Freezes final source-to-physical offsets after rejection propagation.
iree_status_t loom_boundary_projection_finalize_loops(
    loom_boundary_projection_plan_t* plan,
    loom_boundary_projection_function_t* function);

// Rebuilds retained selected loops in operation postorder.
iree_status_t loom_boundary_projection_apply_loops(
    loom_boundary_projection_plan_t* plan,
    loom_boundary_projection_function_t* function);

#ifdef __cplusplus
}
#endif

#endif  // LOOM_TRANSFORMS_BOUNDARY_PROJECTION_LOOP_H_
