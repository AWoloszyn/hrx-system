// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/util/cfg_dominance.h"

#include <string.h>

// The simple Lengauer-Tarjan algorithm links the DFS forest in reverse preorder
// and compresses ancestor paths while retaining their minimum semidominator.
// Unlike iterative dominator-chain intersection, it bounds work on arbitrary
// (including irreducible) CFGs without a per-block dominance bitmap.
typedef struct loom_cfg_semidominator_t {
  // DFS preorder of this block's semidominator.
  uint16_t preorder;
  // Linked ancestor in the path-compressed forest, or UINT16_MAX at a root.
  uint16_t ancestor;
  // Block with the minimum semidominator on the compressed ancestor path.
  uint16_t label;
  // First block awaiting dominator resolution at this semidominator.
  uint16_t bucket_head;
  // Next block in the bucket containing this block.
  uint16_t bucket_next;
} loom_cfg_semidominator_t;

static uint16_t loom_cfg_dominance_evaluate(loom_cfg_semidominator_t* records,
                                            uint16_t block_index,
                                            uint16_t* stack) {
  iree_host_size_t stack_count = 0;
  uint16_t cursor = block_index;
  while (records[cursor].ancestor != UINT16_MAX &&
         records[records[cursor].ancestor].ancestor != UINT16_MAX) {
    stack[stack_count++] = cursor;
    cursor = records[cursor].ancestor;
  }
  while (stack_count > 0) {
    loom_cfg_semidominator_t* record = &records[stack[--stack_count]];
    const loom_cfg_semidominator_t* ancestor = &records[record->ancestor];
    if (records[ancestor->label].preorder < records[record->label].preorder) {
      record->label = ancestor->label;
    }
    record->ancestor = ancestor->ancestor;
  }
  return records[block_index].label;
}

// Reuses the finished link/eval records for the dominator child lists and the
// compression stack for iterative tree traversal. The DFS order becomes the
// retained dominator preorder; no second graph traversal or stack is needed.
static void loom_cfg_dominance_number_tree(iree_host_size_t block_count,
                                           loom_cfg_semidominator_t* records,
                                           uint16_t* stack, uint16_t* order,
                                           loom_cfg_dominance_t* dominance) {
  for (iree_host_size_t i = 0; i < block_count; ++i) {
    records[i].bucket_head = UINT16_MAX;
  }
  // Prepending in reverse block order preserves source order among siblings.
  for (iree_host_size_t i = block_count; i > 1; --i) {
    uint16_t block_index = (uint16_t)(i - 1);
    uint16_t parent = dominance->immediate_dominators[block_index];
    if (parent == LOOM_CFG_DOMINATOR_INVALID) {
      continue;
    }
    records[block_index].bucket_next = records[parent].bucket_head;
    records[parent].bucket_head = block_index;
  }
  iree_host_size_t stack_count = 1;
  uint32_t preorder = 0;
  stack[0] = 0;
  order[preorder] = 0;
  dominance->intervals[0] = preorder++;
  while (stack_count > 0) {
    uint16_t block_index = stack[stack_count - 1];
    uint16_t child = records[block_index].bucket_head;
    if (child != UINT16_MAX) {
      records[block_index].bucket_head = records[child].bucket_next;
      order[preorder] = child;
      dominance->intervals[child] = preorder++;
      stack[stack_count++] = child;
    } else {
      dominance->intervals[block_index] |= (preorder - 1) << 16;
      --stack_count;
    }
  }
  dominance->preorder =
      (loom_cfg_block_index_span_t){.values = order, .count = preorder};
}

// A unique entry predecessor must be the immediate dominator: it has a direct
// edge into the block. Any other reachable predecessor must be dominated by
// the block (a backedge), or that direct alternative can be bypassed. Reuse the
// finished DFS stack to retain this O(B+E) classification without allocating.
static void loom_cfg_dominance_classify_entries(const loom_cfg_graph_t* graph,
                                                loom_cfg_dominance_t* dominance,
                                                uint16_t* entries) {
  dominance->entry_predecessors = entries;
  for (uint16_t block = 0; block < graph->block_count; ++block) {
    uint16_t entry = block ? dominance->immediate_dominators[block]
                           : LOOM_CFG_DOMINATOR_INVALID;
    if (entry != LOOM_CFG_DOMINATOR_INVALID) {
      const loom_cfg_block_index_span_t predecessors =
          loom_cfg_graph_predecessors(graph, block);
      for (iree_host_size_t i = 0; i < predecessors.count; ++i) {
        const uint16_t predecessor = predecessors.values[i];
        if (predecessor != entry && graph->blocks[predecessor].reachable &&
            !loom_cfg_dominance_block_dominates(dominance, block,
                                                predecessor)) {
          entry = LOOM_CFG_DOMINATOR_INVALID;
          break;
        }
      }
    }
    entries[block] = entry;
  }
}

iree_status_t loom_cfg_dominance_build(const loom_cfg_graph_t* graph,
                                       iree_arena_allocator_t* arena,
                                       loom_cfg_dominance_t* out_dominance) {
  memset(out_dominance, 0, sizeof(*out_dominance));
  if (graph->malformed) {
    return iree_ok_status();
  }
  const iree_host_size_t block_count = graph->block_count;
  if (block_count == 0) {
    out_dominance->available = true;
    return iree_ok_status();
  }
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, block_count, sizeof(*out_dominance->immediate_dominators),
      (void**)&out_dominance->immediate_dominators));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, block_count, sizeof(*out_dominance->intervals),
      (void**)&out_dominance->intervals));
  loom_cfg_semidominator_t* records = NULL;
  uint16_t* stack = NULL;
  uint16_t* order = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, block_count, sizeof(*records), (void**)&records));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, block_count, sizeof(*stack), (void**)&stack));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, block_count, sizeof(*order), (void**)&order));
  for (iree_host_size_t i = 0; i < block_count; ++i) {
    records[i] = (loom_cfg_semidominator_t){
        .preorder = graph->blocks[i].preorder,
        .ancestor = UINT16_MAX,
        .label = (uint16_t)i,
        .bucket_head = UINT16_MAX,
        .bucket_next = UINT16_MAX,
    };
    out_dominance->immediate_dominators[i] = LOOM_CFG_DOMINATOR_INVALID;
    out_dominance->intervals[i] = UINT32_MAX;
    if (graph->blocks[i].reachable) {
      order[graph->blocks[i].preorder] = (uint16_t)i;
    }
  }
  const iree_host_size_t reachable_count = graph->reverse_postorder.count;
  uint16_t* immediate_dominators = out_dominance->immediate_dominators;
  immediate_dominators[0] = 0;
  for (iree_host_size_t i = reachable_count; i > 1; --i) {
    uint16_t block_index = order[i - 1];
    loom_cfg_semidominator_t* record = &records[block_index];
    loom_cfg_block_index_span_t predecessors =
        loom_cfg_graph_predecessors(graph, block_index);
    for (iree_host_size_t p = 0; p < predecessors.count; ++p) {
      uint16_t predecessor = predecessors.values[p];
      if (!graph->blocks[predecessor].reachable) {
        continue;
      }
      uint16_t label = loom_cfg_dominance_evaluate(records, predecessor, stack);
      if (records[label].preorder < record->preorder) {
        record->preorder = records[label].preorder;
      }
    }
    uint16_t semidominator = order[record->preorder];
    record->bucket_next = records[semidominator].bucket_head;
    records[semidominator].bucket_head = block_index;
    uint16_t parent = graph->blocks[block_index].parent;
    record->ancestor = parent;
    uint16_t pending = records[parent].bucket_head;
    records[parent].bucket_head = UINT16_MAX;
    while (pending != UINT16_MAX) {
      uint16_t label = loom_cfg_dominance_evaluate(records, pending, stack);
      immediate_dominators[pending] =
          records[label].preorder < records[pending].preorder ? label : parent;
      pending = records[pending].bucket_next;
    }
  }
  // Resolve deferred idoms in DFS order, after their own idoms are final.
  for (iree_host_size_t i = 1; i < reachable_count; ++i) {
    uint16_t block_index = order[i];
    if (immediate_dominators[block_index] !=
        order[records[block_index].preorder]) {
      immediate_dominators[block_index] =
          immediate_dominators[immediate_dominators[block_index]];
    }
  }
  loom_cfg_dominance_number_tree(block_count, records, stack, order,
                                 out_dominance);
  loom_cfg_dominance_classify_entries(graph, out_dominance, stack);
  out_dominance->available = true;
  return iree_ok_status();
}
