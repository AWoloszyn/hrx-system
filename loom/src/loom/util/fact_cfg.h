// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// CFG forwarding components retained with a value-fact scope. Mutually
// forwarded arguments share the same possible incoming values, so their facts
// are joined as one component before widening. Arithmetic producers remain
// external inputs to the component and retain normal iterative inference.

#ifndef LOOM_UTIL_FACT_CFG_H_
#define LOOM_UTIL_FACT_CFG_H_

#include "loom/analysis/scc.h"
#include "loom/util/cfg_graph.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_value_fact_table_t loom_value_fact_table_t;

// One argument participating in the region's forwarding graph.
typedef struct loom_value_fact_cfg_argument_t {
  // SSA value defined by the argument.
  loom_value_id_t value_id;
  // Owning block's dense CFG index.
  uint16_t block_index;
  // Position in the owning block's argument list.
  uint16_t argument_index;
} loom_value_fact_cfg_argument_t;

// Retained control-flow structure and cyclic-summary state for one region.
// Structural edits replace the snapshot; semantic edits mark summaries dirty.
typedef struct loom_value_fact_cfg_region_t {
  // CFG edges and reachability owned by this analysis.
  loom_cfg_graph_t graph;
  // First forwarding node for each block, plus one terminal count entry.
  iree_host_size_t* argument_offsets;
  // Forwarding nodes for reachable non-entry arguments.
  loom_value_fact_cfg_argument_t* arguments;
  // Number of forwarding nodes.
  iree_host_size_t argument_count;
  // Components of the direct argument-to-argument forwarding relation.
  loom_scc_list_t components;
  // Component index for each forwarding node.
  iree_host_size_t* argument_components;
  // Control-flow components bound the values that must restart together when
  // an edit changes a cyclic dataflow equation.
  struct {
    // Member spans grouped by graph-owned reachable component ordinal.
    loom_scc_list_t components;
    // Existing payload terminator used to schedule each cyclic summary.
    loom_op_t** anchors;
    // True when semantic edits or input changes require a cyclic summary.
    bool* dirty;
  } control_flow;
} loom_value_fact_cfg_region_t;

// Constructs CFG and forwarding structure once. Acyclic regions need no
// forwarding components; ordinary block argument inference handles them.
iree_status_t loom_value_fact_cfg_region_initialize(
    const loom_module_t* module, const loom_region_t* region,
    iree_arena_allocator_t* arena, loom_value_fact_cfg_region_t* out_region);

// Within a region with forwarding components, returns the node for a value,
// or IREE_HOST_SIZE_MAX for an operation result, entry argument, or value
// outside the reachable region.
iree_host_size_t loom_value_fact_cfg_region_argument_index(
    const loom_value_fact_cfg_region_t* region, loom_value_id_t value_id);

// Returns the retained structural snapshot without constructing one. A caller
// comparing snapshots across an edit uses this before publishing new structure.
const loom_value_fact_cfg_region_t* loom_value_fact_table_lookup_cfg_region(
    const loom_value_fact_table_t* table, const loom_region_t* region);

// Returns the region structure cached in the fact scope, constructing it on
// first use. It remains valid until the scope is cleared or the region's
// structure is replaced.
iree_status_t loom_value_fact_table_get_or_build_cfg_region(
    loom_value_fact_table_t* table, const loom_module_t* module,
    const loom_region_t* region,
    const loom_value_fact_cfg_region_t** out_region);

// Publishes caller-owned structure for a region after a completed CFG edit.
// The caller keeps the structure alive until replacing it again or forgetting
// it. This changes only structural analysis; existing value facts are retained.
iree_status_t loom_value_fact_table_set_cfg_region(
    loom_value_fact_table_t* table, const loom_region_t* region,
    const loom_value_fact_cfg_region_t* structure);

// Withdraws a region's structure before its storage or CFG becomes invalid.
// A subsequent structural query rebuilds it from the current IR.
void loom_value_fact_table_forget_cfg_region(loom_value_fact_table_t* table,
                                             const loom_region_t* region);

// Receives each value whose facts changed, including block arguments and
// results recomputed in a cyclic component. The callback schedules dependents.
typedef iree_status_t (*loom_value_fact_cfg_changed_fn_t)(
    void* user_data, loom_value_id_t value_id);

// Recomputes the joins defined by an acyclic block using current incoming
// value facts. Cyclic blocks use recompute_cfg_component so obsolete feedback
// cannot prevent narrowing after an edit. The caller propagates changed facts
// through users before querying the updated fixed point.
iree_status_t loom_value_fact_table_update_cfg_block_args(
    loom_value_fact_table_t* table, const loom_module_t* module,
    const loom_value_fact_cfg_region_t* region, uint16_t block_index,
    loom_value_fact_cfg_changed_fn_t on_changed, void* user_data);

// Restarts one cyclic control-flow component from its unchanged external
// inputs, then reports values whose converged facts differ from the old facts.
// Storage for the solve is temporary; extension payloads stay in the table.
iree_status_t loom_value_fact_table_recompute_cfg_component(
    loom_value_fact_table_t* table, const loom_module_t* module,
    const loom_value_fact_cfg_region_t* region, const loom_scc_t* component,
    iree_arena_allocator_t* scratch_arena,
    loom_value_fact_cfg_changed_fn_t on_changed, void* user_data);

#ifdef __cplusplus
}
#endif

#endif  // LOOM_UTIL_FACT_CFG_H_
