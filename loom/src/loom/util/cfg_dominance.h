// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_UTIL_CFG_DOMINANCE_H_
#define LOOM_UTIL_CFG_DOMINANCE_H_

#include "iree/base/api.h"
#include "iree/base/internal/arena.h"
#include "loom/util/cfg_graph.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LOOM_CFG_DOMINATOR_INVALID UINT16_MAX

// Dominator tree for one immutable CFG snapshot. Dense indices refer to the
// input graph's block table; all arrays are owned by the caller's arena.
typedef struct loom_cfg_dominance_t {
  // Immediate dominator per block; entry dominates itself and unreachable
  // blocks have LOOM_CFG_DOMINATOR_INVALID.
  uint16_t* immediate_dominators;
  // Packed inclusive dominator-tree preorder ranges per block: first in the
  // low 16 bits, last in the high 16 bits. Unreachable entries are UINT32_MAX.
  uint32_t* intervals;
  // Reachable blocks in dominator-tree preorder. Siblings retain block order.
  loom_cfg_block_index_span_t preorder;
  // False for a malformed input graph; arrays and preorder are then empty.
  bool available;
} loom_cfg_dominance_t;

// Computes dominance from graph-owned DFS facts in O(B + E log B) time and
// O(B) arena space using Lengauer-Tarjan link/eval with path compression.
// Does not revisit IR or nested regions. The graph must have been constructed
// by loom_cfg_graph_build; malformed graphs yield an unavailable result so
// verification can diagnose their structure. Empty graphs are available.
// Rebuild after changing the graph topology or block indices.
iree_status_t loom_cfg_dominance_build(const loom_cfg_graph_t* graph,
                                       iree_arena_allocator_t* arena,
                                       loom_cfg_dominance_t* out_dominance);

// Constant-time query for valid dense block indices in an available result.
// Unreachable blocks dominate only themselves.
static inline bool loom_cfg_dominance_block_dominates(
    const loom_cfg_dominance_t* dominance, uint16_t dominator,
    uint16_t dominated) {
  if (dominator == dominated) return true;
  uint32_t interval = dominance->intervals[dominator];
  uint32_t other = dominance->intervals[dominated];
  return interval != UINT32_MAX && other != UINT32_MAX &&
         (uint16_t)interval <= (uint16_t)other &&
         (uint16_t)other <= (uint16_t)(interval >> 16);
}

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_UTIL_CFG_DOMINANCE_H_
