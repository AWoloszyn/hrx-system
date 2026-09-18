// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/util/cfg_control.h"

#include <string.h>

#include "loom/analysis/scc.h"

// Construction-only heavy-path index over the postdominator tree.
typedef struct loom_cfg_control_path_t {
  // First child in the postdominator tree, or INVALID.
  uint32_t child;
  // Next child of the same parent, or INVALID.
  uint32_t sibling;
  // Number of nodes in this subtree, including this node.
  uint32_t size;
  // Largest child subtree, or INVALID for a leaf.
  uint32_t heavy;
  // First node of this node's heavy path.
  uint32_t head;
  // Leaf position in heavy-path order.
  uint32_t position;
} loom_cfg_control_path_t;

static bool loom_cfg_control_has_alternatives(const loom_cfg_graph_t* graph,
                                              uint32_t block) {
  const loom_cfg_block_index_span_t successors =
      loom_cfg_graph_successors(graph, block);
  for (iree_host_size_t i = 1; i < successors.count; ++i) {
    if (successors.values[i] != successors.values[0]) {
      return true;
    }
  }
  return false;
}

static iree_status_t loom_cfg_control_build_paths(
    loom_cfg_control_t* control, iree_arena_allocator_t* scratch,
    loom_cfg_control_path_t** out_paths) {
  const uint32_t exit = control->postdominance.exit_node;
  loom_cfg_control_path_t* paths = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      scratch, exit + 1, sizeof(*paths), (void**)&paths));
  memset(paths, 0xFF, (exit + 1) * sizeof(*paths));
  for (uint32_t i = 0; i <= exit; ++i) {
    paths[i].size = 1;
  }
  for (uint32_t i = 0; i < exit; ++i) {
    const uint32_t parent =
        control->postdominance.nodes[i].immediate_postdominator;
    if (parent == LOOM_CFG_POSTDOMINATOR_INVALID) {
      continue;
    }
    paths[i].sibling = paths[parent].child;
    paths[parent].child = i;
  }
  uint32_t* order = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      scratch, exit + 1, sizeof(*order), (void**)&order));
  uint32_t count = 1;
  order[0] = exit;
  for (uint32_t cursor = 0; cursor < count; ++cursor) {
    for (uint32_t child = paths[order[cursor]].child;
         child != LOOM_CFG_CONTROL_INVALID; child = paths[child].sibling) {
      order[count++] = child;
    }
  }
  for (uint32_t cursor = count; cursor > 1; --cursor) {
    const uint32_t node = order[cursor - 1];
    const uint32_t parent =
        control->postdominance.nodes[node].immediate_postdominator;
    paths[parent].size += paths[node].size;
    if (paths[parent].heavy == LOOM_CFG_CONTROL_INVALID ||
        paths[node].size > paths[paths[parent].heavy].size) {
      paths[parent].heavy = node;
    }
  }
  control->leaf_base = count - 1;
  control->node_count = 2 * count - 1;
  // Reuse the order array as an explicit stack of pending light-path heads.
  uint32_t pending = 1;
  uint32_t position = 0;
  order[0] = exit;
  while (pending) {
    const uint32_t head = order[--pending];
    for (uint32_t node = head; node != LOOM_CFG_CONTROL_INVALID;
         node = paths[node].heavy) {
      paths[node].head = head;
      paths[node].position = position++;
      for (uint32_t child = paths[node].child;
           child != LOOM_CFG_CONTROL_INVALID; child = paths[child].sibling) {
        if (child != paths[node].heavy) {
          order[pending++] = child;
        }
      }
    }
  }
  *out_paths = paths;
  return iree_ok_status();
}

typedef struct loom_cfg_control_bindings_t {
  // Scratch allocation owner for the growing binding array.
  iree_arena_allocator_t* arena;
  // Constructed control-path bindings.
  loom_cfg_control_binding_t* values;
  // Number of initialized bindings.
  iree_host_size_t count;
  // Allocated binding slots.
  iree_host_size_t capacity;
} loom_cfg_control_bindings_t;

static iree_status_t loom_cfg_control_append_binding(
    loom_cfg_control_bindings_t* bindings, loom_cfg_edge_index_t edge,
    uint32_t node) {
  if (bindings->count == LOOM_CFG_CONTROL_INVALID) {
    return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                            "CFG control bindings exceed 32-bit index space");
  }
  if (bindings->count == bindings->capacity) {
    IREE_RETURN_IF_ERROR(
        iree_arena_grow_array(bindings->arena, bindings->count,
                              bindings->count + 1, sizeof(*bindings->values),
                              &bindings->capacity, (void**)&bindings->values));
  }
  bindings->values[bindings->count++] = (loom_cfg_control_binding_t){
      .edge = edge,
      .node = node,
      .selector_input = LOOM_CFG_CONTROL_INVALID,
  };
  return iree_ok_status();
}

static iree_status_t loom_cfg_control_bind_interval(
    loom_cfg_control_bindings_t* bindings, loom_cfg_edge_index_t edge,
    uint32_t leaf_count, uint32_t begin, uint32_t end) {
  // Use one-based heap indices for the half-open interval decomposition, then
  // convert retained nodes to the zero-based representation.
  begin += leaf_count;
  end += leaf_count;
  while (begin < end) {
    if (begin & 1) {
      IREE_RETURN_IF_ERROR(
          loom_cfg_control_append_binding(bindings, edge, begin++ - 1));
    }
    if (end & 1) {
      IREE_RETURN_IF_ERROR(
          loom_cfg_control_append_binding(bindings, edge, --end - 1));
    }
    begin /= 2;
    end /= 2;
  }
  return iree_ok_status();
}

static iree_status_t loom_cfg_control_build_bindings(
    loom_cfg_control_t* control, const loom_cfg_control_path_t* paths,
    iree_arena_allocator_t* scratch, iree_arena_allocator_t* arena) {
  const loom_cfg_graph_t* graph = control->graph;
  loom_cfg_control_bindings_t bindings = {.arena = scratch};
  for (uint32_t block = 0; block < graph->block_count; ++block) {
    loom_cfg_control_block_t* output = &control->blocks[block];
    output->binding_start = bindings.count;
    if (!graph->blocks[block].reachable ||
        !loom_cfg_control_has_alternatives(graph, block)) {
      continue;
    }
    const uint32_t stop =
        control->postdominance.nodes[block].immediate_postdominator;
    const loom_cfg_edge_index_span_t edges =
        loom_cfg_graph_successor_edges(graph, block);
    for (iree_host_size_t i = 0; i < edges.count; ++i) {
      const loom_cfg_edge_index_t edge = edges.values[i];
      uint32_t node = graph->edges[edge].target_block_index;
      while (control->postdominance.nodes[node].depth >
                 control->postdominance.nodes[stop].depth &&
             paths[node].head != paths[stop].head) {
        const uint32_t head = paths[node].head;
        IREE_RETURN_IF_ERROR(loom_cfg_control_bind_interval(
            &bindings, edge, control->leaf_base + 1, paths[head].position,
            paths[node].position + 1));
        node = control->postdominance.nodes[head].immediate_postdominator;
      }
      if (control->postdominance.nodes[node].depth >
          control->postdominance.nodes[stop].depth) {
        IREE_RETURN_IF_ERROR(loom_cfg_control_bind_interval(
            &bindings, edge, control->leaf_base + 1, paths[stop].position + 1,
            paths[node].position + 1));
      }
    }
    output->binding_count = bindings.count - output->binding_start;
  }
  control->binding_count = bindings.count;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(arena, bindings.count,
                                                 sizeof(*control->bindings),
                                                 (void**)&control->bindings));
  if (bindings.count) {
    memcpy(control->bindings, bindings.values,
           bindings.count * sizeof(*control->bindings));
  }
  return iree_ok_status();
}

typedef struct loom_cfg_control_successors_t {
  // Compressed graph whose segment and binding edges are being traversed.
  const loom_cfg_control_t* control;
  // Real block index at each leaf position, or the synthetic exit index.
  const uint32_t* leaf_blocks;
} loom_cfg_control_successors_t;

static iree_status_t loom_cfg_control_visit_successors(
    void* user_data, iree_host_size_t node,
    loom_scc_successor_callback_t successor) {
  const loom_cfg_control_successors_t* context = user_data;
  const loom_cfg_control_t* control = context->control;
  if (node < control->leaf_base) {
    IREE_RETURN_IF_ERROR(successor.fn(successor.user_data, 2 * node + 1));
    return successor.fn(successor.user_data, 2 * node + 2);
  }
  const uint32_t block = context->leaf_blocks[node - control->leaf_base];
  if (block == control->graph->block_count) {
    return iree_ok_status();
  }
  const loom_cfg_control_block_t* source = &control->blocks[block];
  for (uint32_t i = source->binding_start;
       i < source->binding_start + source->binding_count; ++i) {
    IREE_RETURN_IF_ERROR(
        successor.fn(successor.user_data, control->bindings[i].node));
  }
  return iree_ok_status();
}

static void loom_cfg_control_append_input(loom_cfg_control_t* control,
                                          uint32_t source, uint32_t target,
                                          loom_cfg_edge_index_t edge) {
  const uint32_t index = control->input_count++;
  control->inputs[index] = (loom_cfg_control_input_t){
      .target_component = target,
      .source_component = source,
      .edge = edge,
      .next_outgoing = LOOM_CFG_CONTROL_INVALID,
  };
  if (source != LOOM_CFG_CONTROL_INVALID) {
    control->inputs[index].next_outgoing =
        control->components[source].outgoing_head;
    control->components[source].outgoing_head = index;
  }
}

static iree_status_t loom_cfg_control_build_components(
    loom_cfg_control_t* control, const loom_cfg_control_path_t* paths,
    iree_arena_allocator_t* scratch, iree_arena_allocator_t* arena) {
  uint32_t* leaf_blocks = NULL;
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate_array(scratch, control->leaf_base + 1,
                                sizeof(*leaf_blocks), (void**)&leaf_blocks));
  for (uint32_t i = 0; i <= control->graph->block_count; ++i) {
    if (paths[i].position != LOOM_CFG_CONTROL_INVALID) {
      leaf_blocks[paths[i].position] = i;
    }
  }
  const loom_cfg_control_successors_t context = {control, leaf_blocks};
  const loom_scc_graph_t graph = {
      .node_count = control->node_count,
      .visit_successors = loom_scc_visit_successors_callback_make(
          loom_cfg_control_visit_successors, (void*)&context),
  };
  loom_scc_list_t components = {0};
  IREE_RETURN_IF_ERROR(loom_scc_compute(&graph, NULL, scratch, &components));
  control->component_count = components.count;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(arena, components.count,
                                                 sizeof(*control->components),
                                                 (void**)&control->components));
  memset(control->components, 0xFF,
         components.count * sizeof(*control->components));
  for (uint32_t i = 0; i < components.count; ++i) {
    const loom_scc_t* component = &components.values[i];
    for (iree_host_size_t j = 0; j < component->node_count; ++j) {
      control->nodes[component->nodes[j]].component = i;
    }
  }
  for (uint32_t i = 0; i < control->graph->block_count; ++i) {
    loom_cfg_control_block_t* block = &control->blocks[i];
    if (block->node == LOOM_CFG_CONTROL_INVALID) {
      continue;
    }
    loom_cfg_control_component_t* component =
        &control->components[control->nodes[block->node].component];
    block->next_component_block = component->block_head;
    component->block_head = i;
  }
  // Each selector contributes one input even for an internal cyclic binding.
  iree_host_size_t count = control->binding_count;
  for (uint32_t node = 1; node < control->node_count; ++node) {
    count += control->nodes[(node - 1) / 2].component !=
             control->nodes[node].component;
  }
  for (uint32_t i = 0; i < control->binding_count; ++i) {
    const loom_cfg_control_binding_t* binding = &control->bindings[i];
    const uint32_t source =
        control->blocks[control->graph->edges[binding->edge].source_block_index]
            .node;
    count += control->nodes[source].component !=
             control->nodes[binding->node].component;
  }
  if (count >= LOOM_CFG_CONTROL_INVALID) {
    return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                            "CFG control inputs exceed 32-bit index space");
  }
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, count, sizeof(*control->inputs), (void**)&control->inputs));
  for (uint32_t node = 1; node < control->node_count; ++node) {
    const uint32_t source = control->nodes[(node - 1) / 2].component;
    const uint32_t target = control->nodes[node].component;
    if (source != target) {
      loom_cfg_control_append_input(control, source, target,
                                    LOOM_CFG_EDGE_INDEX_INVALID);
    }
  }
  for (uint32_t i = 0; i < control->binding_count; ++i) {
    loom_cfg_control_binding_t* binding = &control->bindings[i];
    const uint32_t source =
        control
            ->nodes[control
                        ->blocks[control->graph->edges[binding->edge]
                                     .source_block_index]
                        .node]
            .component;
    const uint32_t target = control->nodes[binding->node].component;
    if (source != target) {
      loom_cfg_control_append_input(control, source, target,
                                    LOOM_CFG_EDGE_INDEX_INVALID);
    }
    binding->selector_input = control->input_count;
    loom_cfg_control_append_input(control, LOOM_CFG_CONTROL_INVALID, target,
                                  binding->edge);
  }
  return iree_ok_status();
}

static iree_status_t loom_cfg_control_build_structure(
    loom_cfg_control_t* control, iree_arena_allocator_t* scratch,
    iree_arena_allocator_t* arena) {
  IREE_RETURN_IF_ERROR(loom_cfg_postdominance_build(control->graph, arena,
                                                    &control->postdominance));
  loom_cfg_control_path_t* paths = NULL;
  IREE_RETURN_IF_ERROR(loom_cfg_control_build_paths(control, scratch, &paths));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, control->graph->block_count, sizeof(*control->blocks),
      (void**)&control->blocks));
  for (uint32_t i = 0; i < control->graph->block_count; ++i) {
    control->blocks[i] = (loom_cfg_control_block_t){
        .node = control->graph->blocks[i].reachable
                    ? control->leaf_base + paths[i].position
                    : LOOM_CFG_CONTROL_INVALID,
        .next_component_block = LOOM_CFG_CONTROL_INVALID,
    };
  }
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(arena, control->node_count,
                                                 sizeof(*control->nodes),
                                                 (void**)&control->nodes));
  memset(control->nodes, 0xFF, control->node_count * sizeof(*control->nodes));
  IREE_RETURN_IF_ERROR(
      loom_cfg_control_build_bindings(control, paths, scratch, arena));
  return loom_cfg_control_build_components(control, paths, scratch, arena);
}

iree_status_t loom_cfg_control_build(const loom_cfg_graph_t* graph,
                                     iree_arena_allocator_t* arena,
                                     loom_cfg_control_t* out_control) {
  *out_control = (loom_cfg_control_t){.graph = graph};
  if (graph->malformed) {
    return iree_ok_status();
  }
  bool has_alternatives = false;
  for (uint32_t i = 0; i < graph->block_count && !has_alternatives; ++i) {
    has_alternatives = graph->blocks[i].reachable &&
                       loom_cfg_control_has_alternatives(graph, i);
  }
  if (!has_alternatives) {
    out_control->available = true;
    return iree_ok_status();
  }
  iree_arena_allocator_t scratch;
  iree_arena_initialize(arena->block_pool, &scratch);
  iree_status_t status =
      loom_cfg_control_build_structure(out_control, &scratch, arena);
  iree_arena_deinitialize(&scratch);
  out_control->available = iree_status_is_ok(status);
  return status;
}
