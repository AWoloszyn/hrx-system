// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/analysis/scc.h"

#include <string.h>

typedef struct loom_scc_frame_t {
  // Node whose successor enumeration is suspended by a DFS child.
  iree_host_size_t node;
  // Next successor in the active-frame adjacency buffer.
  iree_host_size_t next_successor;
  // Exclusive end of this frame's successor slice.
  iree_host_size_t successor_end;
} loom_scc_frame_t;

typedef struct loom_scc_state_t {
  // Caller graph adapter.
  const loom_scc_graph_t* graph;
  // Arena owning traversal-only buffers, reclaimed after the solve.
  iree_arena_allocator_t* arena;
  // Explicit DFS frames, bounded by the graph node count.
  loom_scc_frame_t* frames;
  // Number of suspended or active DFS frames.
  iree_host_size_t frame_count;
  // Adjacency retained only for currently active DFS frames.
  struct {
    // Successor slices enumerated once per reached node.
    iree_host_size_t* values;
    // Logical end of the active-frame slices.
    iree_host_size_t count;
    // Allocated slots, reused as completed frames are popped.
    iree_host_size_t capacity;
  } successors;
  // Tarjan discovery index per node, or IREE_HOST_SIZE_MAX when unvisited.
  iree_host_size_t* indexes;
  // Tarjan lowlink value per node.
  iree_host_size_t* lowlinks;
  // True while a node is present in stack_nodes.
  bool* on_stack;
  // True when a node has an explicit self-edge.
  bool* has_self_edge;
  // Tarjan active stack.
  iree_host_size_t* stack_nodes;
  // Number of entries currently in stack_nodes.
  iree_host_size_t stack_count;
  // Next discovery index to assign.
  iree_host_size_t next_index;
  // Component descriptors allocated for the output list.
  loom_scc_t* components;
  // Number of entries currently written to components.
  iree_host_size_t component_count;
  // Backing node storage for component node slices.
  iree_host_size_t* component_nodes;
  // Number of entries currently written to component_nodes.
  iree_host_size_t component_node_count;
} loom_scc_state_t;

static iree_status_t loom_scc_append_successor(void* user_data,
                                               iree_host_size_t successor) {
  loom_scc_state_t* state = user_data;
  if (successor >= state->graph->node_count) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "SCC graph edge references node %" PRIhsz
                            " but graph has %" PRIhsz " nodes",
                            successor, state->graph->node_count);
  }
  if (state->successors.count == state->successors.capacity) {
    IREE_RETURN_IF_ERROR(iree_arena_grow_array(
        state->arena, state->successors.count, state->successors.count + 1,
        sizeof(*state->successors.values), &state->successors.capacity,
        (void**)&state->successors.values));
  }
  state->successors.values[state->successors.count++] = successor;
  return iree_ok_status();
}

static void loom_scc_emit_component(loom_scc_state_t* state,
                                    iree_host_size_t root_node) {
  iree_host_size_t first_node = state->component_node_count;
  bool has_self_edge = false;
  iree_host_size_t node;
  do {
    node = state->stack_nodes[--state->stack_count];
    state->on_stack[node] = false;
    has_self_edge |= state->has_self_edge[node];
    state->component_nodes[state->component_node_count++] = node;
  } while (node != root_node);

  iree_host_size_t node_count = state->component_node_count - first_node;
  state->components[state->component_count++] = (loom_scc_t){
      .nodes = state->component_nodes + first_node,
      .node_count = node_count,
      .is_cycle = node_count > 1 || has_self_edge,
  };
}

static iree_status_t loom_scc_push_node(loom_scc_state_t* state,
                                        iree_host_size_t node) {
  state->indexes[node] = state->next_index;
  state->lowlinks[node] = state->next_index++;
  state->stack_nodes[state->stack_count++] = node;
  state->on_stack[node] = true;
  const iree_host_size_t first_successor = state->successors.count;
  IREE_RETURN_IF_ERROR(state->graph->visit_successors.fn(
      state->graph->visit_successors.user_data, node,
      loom_scc_successor_callback_make(loom_scc_append_successor, state)));
  state->frames[state->frame_count++] = (loom_scc_frame_t){
      .node = node,
      .next_successor = first_successor,
      .successor_end = state->successors.count,
  };
  return iree_ok_status();
}

static iree_status_t loom_scc_visit_node(loom_scc_state_t* state,
                                         iree_host_size_t root) {
  IREE_RETURN_IF_ERROR(loom_scc_push_node(state, root));
  while (state->frame_count) {
    loom_scc_frame_t* frame = &state->frames[state->frame_count - 1];
    const iree_host_size_t node = frame->node;
    if (frame->next_successor != frame->successor_end) {
      const iree_host_size_t successor =
          state->successors.values[frame->next_successor++];
      if (successor == node) {
        state->has_self_edge[node] = true;
      }
      if (state->indexes[successor] == IREE_HOST_SIZE_MAX) {
        IREE_RETURN_IF_ERROR(loom_scc_push_node(state, successor));
      } else if (state->on_stack[successor]) {
        state->lowlinks[node] =
            iree_min(state->lowlinks[node], state->indexes[successor]);
      }
      continue;
    }
    if (state->lowlinks[node] == state->indexes[node]) {
      loom_scc_emit_component(state, node);
    }
    --state->frame_count;
    if (state->frame_count) {
      const loom_scc_frame_t* parent = &state->frames[state->frame_count - 1];
      state->lowlinks[parent->node] =
          iree_min(state->lowlinks[parent->node], state->lowlinks[node]);
      state->successors.count = parent->successor_end;
    } else {
      state->successors.count = 0;
    }
  }
  return iree_ok_status();
}

static iree_status_t loom_scc_allocate_state(const loom_scc_graph_t* graph,
                                             iree_arena_allocator_t* arena,
                                             loom_scc_state_t* state) {
  state->graph = graph;
  state->arena = arena;
  if (graph->node_count == 0) {
    return iree_ok_status();
  }
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(arena, graph->node_count,
                                                 sizeof(*state->indexes),
                                                 (void**)&state->indexes));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(arena, graph->node_count,
                                                 sizeof(*state->lowlinks),
                                                 (void**)&state->lowlinks));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(arena, graph->node_count,
                                                 sizeof(*state->on_stack),
                                                 (void**)&state->on_stack));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, graph->node_count, sizeof(*state->has_self_edge),
      (void**)&state->has_self_edge));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(arena, graph->node_count,
                                                 sizeof(*state->stack_nodes),
                                                 (void**)&state->stack_nodes));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(arena, graph->node_count,
                                                 sizeof(*state->frames),
                                                 (void**)&state->frames));
  for (iree_host_size_t i = 0; i < graph->node_count; ++i) {
    state->indexes[i] = IREE_HOST_SIZE_MAX;
  }
  memset(state->on_stack, 0, graph->node_count * sizeof(*state->on_stack));
  memset(state->has_self_edge, 0,
         graph->node_count * sizeof(*state->has_self_edge));
  return iree_ok_status();
}

static iree_status_t loom_scc_visit_root(loom_scc_state_t* state,
                                         iree_host_size_t root_node) {
  if (root_node >= state->graph->node_count) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "SCC root node %" PRIhsz
                            " out of range for graph with %" PRIhsz " nodes",
                            root_node, state->graph->node_count);
  }
  if (state->indexes[root_node] != IREE_HOST_SIZE_MAX) {
    return iree_ok_status();
  }
  return loom_scc_visit_node(state, root_node);
}

iree_status_t loom_scc_compute(const loom_scc_graph_t* graph,
                               const loom_scc_options_t* options,
                               iree_arena_allocator_t* arena,
                               loom_scc_list_t* out_sccs) {
  *out_sccs = (loom_scc_list_t){0};
  if (graph->node_count > 0 && !graph->visit_successors.fn) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "non-empty SCC graph requires successor iterator");
  }
  if (options && options->root_count > 0 && !options->root_nodes) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "SCC root filter requires root nodes");
  }
  loom_scc_state_t state = {0};
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(arena, graph->node_count,
                                                 sizeof(*state.components),
                                                 (void**)&state.components));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, graph->node_count, sizeof(*state.component_nodes),
      (void**)&state.component_nodes));
  iree_arena_allocator_t scratch_arena;
  iree_arena_initialize(arena->block_pool, &scratch_arena);
  iree_status_t status = loom_scc_allocate_state(graph, &scratch_arena, &state);
  if (options && options->root_nodes) {
    for (iree_host_size_t i = 0;
         i < options->root_count && iree_status_is_ok(status); ++i) {
      status = loom_scc_visit_root(&state, options->root_nodes[i]);
    }
  } else {
    for (iree_host_size_t i = 0;
         i < graph->node_count && iree_status_is_ok(status); ++i) {
      status = loom_scc_visit_root(&state, i);
    }
  }
  iree_arena_deinitialize(&scratch_arena);
  if (iree_status_is_ok(status)) {
    *out_sccs = (loom_scc_list_t){
        .values = state.components,
        .count = state.component_count,
    };
  }
  return status;
}
