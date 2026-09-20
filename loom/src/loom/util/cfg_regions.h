// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Single-entry regions retained with an immutable CFG and dominator tree.
// Each block owns its dominance subtree. Its external edges either converge
// at one real block or have no single continuation. A return is an external
// exit, so a subtree containing a return never has a real continuation.

#ifndef LOOM_UTIL_CFG_REGIONS_H_
#define LOOM_UTIL_CFG_REGIONS_H_

#include "loom/util/cfg_dominance.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LOOM_CFG_REGION_CONTINUATION_NONE UINT16_MAX

typedef struct loom_cfg_region_t {
  // Unique external destination, or NONE for multiple exits, returns, closed
  // cycles, and unreachable entries.
  uint16_t continuation_index;
  // Number of edges leaving the subtree, plus one per return in the subtree.
  iree_host_size_t exit_count;
  // Every edge leaving this dominance subtree for its unique continuation.
  // Empty when continuation_index is NONE; ordered by source dominance
  // preorder.
  loom_cfg_edge_index_span_t exit_edges;
} loom_cfg_region_t;

typedef struct loom_cfg_regions_t {
  // One region per graph block, owned by the caller's arena.
  loom_cfg_region_t* blocks;
  // Reachable incoming edges grouped by target and source dominance preorder.
  const loom_cfg_edge_index_t* incoming_edges;
  // Target bucket starts, followed by a sentinel at graph.block_count.
  const iree_host_size_t* incoming_offsets;
} loom_cfg_regions_t;

// Builds unique destinations and exact exit spans from retained graph and
// dominance facts in O(B + E + B log E) time and O(B + E) space. Exit spans
// share a target-indexed edge table, without inclusive per-region edge lists.
// Scratch storage is released before returning. Malformed graphs have no rows.
iree_status_t loom_cfg_regions_build(const loom_cfg_graph_t* graph,
                                     const loom_cfg_dominance_t* dominance,
                                     iree_arena_allocator_t* arena,
                                     loom_cfg_regions_t* out_regions);

// Returns every edge from entry's dominance subtree to destination. This is
// an indexed O(log E) range query; destination may be inside or outside the
// subtree. Unreachable sources have no entries. The span borrows regions.
loom_cfg_edge_index_span_t loom_cfg_regions_edges_to(
    const loom_cfg_graph_t* graph, const loom_cfg_dominance_t* dominance,
    const loom_cfg_regions_t* regions, uint16_t entry, uint16_t destination);

#ifdef __cplusplus
}
#endif

#endif  // LOOM_UTIL_CFG_REGIONS_H_
