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

// Immutable control-flow structure for one populated fact scope.
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

// Returns the immutable region structure cached in the fact scope, constructing
// it on first use. It remains valid until the table scope is cleared.
iree_status_t loom_value_fact_table_get_or_build_cfg_region(
    loom_value_fact_table_t* table, const loom_module_t* module,
    const loom_region_t* region,
    const loom_value_fact_cfg_region_t** out_region);

#ifdef __cplusplus
}
#endif

#endif  // LOOM_UTIL_FACT_CFG_H_
