// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Native scheduling scope identity and control-flow balance.

#ifndef LOOM_OPS_LOW_SCHEDULE_SCOPE_H_
#define LOOM_OPS_LOW_SCHEDULE_SCOPE_H_

#include "iree/base/api.h"
#include "iree/base/internal/arena.h"
#include "loom/error/emitter.h"
#include "loom/ops/low/ops.h"
#include "loom/util/cfg_graph.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum loom_low_schedule_control_kind_e {
  // Operation does not change scheduling scope or phase.
  LOOM_LOW_SCHEDULE_CONTROL_NONE = 0,
  // Opens an independent scope nested in the current phase.
  LOOM_LOW_SCHEDULE_CONTROL_BEGIN = 1,
  // Joins one phase and opens its successor in the same scope.
  LOOM_LOW_SCHEDULE_CONTROL_PHASE = 2,
  // Joins the last phase and resumes the parent scope.
  LOOM_LOW_SCHEDULE_CONTROL_END = 3,
} loom_low_schedule_control_kind_t;

// Returns the authored scheduling control carried by an operation.
static inline loom_low_schedule_control_kind_t loom_low_schedule_control_kind(
    const loom_op_t* op) {
  switch (op->kind) {
    case LOOM_OP_LOW_SCHEDULE_BEGIN:
      return LOOM_LOW_SCHEDULE_CONTROL_BEGIN;
    case LOOM_OP_LOW_SCHEDULE_PHASE:
      return LOOM_LOW_SCHEDULE_CONTROL_PHASE;
    case LOOM_OP_LOW_SCHEDULE_END:
      return LOOM_LOW_SCHEDULE_CONTROL_END;
    default:
      return LOOM_LOW_SCHEDULE_CONTROL_NONE;
  }
}

// An explicit scope ID is the source-order index of its begin control plus one.
// A phased function's implicit scope follows all control IDs. Zero denotes the
// outer, unconstrained instruction stream. Unreachable CFG blocks
// have no active scope; their controls are retained but impose no dynamic
// order.
#define LOOM_LOW_SCHEDULE_SCOPE_UNREACHABLE UINT32_MAX

typedef struct loom_low_schedule_control_t {
  // Authored operation, borrowed from the immutable analyzed function.
  const loom_op_t* op;
  // Dense source-order node index across all function body blocks.
  uint32_t node_index;
  // Region block index containing this control.
  uint16_t block_index;
  // Scope or phase transition requested by the operation.
  loom_low_schedule_control_kind_t kind;
  // Active scope before the control; for a begin, this is its parent scope.
  uint32_t scope_before;
  // Active scope after the control.
  uint32_t scope_after;
} loom_low_schedule_control_t;

typedef struct loom_low_schedule_scopes_t {
  // Validated controls in source node order; borrowed from the builder arena.
  const loom_low_schedule_control_t* controls;
  // Number of controls.
  iree_host_size_t control_count;
  // Implicit function scope ID for schedule(phased), or zero when absent.
  uint32_t function_scope;
  // Active entry scope for each body block, or SCOPE_UNREACHABLE.
  const uint32_t* block_entry_scopes;
  // Number of reachable explicit scopes and the optional function scope.
  uint32_t scope_count;
  // Number of nesting errors emitted; failed results are not consumable.
  uint32_t error_count;
} loom_low_schedule_scopes_t;

typedef struct loom_low_schedule_scope_builder_t {
  // Source-ordered control records collected by the owning body walk.
  loom_low_schedule_control_t* controls;
  // Number of collected control records.
  iree_host_size_t control_count;
  // Allocated control record capacity; storage is allocated on the first use.
  iree_host_size_t control_capacity;
} loom_low_schedule_scope_builder_t;

// Appends one known control. Calls must follow region block and operation
// order. An all-zero builder is empty and owns no allocations.
iree_status_t loom_low_schedule_scope_builder_append(
    loom_low_schedule_scope_builder_t* builder, const loom_op_t* op,
    loom_low_schedule_control_kind_t kind, uint16_t block_index,
    uint32_t node_index, iree_arena_allocator_t* arena);

// Resolves scope identity and validates reachable CFG joins, backedges and
// exits. The caller supplies its retained CFG; a graph without adjacency is
// the usual single-block, non-CFG body. Generic verification handles malformed
// CFG structure before this analysis is used. No IR is changed.
// schedule(phased) supplies a function scope balanced implicitly at every exit.
//
// Work is linear in controls, blocks and CFG edges, independent of nesting
// depth. The builder, result and referenced IR must stay immutable and alive
// together while consumers use the result. A rewritten function needs a new
// analysis of that snapshot. Empty builders return without allocation.
iree_status_t loom_low_schedule_scope_builder_finish(
    loom_low_schedule_scope_builder_t* builder, const loom_cfg_graph_t* graph,
    loom_low_schedule_t schedule, iree_diagnostic_emitter_t emitter,
    iree_arena_allocator_t* arena, loom_low_schedule_scopes_t* out_scopes);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_OPS_LOW_SCHEDULE_SCOPE_H_
