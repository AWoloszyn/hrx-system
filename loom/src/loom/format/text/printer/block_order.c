// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/format/text/printer/block_order.h"

#include <string.h>

#include "loom/ir/module.h"
#include "loom/util/cfg_dominance.h"
#include "loom/util/cfg_graph.h"

static iree_status_t loom_print_block_order_build(
    const loom_module_t* module, const loom_region_t* region,
    loom_print_block_order_t* order) {
  loom_cfg_graph_t graph = {0};
  loom_cfg_dominance_t dominance = {0};
  IREE_RETURN_IF_ERROR(
      loom_cfg_graph_build(module, region, &order->arena, &graph));
  IREE_RETURN_IF_ERROR(
      loom_cfg_dominance_build(&graph, &order->arena, &dominance));
  if (!dominance.available) {
    return iree_ok_status();
  }

  bool needs_order = false;
  bool saw_unreachable = false;
  for (uint16_t i = 0; i < region->block_count; ++i) {
    if (!graph.blocks[i].reachable) {
      saw_unreachable = true;
    } else if (saw_unreachable || dominance.immediate_dominators[i] > i) {
      needs_order = true;
    }
  }
  if (!needs_order) {
    return iree_ok_status();
  }

  uint16_t* indices = NULL;
  uint16_t* pending = NULL;
  uint8_t* emitted = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      &order->arena, region->block_count, sizeof(*indices), (void**)&indices));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      &order->arena, region->block_count, sizeof(*pending), (void**)&pending));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      &order->arena, region->block_count, sizeof(*emitted), (void**)&emitted));
  memset(emitted, 0, region->block_count * sizeof(*emitted));

  // Pull only the not-yet-emitted ancestors ahead of each physical block.
  // Every block enters the pending stack once; deep CFGs do not use recursion.
  uint16_t count = 0;
  for (uint16_t i = 0; i < region->block_count; ++i) {
    if (!graph.blocks[i].reachable || emitted[i]) {
      continue;
    }
    uint16_t pending_count = 0;
    uint16_t ancestor = i;
    while (!emitted[ancestor]) {
      pending[pending_count++] = ancestor;
      if (ancestor == 0) {
        break;
      }
      ancestor = dominance.immediate_dominators[ancestor];
    }
    while (pending_count > 0) {
      uint16_t block_index = pending[--pending_count];
      emitted[block_index] = 1;
      indices[count++] = block_index;
    }
  }
  for (uint16_t i = 0; i < region->block_count; ++i) {
    if (!graph.blocks[i].reachable) {
      indices[count++] = i;
    }
  }
  order->indices = indices;
  return iree_ok_status();
}

iree_status_t loom_print_block_order_initialize(
    const loom_module_t* module, const loom_region_t* region,
    loom_print_block_order_t* out_order) {
  memset(out_order, 0, sizeof(*out_order));
  if (!region || region->block_count <= 1) {
    return iree_ok_status();
  }
  iree_arena_initialize(module->arena.block_pool, &out_order->arena);
  iree_status_t status =
      loom_print_block_order_build(module, region, out_order);
  if (!iree_status_is_ok(status)) {
    loom_print_block_order_deinitialize(out_order);
  }
  return status;
}

void loom_print_block_order_deinitialize(loom_print_block_order_t* order) {
  iree_arena_deinitialize(&order->arena);
  memset(order, 0, sizeof(*order));
}
