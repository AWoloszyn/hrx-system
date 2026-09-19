// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_FORMAT_TEXT_PRINTER_BLOCK_ORDER_H_
#define LOOM_FORMAT_TEXT_PRINTER_BLOCK_ORDER_H_

#include "iree/base/internal/arena.h"
#include "loom/ir/ir.h"

#ifdef __cplusplus
extern "C" {
#endif

// Print-scoped block order. Each reachable block follows all its dominators;
// unrelated blocks retain their relative order unless an earlier block needs
// a later dominator. Unreachable blocks follow in their original order. The
// module's physical block table and successor identities are not changed.
typedef struct loom_print_block_order_t {
  // Original block index per printed position, or NULL for identity order.
  const uint16_t* indices;
  // Owns the order and its CFG analysis. Empty for single-block regions.
  iree_arena_allocator_t arena;
} loom_print_block_order_t;

// Establishes canonical text order once for a region. Structurally malformed
// CFGs retain physical order so diagnostic printing can describe their errors.
iree_status_t loom_print_block_order_initialize(
    const loom_module_t* module, const loom_region_t* region,
    loom_print_block_order_t* out_order);

// Releases a print order, including an empty or failed initialization.
void loom_print_block_order_deinitialize(loom_print_block_order_t* order);

static inline uint16_t loom_print_block_order_index(
    const loom_print_block_order_t* order, uint16_t position) {
  return order->indices ? order->indices[position] : position;
}

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_FORMAT_TEXT_PRINTER_BLOCK_ORDER_H_
