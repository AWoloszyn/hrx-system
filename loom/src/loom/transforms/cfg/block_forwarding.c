// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/transforms/cfg/block_forwarding.h"

#include <string.h>

#include "loom/ops/cfg/ops.h"
#include "loom/ops/op_defs.h"
#include "loom/transforms/cfg/block_arguments.h"

// A planned edge edit borrows the original edge identity and any final payload.
// Forwarding changes no SSA definitions, so payload values remain current.
typedef struct loom_cfg_forward_edge_t {
  // Original terminator and successor ordinal, valid until this edit applies.
  const loom_cfg_edge_info_t* edge;
  // Final destination after resolving the empty chain.
  loom_block_t* destination;
  // Terminal forwarding branch supplying a payload, or NULL for no payload.
  const loom_op_t* payload;
} loom_cfg_forward_edge_t;

typedef enum loom_cfg_forward_state_e {
  LOOM_CFG_FORWARD_UNVISITED = 0,
  LOOM_CFG_FORWARD_ACTIVE = 1,
  LOOM_CFG_FORWARD_RESOLVED = 2,
} loom_cfg_forward_state_t;

// Resolve a functional graph in linear time, retaining the last source-order
// member of each cycle. Each node enters one path and is then permanently
// resolved; shared tails are never walked again.
static iree_status_t loom_cfg_resolve_forwarding_targets(
    uint16_t* targets, iree_host_size_t block_count,
    iree_arena_allocator_t* arena) {
  uint8_t* states = NULL;
  uint16_t* path = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, block_count, sizeof(*states), (void**)&states));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(arena, block_count,
                                                 sizeof(*path), (void**)&path));
  memset(states, 0, block_count * sizeof(*states));
  for (uint16_t start = 0; start < block_count; ++start) {
    if (states[start] == LOOM_CFG_FORWARD_RESOLVED) {
      continue;
    }
    iree_host_size_t path_count = 0;
    uint16_t current = start;
    while (states[current] == LOOM_CFG_FORWARD_UNVISITED) {
      states[current] = LOOM_CFG_FORWARD_ACTIVE;
      path[path_count++] = current;
      current = targets[current];
    }
    uint16_t destination = targets[current];
    if (states[current] == LOOM_CFG_FORWARD_ACTIVE) {
      destination = current;
      for (uint16_t member = targets[current]; member != current;
           member = targets[member]) {
        destination = iree_max(destination, member);
      }
    }
    while (path_count != 0) {
      uint16_t member = path[--path_count];
      targets[member] = destination;
      states[member] = LOOM_CFG_FORWARD_RESOLVED;
    }
  }
  return iree_ok_status();
}

static iree_status_t loom_cfg_apply_forward_edge(
    loom_rewriter_t* rewriter, const loom_cfg_forward_edge_t* edit) {
  loom_op_t* terminator = (loom_op_t*)edit->edge->terminator;
  if (edit->payload) {
    loom_value_slice_t arguments = loom_cfg_br_args((loom_op_t*)edit->payload);
    loom_builder_ip_t saved_ip = loom_builder_save(&rewriter->builder);
    loom_builder_set_before(&rewriter->builder, terminator);
    loom_op_t* branch = NULL;
    iree_status_t status = loom_cfg_br_build(
        &rewriter->builder, edit->destination, arguments.values,
        arguments.count, terminator->location, &branch);
    loom_builder_restore(&rewriter->builder, saved_ip);
    IREE_RETURN_IF_ERROR(status);
    return loom_rewriter_erase(rewriter, terminator);
  }
  loom_op_successors(terminator)[edit->edge->successor_index] =
      edit->destination;
  rewriter->flags |= LOOM_REWRITER_FLAG_CHANGED;
  return loom_rewriter_add_to_worklist(rewriter, terminator);
}

iree_status_t loom_cfg_forward_empty_blocks(
    loom_rewriter_t* rewriter, const loom_cfg_graph_t* graph,
    const loom_dominance_info_t* dominance, iree_arena_allocator_t* arena,
    iree_host_size_t* out_forwarded_count) {
  *out_forwarded_count = 0;
  if (graph->malformed || graph->block_count <= 1) {
    return iree_ok_status();
  }
  uint16_t* targets = NULL;
  const loom_op_t** payloads = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, graph->block_count, sizeof(*targets), (void**)&targets));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, graph->block_count, sizeof(*payloads), (void**)&payloads));
  memset(payloads, 0, graph->block_count * sizeof(*payloads));
  bool has_forwarding = false;
  for (uint16_t i = 0; i < graph->block_count; ++i) {
    targets[i] = i;
    const loom_block_t* block = graph->blocks[i].block;
    if (i == 0 || block->arg_count != 0 || block->first_op != block->last_op ||
        !loom_cfg_br_isa(block->last_op)) {
      continue;
    }
    const loom_op_t* branch = block->last_op;
    loom_block_t* destination = loom_cfg_br_dest(branch);
    if (destination->arg_count != 0) {
      payloads[i] = branch;
      has_forwarding = true;
    } else {
      targets[i] = destination->region_index;
      has_forwarding |= targets[i] != i;
    }
  }
  if (!has_forwarding) {
    return iree_ok_status();
  }
  IREE_RETURN_IF_ERROR(
      loom_cfg_resolve_forwarding_targets(targets, graph->block_count, arena));

  loom_cfg_forward_edge_t* edits = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, graph->edge_count, sizeof(*edits), (void**)&edits));
  iree_host_size_t edit_count = 0;
  for (iree_host_size_t i = 0; i < graph->edge_count; ++i) {
    const loom_cfg_edge_info_t* edge = &graph->edges[i];
    const loom_op_t* terminator = edge->terminator;
    if (!loom_cfg_br_isa(terminator) && !loom_cfg_cond_br_isa(terminator)) {
      continue;
    }
    uint16_t target_index = targets[edge->target_block_index];
    loom_cfg_forward_edge_t edit = {
        .edge = edge,
        .destination = (loom_block_t*)graph->blocks[target_index].block,
    };
    const loom_op_t* payload = payloads[target_index];
    if (payload && loom_cfg_br_isa(terminator) &&
        loom_cfg_block_arguments_can_replace(
            rewriter->module, dominance, loom_cfg_br_dest(payload),
            loom_cfg_br_args((loom_op_t*)payload), terminator)) {
      edit.destination = loom_cfg_br_dest(payload);
      edit.payload = payload;
    }
    if (edit.destination == graph->blocks[edge->target_block_index].block) {
      continue;
    }
    edits[edit_count++] = edit;
  }

  for (iree_host_size_t i = 0; i < edit_count; ++i) {
    IREE_RETURN_IF_ERROR(loom_cfg_apply_forward_edge(rewriter, &edits[i]));
  }
  *out_forwarded_count = edit_count;
  return iree_ok_status();
}
