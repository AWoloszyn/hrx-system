// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Postdominator tree over an immutable CFG snapshot.

#ifndef LOOM_UTIL_CFG_POSTDOMINANCE_H_
#define LOOM_UTIL_CFG_POSTDOMINANCE_H_

#include "iree/base/api.h"
#include "iree/base/internal/arena.h"
#include "loom/util/cfg_graph.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LOOM_CFG_POSTDOMINATOR_INVALID UINT32_MAX

// One real block or the synthetic exit in the postdominator tree. Indices are
// 32-bit because the synthetic exit follows the full 16-bit block index range.
typedef struct loom_cfg_postdominator_t {
  // Immediate postdominator, or LOOM_CFG_POSTDOMINATOR_INVALID for an
  // unreachable block. The synthetic exit is its own parent.
  uint32_t immediate_postdominator;
  // Tree depth, with zero at the synthetic exit and for unreachable blocks.
  uint32_t depth;
} loom_cfg_postdominator_t;

// Graph-only structural result. It contains no value facts or mutable solver
// state and remains valid until the graph topology or block indices change.
typedef struct loom_cfg_postdominance_t {
  // One node per graph block, followed by the synthetic exit.
  loom_cfg_postdominator_t* nodes;
  // Synthetic exit index, equal to the input graph's block count.
  uint32_t exit_node;
  // False for malformed graphs; nodes are then absent.
  bool available;
} loom_cfg_postdominance_t;

// Builds postdominators in O(B + E log B) time and O(B) retained arena space.
// The synthetic exit joins real exits and every reachable block that cannot
// reach a real exit. This gives nonterminating regions conservative control
// dependence without choosing an arbitrary loop node as their exit. Unreachable
// blocks remain outside the tree. Construction uses the graph's adjacency and
// reachability directly; no IR is traversed. Temporary DFS and semidominator
// storage is reclaimed before returning.
iree_status_t loom_cfg_postdominance_build(
    const loom_cfg_graph_t* graph, iree_arena_allocator_t* arena,
    loom_cfg_postdominance_t* out_postdominance);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_UTIL_CFG_POSTDOMINANCE_H_
