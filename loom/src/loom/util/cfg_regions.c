// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/util/cfg_regions.h"

typedef struct loom_cfg_region_targets_t {
  // Two distinct lowest target positions; UINT32_MAX fills absent entries.
  uint32_t lowest[2];
  // Two distinct highest target positions plus one; zero fills absent entries.
  uint32_t highest[2];
  // Tree difference of outgoing edges/returns and internally contained edges.
  int64_t exit_balance;
} loom_cfg_region_targets_t;

static void loom_cfg_region_targets_insert(loom_cfg_region_targets_t* targets,
                                           uint32_t position) {
  if (position < targets->lowest[0]) {
    targets->lowest[1] = targets->lowest[0];
    targets->lowest[0] = position;
  } else if (position != targets->lowest[0] && position < targets->lowest[1]) {
    targets->lowest[1] = position;
  }
  ++position;
  if (position > targets->highest[0]) {
    targets->highest[1] = targets->highest[0];
    targets->highest[0] = position;
  } else if (position != targets->highest[0] &&
             position > targets->highest[1]) {
    targets->highest[1] = position;
  }
}

static iree_host_size_t loom_cfg_region_edge_lower_bound(
    const loom_cfg_graph_t* graph, const loom_cfg_dominance_t* dominance,
    const loom_cfg_edge_index_t* edges, iree_host_size_t count,
    uint32_t position) {
  iree_host_size_t first = 0;
  while (count) {
    iree_host_size_t step = count / 2;
    uint16_t source = graph->edges[edges[first + step]].source_block_index;
    if ((uint16_t)dominance->intervals[source] < position) {
      first += step + 1;
      count -= step + 1;
    } else {
      count = step;
    }
  }
  return first;
}

loom_cfg_edge_index_span_t loom_cfg_regions_edges_to(
    const loom_cfg_graph_t* graph, const loom_cfg_dominance_t* dominance,
    const loom_cfg_regions_t* regions, uint16_t entry, uint16_t destination) {
  iree_host_size_t start = regions->incoming_offsets[destination];
  iree_host_size_t count = regions->incoming_offsets[destination + 1] - start;
  if (!count || dominance->intervals[entry] == UINT32_MAX) {
    return (loom_cfg_edge_index_span_t){0};
  }
  const loom_cfg_edge_index_t* incoming = regions->incoming_edges + start;
  uint32_t first = (uint16_t)dominance->intervals[entry];
  uint32_t end = (dominance->intervals[entry] >> 16) + 1;
  iree_host_size_t begin = loom_cfg_region_edge_lower_bound(
      graph, dominance, incoming, count, first);
  iree_host_size_t limit =
      loom_cfg_region_edge_lower_bound(graph, dominance, incoming, count, end);
  return (loom_cfg_edge_index_span_t){.values = incoming + begin,
                                      .count = limit - begin};
}

static iree_status_t loom_cfg_regions_build_impl(
    const loom_cfg_graph_t* graph, const loom_cfg_dominance_t* dominance,
    iree_arena_allocator_t* scratch_arena, iree_arena_allocator_t* arena,
    loom_cfg_regions_t* regions) {
  const iree_host_size_t block_count = graph->block_count;
  loom_cfg_region_targets_t* targets = NULL;
  iree_host_size_t* offsets = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      scratch_arena, block_count, sizeof(*targets), (void**)&targets));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, block_count + 1, sizeof(*offsets), (void**)&offsets));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, block_count, sizeof(*regions->blocks), (void**)&regions->blocks));
  loom_cfg_edge_index_t* edges = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, graph->edge_count, sizeof(*edges), (void**)&edges));
  regions->incoming_edges = edges;
  regions->incoming_offsets = offsets;
  for (iree_host_size_t i = 0; i < block_count; ++i) {
    targets[i] = (loom_cfg_region_targets_t){
        .lowest = {UINT32_MAX, UINT32_MAX},
    };
    regions->blocks[i] = (loom_cfg_region_t){
        .continuation_index = LOOM_CFG_REGION_CONTINUATION_NONE,
    };
    offsets[i] = 0;
  }
  for (iree_host_size_t i = 0; i < graph->edge_count; ++i) {
    const loom_cfg_edge_info_t* edge = &graph->edges[i];
    if (graph->blocks[edge->source_block_index].reachable) {
      ++offsets[edge->target_block_index];
      ++targets[edge->source_block_index].exit_balance;
      // Every path reaching a predecessor must pass the target's immediate
      // dominator, unless the edge returns from inside the target's subtree.
      // Thus a CFG edge's endpoint LCA is available without a tree walk.
      uint16_t ancestor =
          loom_cfg_dominance_block_dominates(
              dominance, edge->target_block_index, edge->source_block_index)
              ? edge->target_block_index
              : dominance->immediate_dominators[edge->target_block_index];
      --targets[ancestor].exit_balance;
    }
  }
  iree_host_size_t total = 0;
  for (iree_host_size_t i = 0; i < block_count; ++i) {
    total += offsets[i];
    offsets[i] = total;
  }
  offsets[block_count] = total;

  // Reverse preorder with descending bucket cursors leaves each target's
  // incoming edges ordered by source dominance preorder. The final cursors
  // are the immutable bucket starts used by the exit-span queries below.
  for (iree_host_size_t i = dominance->preorder.count; i > 0; --i) {
    uint16_t block = dominance->preorder.values[i - 1];
    loom_cfg_edge_index_span_t outgoing =
        loom_cfg_graph_successor_edges(graph, block);
    if (!outgoing.count) {
      loom_cfg_region_targets_insert(&targets[block], (uint32_t)block_count);
      ++targets[block].exit_balance;
    }
    for (iree_host_size_t j = outgoing.count; j > 0; --j) {
      loom_cfg_edge_index_t edge = outgoing.values[j - 1];
      uint16_t target = graph->edges[edge].target_block_index;
      edges[--offsets[target]] = edge;
      loom_cfg_region_targets_insert(&targets[block],
                                     (uint16_t)dominance->intervals[target]);
    }
    if (block) {
      loom_cfg_region_targets_t* parent =
          &targets[dominance->immediate_dominators[block]];
      parent->exit_balance += targets[block].exit_balance;
      for (iree_host_size_t j = 0; j < 2; ++j) {
        if (targets[block].lowest[j] != UINT32_MAX) {
          loom_cfg_region_targets_insert(parent, targets[block].lowest[j]);
        }
        if (targets[block].highest[j]) {
          loom_cfg_region_targets_insert(parent, targets[block].highest[j] - 1);
        }
      }
    }
  }

  for (iree_host_size_t i = 0; i < dominance->preorder.count; ++i) {
    uint16_t block = dominance->preorder.values[i];
    uint32_t first = (uint16_t)dominance->intervals[block];
    uint32_t end = (dominance->intervals[block] >> 16) + 1;
    const loom_cfg_region_targets_t* summary = &targets[block];
    regions->blocks[block].exit_count = (iree_host_size_t)summary->exit_balance;
    bool exits_before = summary->lowest[0] < first;
    bool exits_after = summary->highest[0] > end;
    if (exits_before == exits_after || summary->lowest[1] < first ||
        summary->highest[1] > end) {
      continue;
    }
    uint32_t position =
        exits_before ? summary->lowest[0] : summary->highest[0] - 1;
    if (position == block_count) {
      continue;
    }
    uint16_t continuation = dominance->preorder.values[position];
    regions->blocks[block].continuation_index = continuation;
    regions->blocks[block].exit_edges = loom_cfg_regions_edges_to(
        graph, dominance, regions, block, continuation);
  }
  return iree_ok_status();
}

iree_status_t loom_cfg_regions_build(const loom_cfg_graph_t* graph,
                                     const loom_cfg_dominance_t* dominance,
                                     iree_arena_allocator_t* arena,
                                     loom_cfg_regions_t* out_regions) {
  *out_regions = (loom_cfg_regions_t){0};
  if (!dominance->available || !graph->block_count) {
    return iree_ok_status();
  }
  iree_arena_allocator_t scratch_arena;
  iree_arena_initialize(arena->block_pool, &scratch_arena);
  iree_status_t status = loom_cfg_regions_build_impl(
      graph, dominance, &scratch_arena, arena, out_regions);
  iree_arena_deinitialize(&scratch_arena);
  return status;
}
