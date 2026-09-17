// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/util/cfg_postdominance.h"

#include <string.h>

#define LOOM_CFG_POSTDOMINANCE_CAN_REACH_EXIT 1u

// Lengauer-Tarjan construction state; none of this survives the build.
typedef struct loom_cfg_postdominance_node_t {
  // One-based reverse-CFG depth-first-search number, or zero when unvisited.
  uint32_t dfs_number;
  // Parent in the reverse-CFG depth-first-search tree.
  uint32_t dfs_parent;
  // Lengauer-Tarjan semidominator DFS number.
  uint32_t semidominator;
  // Best semidominator representative maintained by path compression.
  uint32_t label;
  // Lengauer-Tarjan union-forest ancestor.
  uint32_t ancestor;
  // Resolved immediate postdominator.
  uint32_t immediate_postdominator;
  // Head node in the semidominator bucket chain.
  uint32_t bucket_head;
  // Next node in the semidominator bucket chain.
  uint32_t bucket_next;
  // Successor cursor for the iterative reverse DFS.
  uint32_t scratch;
  // Resolved tree depth.
  uint32_t postdominator_depth;
  // Exit reachability from LOOM_CFG_POSTDOMINANCE_CAN_REACH_EXIT.
  uint32_t flags;
} loom_cfg_postdominance_node_t;

typedef struct loom_cfg_postdominance_region_t {
  // Immutable graph supplying adjacency and entry reachability.
  const loom_cfg_graph_t* graph;
  // Scratch node state, including the synthetic exit.
  loom_cfg_postdominance_node_t* nodes;
  // Synthetic exit index after all graph blocks.
  uint32_t exit_node;
} loom_cfg_postdominance_region_t;

static bool loom_cfg_postdominance_block_is_synthetic_exit(
    const loom_cfg_postdominance_region_t* summary, uint32_t block_index) {
  const loom_cfg_postdominance_node_t* node = &summary->nodes[block_index];
  const loom_cfg_block_index_span_t successors =
      loom_cfg_graph_successors(summary->graph, (uint16_t)block_index);
  return successors.count == 0 ||
         !iree_any_bit_set(node->flags, LOOM_CFG_POSTDOMINANCE_CAN_REACH_EXIT);
}

static void loom_cfg_postdominance_mark_exit_reachability(
    loom_cfg_postdominance_region_t* summary, uint32_t* stack) {
  iree_host_size_t stack_count = 0;
  for (uint32_t block_index = 0; block_index < summary->exit_node;
       ++block_index) {
    if (!loom_cfg_graph_block_is_reachable(summary->graph,
                                           (uint16_t)block_index) ||
        loom_cfg_graph_successors(summary->graph, (uint16_t)block_index)
                .count != 0) {
      continue;
    }
    summary->nodes[block_index].flags |= LOOM_CFG_POSTDOMINANCE_CAN_REACH_EXIT;
    stack[stack_count++] = block_index;
  }
  while (stack_count > 0) {
    const uint32_t block_index = stack[--stack_count];
    const loom_cfg_block_index_span_t predecessors =
        loom_cfg_graph_predecessors(summary->graph, (uint16_t)block_index);
    for (iree_host_size_t i = 0; i < predecessors.count; ++i) {
      const uint32_t predecessor_index = predecessors.values[i];
      loom_cfg_postdominance_node_t* predecessor =
          &summary->nodes[predecessor_index];
      if (!loom_cfg_graph_block_is_reachable(summary->graph,
                                             (uint16_t)predecessor_index) ||
          iree_any_bit_set(predecessor->flags,
                           LOOM_CFG_POSTDOMINANCE_CAN_REACH_EXIT)) {
        continue;
      }
      predecessor->flags |= LOOM_CFG_POSTDOMINANCE_CAN_REACH_EXIT;
      stack[stack_count++] = predecessor_index;
    }
  }
}

static bool loom_cfg_postdominance_next_reverse_successor(
    loom_cfg_postdominance_region_t* summary, uint32_t node_index,
    uint32_t* out_successor_index) {
  loom_cfg_postdominance_node_t* node = &summary->nodes[node_index];
  if (node_index == summary->exit_node) {
    while (node->scratch < summary->exit_node) {
      const uint32_t candidate_index = node->scratch++;
      if (loom_cfg_graph_block_is_reachable(summary->graph,
                                            (uint16_t)candidate_index) &&
          loom_cfg_postdominance_block_is_synthetic_exit(summary,
                                                         candidate_index)) {
        *out_successor_index = candidate_index;
        return true;
      }
    }
    return false;
  }

  const loom_cfg_block_index_span_t predecessors =
      loom_cfg_graph_predecessors(summary->graph, (uint16_t)node_index);
  while (node->scratch < predecessors.count) {
    const uint32_t candidate_index = predecessors.values[node->scratch++];
    if (loom_cfg_graph_block_is_reachable(summary->graph,
                                          (uint16_t)candidate_index)) {
      *out_successor_index = candidate_index;
      return true;
    }
  }
  return false;
}

static uint32_t loom_cfg_postdominance_build_reverse_dfs(
    loom_cfg_postdominance_region_t* summary, uint32_t* vertex_by_dfs,
    uint32_t* stack) {
  uint32_t dfs_count = 1;
  loom_cfg_postdominance_node_t* exit = &summary->nodes[summary->exit_node];
  exit->dfs_number = dfs_count;
  exit->dfs_parent = LOOM_CFG_POSTDOMINATOR_INVALID;
  vertex_by_dfs[dfs_count] = summary->exit_node;

  iree_host_size_t stack_count = 0;
  stack[stack_count++] = summary->exit_node;
  while (stack_count > 0) {
    const uint32_t node_index = stack[stack_count - 1];
    uint32_t successor_index = LOOM_CFG_POSTDOMINATOR_INVALID;
    if (!loom_cfg_postdominance_next_reverse_successor(summary, node_index,
                                                       &successor_index)) {
      --stack_count;
      continue;
    }
    loom_cfg_postdominance_node_t* successor = &summary->nodes[successor_index];
    if (successor->dfs_number != 0) {
      continue;
    }
    successor->dfs_number = ++dfs_count;
    successor->dfs_parent = node_index;
    vertex_by_dfs[dfs_count] = successor_index;
    stack[stack_count++] = successor_index;
  }
  return dfs_count;
}

static bool loom_cfg_postdominance_next_reverse_predecessor(
    const loom_cfg_postdominance_region_t* summary, uint32_t node_index,
    uint32_t* cursor, uint32_t* out_predecessor_index) {
  if (node_index == summary->exit_node) {
    return false;
  }
  const loom_cfg_block_index_span_t successors =
      loom_cfg_graph_successors(summary->graph, (uint16_t)node_index);
  while (*cursor < successors.count) {
    const uint32_t candidate_index = successors.values[(*cursor)++];
    if (summary->nodes[candidate_index].dfs_number != 0) {
      *out_predecessor_index = candidate_index;
      return true;
    }
  }
  if (*cursor == successors.count) {
    ++*cursor;
    if (loom_cfg_postdominance_block_is_synthetic_exit(summary, node_index)) {
      *out_predecessor_index = summary->exit_node;
      return true;
    }
  }
  return false;
}

static uint32_t loom_cfg_postdominance_eval(
    loom_cfg_postdominance_node_t* nodes, uint32_t node_index,
    uint32_t* stack) {
  if (nodes[node_index].ancestor == LOOM_CFG_POSTDOMINATOR_INVALID) {
    return nodes[node_index].label;
  }

  iree_host_size_t stack_count = 0;
  uint32_t current_index = node_index;
  while (nodes[current_index].ancestor != LOOM_CFG_POSTDOMINATOR_INVALID &&
         nodes[nodes[current_index].ancestor].ancestor !=
             LOOM_CFG_POSTDOMINATOR_INVALID) {
    stack[stack_count++] = current_index;
    current_index = nodes[current_index].ancestor;
  }
  while (stack_count > 0) {
    current_index = stack[--stack_count];
    const uint32_t ancestor_index = nodes[current_index].ancestor;
    if (nodes[nodes[ancestor_index].label].semidominator <
        nodes[nodes[current_index].label].semidominator) {
      nodes[current_index].label = nodes[ancestor_index].label;
    }
    nodes[current_index].ancestor = nodes[ancestor_index].ancestor;
  }
  return nodes[node_index].label;
}

static void loom_cfg_postdominance_compute_postdominators(
    loom_cfg_postdominance_region_t* summary, uint32_t dfs_count,
    const uint32_t* vertex_by_dfs, uint32_t* stack) {
  loom_cfg_postdominance_node_t* nodes = summary->nodes;
  for (uint32_t dfs_number = 1; dfs_number <= dfs_count; ++dfs_number) {
    const uint32_t node_index = vertex_by_dfs[dfs_number];
    loom_cfg_postdominance_node_t* node = &nodes[node_index];
    node->semidominator = dfs_number;
    node->label = node_index;
    node->ancestor = LOOM_CFG_POSTDOMINATOR_INVALID;
    node->immediate_postdominator = LOOM_CFG_POSTDOMINATOR_INVALID;
    node->bucket_head = LOOM_CFG_POSTDOMINATOR_INVALID;
    node->bucket_next = LOOM_CFG_POSTDOMINATOR_INVALID;
  }

  for (uint32_t dfs_number = dfs_count; dfs_number > 1; --dfs_number) {
    const uint32_t node_index = vertex_by_dfs[dfs_number];
    loom_cfg_postdominance_node_t* node = &nodes[node_index];
    uint32_t predecessor_cursor = 0;
    uint32_t predecessor_index = LOOM_CFG_POSTDOMINATOR_INVALID;
    while (loom_cfg_postdominance_next_reverse_predecessor(
        summary, node_index, &predecessor_cursor, &predecessor_index)) {
      const uint32_t representative =
          loom_cfg_postdominance_eval(nodes, predecessor_index, stack);
      node->semidominator =
          iree_min(node->semidominator, nodes[representative].semidominator);
    }

    const uint32_t semidominator_index = vertex_by_dfs[node->semidominator];
    node->bucket_next = nodes[semidominator_index].bucket_head;
    nodes[semidominator_index].bucket_head = node_index;
    node->ancestor = node->dfs_parent;

    loom_cfg_postdominance_node_t* parent = &nodes[node->dfs_parent];
    uint32_t bucket_index = parent->bucket_head;
    parent->bucket_head = LOOM_CFG_POSTDOMINATOR_INVALID;
    while (bucket_index != LOOM_CFG_POSTDOMINATOR_INVALID) {
      loom_cfg_postdominance_node_t* bucket_node = &nodes[bucket_index];
      const uint32_t next_bucket_index = bucket_node->bucket_next;
      const uint32_t representative =
          loom_cfg_postdominance_eval(nodes, bucket_index, stack);
      bucket_node->immediate_postdominator =
          nodes[representative].semidominator < bucket_node->semidominator
              ? representative
              : node->dfs_parent;
      bucket_index = next_bucket_index;
    }
  }

  nodes[summary->exit_node].immediate_postdominator = summary->exit_node;
  for (uint32_t dfs_number = 2; dfs_number <= dfs_count; ++dfs_number) {
    const uint32_t node_index = vertex_by_dfs[dfs_number];
    loom_cfg_postdominance_node_t* node = &nodes[node_index];
    const uint32_t semidominator_index = vertex_by_dfs[node->semidominator];
    if (node->immediate_postdominator != semidominator_index) {
      node->immediate_postdominator =
          nodes[node->immediate_postdominator].immediate_postdominator;
    }
    node->postdominator_depth =
        nodes[node->immediate_postdominator].postdominator_depth + 1;
  }
}

iree_status_t loom_cfg_postdominance_build(
    const loom_cfg_graph_t* graph, iree_arena_allocator_t* arena,
    loom_cfg_postdominance_t* out_postdominance) {
  *out_postdominance = (loom_cfg_postdominance_t){0};
  if (graph->malformed) {
    return iree_ok_status();
  }
  out_postdominance->exit_node = (uint32_t)graph->block_count;
  const iree_host_size_t node_count = graph->block_count + 1;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, node_count, sizeof(*out_postdominance->nodes),
      (void**)&out_postdominance->nodes));

  const iree_arena_checkpoint_t checkpoint = iree_arena_checkpoint_save(arena);
  loom_cfg_postdominance_region_t summary = {
      .graph = graph,
      .exit_node = out_postdominance->exit_node,
  };
  uint32_t* vertex_by_dfs = NULL;
  uint32_t* stack = NULL;
  iree_status_t status = iree_arena_allocate_array(
      arena, node_count, sizeof(*summary.nodes), (void**)&summary.nodes);
  if (iree_status_is_ok(status)) {
    status = iree_arena_allocate_array(
        arena, node_count + 1, sizeof(*vertex_by_dfs), (void**)&vertex_by_dfs);
  }
  if (iree_status_is_ok(status)) {
    status = iree_arena_allocate_array(arena, node_count, sizeof(*stack),
                                       (void**)&stack);
  }
  if (iree_status_is_ok(status)) {
    memset(summary.nodes, 0, node_count * sizeof(*summary.nodes));
    for (iree_host_size_t i = 0; i < node_count; ++i) {
      summary.nodes[i].immediate_postdominator = LOOM_CFG_POSTDOMINATOR_INVALID;
    }
    loom_cfg_postdominance_mark_exit_reachability(&summary, stack);
    const uint32_t dfs_count = loom_cfg_postdominance_build_reverse_dfs(
        &summary, vertex_by_dfs, stack);
    loom_cfg_postdominance_compute_postdominators(&summary, dfs_count,
                                                  vertex_by_dfs, stack);
    for (iree_host_size_t i = 0; i < node_count; ++i) {
      out_postdominance->nodes[i] = (loom_cfg_postdominator_t){
          .immediate_postdominator = summary.nodes[i].immediate_postdominator,
          .depth = summary.nodes[i].postdominator_depth,
      };
    }
    out_postdominance->available = true;
  }
  iree_arena_checkpoint_restore(&checkpoint);
  return status;
}
