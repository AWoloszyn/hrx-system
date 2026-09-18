// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Mutable execution distributions over retained CFG control dependence.
// Every strongly connected control component has one distribution: the minimum
// of its external inputs and selector labels, including labels on internal
// edges. The resulting DAG supports weakening and strengthening without a
// fixed-point iteration bound or rebuilding control structure.

#ifndef LOOM_UTIL_FACT_CONTROL_H_
#define LOOM_UTIL_FACT_CONTROL_H_

#include "loom/ir/facts.h"
#include "loom/util/cfg_control.h"

#ifdef __cplusplus
extern "C" {
#endif

// Distribution ordinals are private to this solver: varying, unknown,
// subgroup, workgroup, cluster. Unknown is distinct from uncomputed selectors,
// which the fact owner initializes optimistically to cluster-uniform.
#define LOOM_VALUE_FACT_CONTROL_DISTRIBUTION_COUNT 5

typedef struct loom_value_fact_control_input_t {
  // Previous input in the target component's distribution bucket, or INVALID.
  uint32_t previous;
  // Next input in the same bucket, or INVALID.
  uint32_t next;
  // Current distribution ordinal of this input.
  uint8_t distribution;
} loom_value_fact_control_input_t;

typedef struct loom_value_fact_control_component_t {
  // Input list heads for each distribution; empty buckets contain INVALID.
  uint32_t heads[LOOM_VALUE_FACT_CONTROL_DISTRIBUTION_COUNT];
  // Materialized diagnostic selector, valid while diagnostics_valid is true.
  loom_cfg_edge_index_t controller;
  // Current settled distribution ordinal.
  uint8_t distribution;
  // True while this component is in the topologically ordered worklist.
  bool queued;
} loom_value_fact_control_component_t;

typedef struct loom_value_fact_control_block_t {
  // Current selector distribution, shared by all this block's bindings.
  uint8_t selector_distribution;
  // Execution distribution before the first unpublished change.
  uint8_t old_distribution;
  // True while this block has an unpublished execution change.
  bool pending;
} loom_value_fact_control_block_t;

typedef struct loom_value_fact_control_t {
  // Immutable structure borrowed from the same CFG snapshot.
  const loom_cfg_control_t* structure;
  // Mutable bucket links for every structural minimum input.
  loom_value_fact_control_input_t* inputs;
  // Live component distributions and cached diagnostic provenance.
  loom_value_fact_control_component_t* components;
  // Per-block selector cache and first-old execution state.
  loom_value_fact_control_block_t* blocks;
  // Max heap of affected component ordinals, in producer-before-consumer order.
  uint32_t* worklist;
  // Number of queued components.
  uint32_t worklist_count;
  // Blocks whose execution changed since the last publication.
  uint16_t* pending_blocks;
  // Number of pending block records.
  uint32_t pending_count;
  // True when controller identities have been materialized for current inputs.
  bool diagnostics_valid;
} loom_value_fact_control_t;

// Allocates all update storage once, initially cluster-uniform. Updates never
// allocate. The owner supplies current selector facts before querying results.
iree_status_t loom_value_fact_control_initialize(
    const loom_cfg_control_t* structure, iree_arena_allocator_t* arena,
    loom_value_fact_control_t* out_control);

// Replaces one selector input and queues its indexed dependent components.
// Multiple replacements may be batched before settle, including mixed
// strengthening and weakening. Uncomputed selectors are supplied as uniform.
void loom_value_fact_control_set_selector(loom_value_fact_control_t* control,
                                          uint16_t block_index,
                                          loom_value_facts_t facts);

// Settles affected components once in topological order. Diagnostic identities
// do not propagate through unchanged scopes. Returns whether execution changed.
bool loom_value_fact_control_settle(loom_value_fact_control_t* control);

// Returns the block's settled execution distribution. Unavailable or
// unreachable control has unknown distribution; it cannot prove participation.
loom_value_facts_t loom_value_fact_control_execution(
    const loom_value_fact_control_t* control, uint16_t block_index);

// Materializes diagnostic selector identities from the settled minimum inputs
// in one dependency-ordered pass. Repeated calls without changed inputs are
// free. No IR, graph structure, or scope facts are recomputed. Call only when
// diagnostics are needed, after settle; ordinary scope queries need no prepare.
void loom_value_fact_control_prepare_diagnostics(
    loom_value_fact_control_t* control);

// Returns a prepared insufficient selector edge for a nonuniform block, or
// INVALID when control is unavailable or no selector limits participation.
// Requires prepare_diagnostics after the last selector update and settlement.
loom_cfg_edge_index_t loom_value_fact_control_controller(
    const loom_value_fact_control_t* control, uint16_t block_index);

// Removes one settled change, skipping transient reset/restore pairs whose
// final distribution equals the first-old distribution. Publication schedules
// dependents only after the enclosing value solve has finished.
bool loom_value_fact_control_take_changed_block(
    loom_value_fact_control_t* control, uint16_t* out_block_index);

#ifdef __cplusplus
}
#endif

#endif  // LOOM_UTIL_FACT_CONTROL_H_
