// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/util/fact_cfg.h"

#include <string.h>

iree_host_size_t loom_value_fact_cfg_region_argument_index(
    const loom_value_fact_cfg_region_t* region, loom_value_id_t value_id) {
  const loom_value_t* value = loom_module_value(region->graph.module, value_id);
  if (!loom_value_is_block_arg(value)) return IREE_HOST_SIZE_MAX;
  iree_host_size_t block_index =
      loom_cfg_graph_block_index(&region->graph, loom_value_def_block(value));
  if (block_index == IREE_HOST_SIZE_MAX || block_index == 0 ||
      !loom_cfg_graph_block_is_reachable(&region->graph, block_index)) {
    return IREE_HOST_SIZE_MAX;
  }
  return region->argument_offsets[block_index] + loom_value_def_index(value);
}

static iree_status_t loom_value_fact_cfg_visit_forwarded_arguments(
    void* user_data, iree_host_size_t node,
    loom_scc_successor_callback_t successor) {
  const loom_value_fact_cfg_region_t* region = user_data;
  const loom_value_fact_cfg_argument_t* argument = &region->arguments[node];
  const loom_cfg_graph_t* graph = &region->graph;
  const loom_block_t* block = graph->blocks[argument->block_index].block;
  loom_cfg_edge_index_span_t incoming =
      loom_cfg_graph_predecessor_edges(graph, argument->block_index);
  for (iree_host_size_t i = 0; i < incoming.count; ++i) {
    const loom_cfg_edge_info_t* edge =
        loom_cfg_graph_edge(graph, incoming.values[i]);
    if (!loom_cfg_graph_block_is_reachable(graph, edge->source_block_index)) {
      continue;
    }
    const loom_value_id_t* sources = NULL;
    uint16_t count = 0;
    if (!loom_cfg_terminator_payload_for_successor(edge->terminator, block,
                                                   &sources, &count) ||
        argument->argument_index >= count) {
      continue;
    }
    iree_host_size_t source = loom_value_fact_cfg_region_argument_index(
        region, sources[argument->argument_index]);
    if (source != IREE_HOST_SIZE_MAX) {
      IREE_RETURN_IF_ERROR(successor.fn(successor.user_data, source));
    }
  }
  return iree_ok_status();
}

static iree_status_t loom_value_fact_cfg_visit_successors(
    void* user_data, iree_host_size_t node,
    loom_scc_successor_callback_t successor) {
  const loom_cfg_graph_t* graph = user_data;
  loom_cfg_block_index_span_t successors =
      loom_cfg_graph_successors(graph, node);
  for (iree_host_size_t i = 0; i < successors.count; ++i) {
    IREE_RETURN_IF_ERROR(
        successor.fn(successor.user_data, successors.values[i]));
  }
  return iree_ok_status();
}

iree_status_t loom_value_fact_cfg_region_initialize(
    const loom_module_t* module, const loom_region_t* region,
    iree_arena_allocator_t* arena, loom_value_fact_cfg_region_t* out_region) {
  *out_region = (loom_value_fact_cfg_region_t){0};
  IREE_RETURN_IF_ERROR(
      loom_cfg_graph_build(module, region, arena, &out_region->graph));
  if (out_region->graph.backward_edge_count == 0) return iree_ok_status();
  const loom_scc_graph_t block_graph = {
      .node_count = region->block_count,
      .visit_successors = loom_scc_visit_successors_callback_make(
          loom_value_fact_cfg_visit_successors, &out_region->graph),
  };
  const iree_host_size_t entry_node = 0;
  const loom_scc_options_t block_options = {
      .root_nodes = &entry_node,
      .root_count = 1,
  };
  IREE_RETURN_IF_ERROR(loom_scc_compute(&block_graph, &block_options, arena,
                                        &out_region->control_flow.components));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, region->block_count,
      sizeof(*out_region->control_flow.block_components),
      (void**)&out_region->control_flow.block_components));
  memset(
      out_region->control_flow.block_components, 0xFF,
      region->block_count * sizeof(*out_region->control_flow.block_components));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, out_region->control_flow.components.count,
      sizeof(*out_region->control_flow.anchors),
      (void**)&out_region->control_flow.anchors));
  memset(out_region->control_flow.anchors, 0,
         out_region->control_flow.components.count *
             sizeof(*out_region->control_flow.anchors));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, out_region->control_flow.components.count,
      sizeof(*out_region->control_flow.dirty),
      (void**)&out_region->control_flow.dirty));
  memset(out_region->control_flow.dirty, 0,
         out_region->control_flow.components.count *
             sizeof(*out_region->control_flow.dirty));
  for (iree_host_size_t i = 0; i < out_region->control_flow.components.count;
       ++i) {
    const loom_scc_t* component =
        &out_region->control_flow.components.values[i];
    for (iree_host_size_t j = 0; j < component->node_count; ++j) {
      iree_host_size_t block_index = component->nodes[j];
      out_region->control_flow.block_components[block_index] = i;
      const loom_block_t* block = out_region->graph.blocks[block_index].block;
      if (!component->is_cycle || !block->arg_count ||
          out_region->control_flow.anchors[i])
        continue;
      loom_cfg_edge_index_span_t predecessors =
          loom_cfg_graph_predecessor_edges(&out_region->graph, block_index);
      for (iree_host_size_t k = 0; k < predecessors.count; ++k) {
        const loom_cfg_edge_info_t* edge =
            &out_region->graph.edges[predecessors.values[k]];
        if (out_region->graph.blocks[edge->source_block_index].reachable &&
            edge->terminator->successor_count == 1) {
          out_region->control_flow.anchors[i] = (loom_op_t*)edge->terminator;
          break;
        }
      }
    }
  }
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, region->block_count + 1, sizeof(*out_region->argument_offsets),
      (void**)&out_region->argument_offsets));
  for (uint16_t i = 0; i < region->block_count; ++i) {
    out_region->argument_offsets[i] = out_region->argument_count;
    if (i != 0 && loom_cfg_graph_block_is_reachable(&out_region->graph, i)) {
      out_region->argument_count +=
          out_region->graph.blocks[i].block->arg_count;
    }
  }
  out_region->argument_offsets[region->block_count] =
      out_region->argument_count;
  if (out_region->argument_count == 0) return iree_ok_status();
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, out_region->argument_count, sizeof(*out_region->arguments),
      (void**)&out_region->arguments));
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate_array(arena, out_region->argument_count,
                                sizeof(*out_region->argument_components),
                                (void**)&out_region->argument_components));
  for (uint16_t i = 1; i < region->block_count; ++i) {
    if (!loom_cfg_graph_block_is_reachable(&out_region->graph, i)) continue;
    const loom_block_t* block = out_region->graph.blocks[i].block;
    for (uint16_t j = 0; j < block->arg_count; ++j) {
      out_region->arguments[out_region->argument_offsets[i] + j] =
          (loom_value_fact_cfg_argument_t){
              .value_id = loom_block_arg_id(block, j),
              .block_index = i,
              .argument_index = j,
          };
    }
  }
  const loom_scc_graph_t graph = {
      .node_count = out_region->argument_count,
      .visit_successors = loom_scc_visit_successors_callback_make(
          loom_value_fact_cfg_visit_forwarded_arguments, out_region),
  };
  IREE_RETURN_IF_ERROR(
      loom_scc_compute(&graph, NULL, arena, &out_region->components));
  for (iree_host_size_t i = 0; i < out_region->components.count; ++i) {
    const loom_scc_t* component = &out_region->components.values[i];
    for (iree_host_size_t j = 0; j < component->node_count; ++j) {
      out_region->argument_components[component->nodes[j]] = i;
    }
  }
  return iree_ok_status();
}
