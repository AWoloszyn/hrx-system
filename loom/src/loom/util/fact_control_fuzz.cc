// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Fuzzes CFG postdominance, compressed control paths, and incremental execution
// scopes against exhaustive test-only oracles. Cycles, unreachable blocks,
// duplicate edges, nontermination, and nonmonotone scope updates are all valid.
//
// Bytes encode: block count (1..24), then each block's successor count (0..4)
// and target ordinals. Remaining byte pairs select a block and a scope/update
// action. Scope is action % 5; bit 0 of action / 5 settles, and bit 1
// publishes. Missing structural bytes are zero. At most 256 updates bound
// oracle work.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "loom/util/cfg_control_test_util.h"
#include "loom/util/cfg_graph_test_util.h"

static void check(iree_status_t status) {
  if (!iree_status_is_ok(status)) {
    iree_status_abort(status);
  }
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  size_t position = 0;
  auto next = [&]() -> uint8_t {
    return position < size ? data[position++] : 0;
  };
  const uint16_t count = 1 + next() % 24;
  std::vector<std::vector<uint16_t>> successors(count);
  for (auto& edges : successors) {
    const uint8_t edge_count = next() % 5;
    for (uint8_t i = 0; i < edge_count; ++i) {
      edges.push_back(next() % count);
    }
  }
  loom::testing::CfgGraph graph(successors);
  loom::testing::CfgControlOracle oracle(graph.get());
  iree_arena_block_pool_t pool;
  iree_arena_block_pool_initialize(4096, iree_allocator_system(), &pool);
  iree_arena_allocator_t arena;
  iree_arena_initialize(&pool, &arena);
  loom_cfg_control_t structure;
  check(loom_cfg_control_build(graph.get(), &arena, &structure));
  check(oracle.CheckStructure(structure));
  loom_value_fact_control_t control;
  check(loom_value_fact_control_initialize(&structure, &arena, &control));
  const size_t allocation_size = arena.used_allocation_size;
  std::vector<uint8_t> selectors(count, 4);
  auto published = oracle.Solve(selectors);
  check(oracle.CheckFacts(&control, selectors));
  for (uint32_t update = 0; update < 256 && position < size; ++update) {
    const uint16_t block = next() % count;
    const uint8_t action = next();
    selectors[block] = action % 5;
    loom_value_fact_control_set_selector(
        &control, block, loom::testing::ControlDistribution(selectors[block]));
    if ((action / 5) & 3) {
      loom_value_fact_control_settle(&control);
      check(oracle.CheckFacts(&control, selectors));
      if ((action / 5) & 2) {
        check(oracle.CheckPublication(&control, selectors, &published));
      }
    }
  }
  loom_value_fact_control_settle(&control);
  check(oracle.CheckFacts(&control, selectors));
  check(oracle.CheckPublication(&control, selectors, &published));
  if (arena.used_allocation_size != allocation_size) {
    check(iree_make_status(IREE_STATUS_INTERNAL, "control update allocated"));
  }
  iree_arena_deinitialize(&arena);
  iree_arena_block_pool_deinitialize(&pool);
  return 0;
}
