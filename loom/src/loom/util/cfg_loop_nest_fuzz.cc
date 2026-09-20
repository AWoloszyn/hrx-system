// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Fuzzes natural-loop nesting and boundary summaries against the independent
// vertex-removal/predecessor-closure oracle. Bytes encode block count (1..32),
// then successor counts (0..4) and target ordinals. Missing bytes are zero.
// Unreachable blocks, parallel edges and irreducible cycles are valid inputs.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "loom/util/cfg_graph_test_util.h"
#include "loom/util/cfg_loop_nest_test_util.h"

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
  const uint16_t count = 1 + next() % 32;
  std::vector<std::vector<uint16_t>> successors(count);
  for (auto& edges : successors) {
    uint8_t edge_count = next() % 5;
    for (uint8_t i = 0; i < edge_count; ++i) {
      edges.push_back(next() % count);
    }
  }
  loom::testing::CfgGraph graph(successors);
  iree_arena_block_pool_t pool;
  iree_arena_block_pool_initialize(4096, iree_allocator_system(), &pool);
  iree_arena_allocator_t arena;
  iree_arena_initialize(&pool, &arena);
  loom_cfg_dominance_t dominance;
  check(loom_cfg_dominance_build(graph.get(), &arena, &dominance));
  loom_cfg_loop_nest_t nest;
  check(loom_cfg_loop_nest_build(graph.get(), &dominance, &arena, &nest));
  check(loom::testing::CheckLoopNest(nest));
  iree_arena_deinitialize(&arena);
  iree_arena_block_pool_deinitialize(&pool);
  return 0;
}
