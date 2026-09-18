// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Compressed, immutable control dependence for one CFG snapshot. A branch
// alternative controls the postdominator path from its target to its source's
// immediate postdominator, excluding that postdominator. Heavy paths and a
// shared segment tree represent these paths without expanding a potentially
// quadratic block-to-controller relation.

#ifndef LOOM_UTIL_CFG_CONTROL_H_
#define LOOM_UTIL_CFG_CONTROL_H_

#include "loom/util/cfg_postdominance.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LOOM_CFG_CONTROL_INVALID UINT32_MAX

typedef struct loom_cfg_control_block_t {
  // Segment-tree leaf for this block, or INVALID when unreachable.
  uint32_t node;
  // First binding selected by this block's terminator.
  uint32_t binding_start;
  // Number of consecutive bindings selected by this block's terminator.
  uint32_t binding_count;
  // Next real block in the same control component, or INVALID.
  uint32_t next_component_block;
} loom_cfg_control_block_t;

typedef struct loom_cfg_control_node_t {
  // Strongly connected control component containing this segment node.
  uint32_t component;
} loom_cfg_control_node_t;

typedef struct loom_cfg_control_binding_t {
  // CFG alternative whose selector and source execution govern this segment.
  loom_cfg_edge_index_t edge;
  // Segment-tree node covering part of the alternative's control path.
  uint32_t node;
  // Mutable input slot for the alternative's selector distribution.
  uint32_t selector_input;
} loom_cfg_control_binding_t;

typedef struct loom_cfg_control_input_t {
  // Component receiving this minimum-transfer input.
  uint32_t target_component;
  // Producing component, or INVALID for a direct selector input.
  uint32_t source_component;
  // CFG edge identifying a direct selector input, or INVALID for propagation.
  loom_cfg_edge_index_t edge;
  // Next outgoing input from source_component, or INVALID.
  uint32_t next_outgoing;
} loom_cfg_control_input_t;

typedef struct loom_cfg_control_component_t {
  // First input receiving this component's distribution, or INVALID.
  uint32_t outgoing_head;
  // First real block contained by this component, or INVALID.
  uint32_t block_head;
} loom_cfg_control_component_t;

typedef struct loom_cfg_control_t {
  // Borrowed CFG snapshot; its lifetime contains this result's lifetime.
  const loom_cfg_graph_t* graph;
  // Postdominator tree owning the control-path endpoints.
  loom_cfg_postdominance_t postdominance;
  // Per-real-block leaves and outgoing binding spans.
  loom_cfg_control_block_t* blocks;
  // Segment nodes: children of node N are 2N+1 and 2N+2 below leaf_base.
  loom_cfg_control_node_t* nodes;
  // First leaf node; leaves follow heavy-path order, not CFG block order.
  uint32_t leaf_base;
  // Number of segment nodes, or zero when the CFG has no control alternatives.
  uint32_t node_count;
  // Control-path segment bindings grouped by their CFG source block.
  loom_cfg_control_binding_t* bindings;
  // Number of retained bindings.
  uint32_t binding_count;
  // SCC minimum inputs: selector terms and edges between distinct components.
  loom_cfg_control_input_t* inputs;
  // Number of minimum inputs.
  uint32_t input_count;
  // Components in successor-before-predecessor order.
  loom_cfg_control_component_t* components;
  // Number of components.
  uint32_t component_count;
  // True for complete structure; malformed builder IR remains unavailable.
  bool available;
} loom_cfg_control_t;

// Builds O(B + E log^2 B) retained structure. Temporary heavy-path and SCC
// construction storage is released before returning. No selector facts are
// consumed: the same snapshot supports changing value facts and diagnostics.
iree_status_t loom_cfg_control_build(const loom_cfg_graph_t* graph,
                                     iree_arena_allocator_t* arena,
                                     loom_cfg_control_t* out_control);

#ifdef __cplusplus
}
#endif

#endif  // LOOM_UTIL_CFG_CONTROL_H_
