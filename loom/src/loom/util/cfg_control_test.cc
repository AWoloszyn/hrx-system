// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/util/cfg_control.h"

#include <random>
#include <vector>

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/util/cfg_control_test_util.h"
#include "loom/util/cfg_graph_test_util.h"

namespace loom {
namespace {

class CfgControlTest : public ::testing::Test {
 protected:
  void SetUp() override {
    iree_arena_block_pool_initialize(4096, iree_allocator_system(), &pool_);
    iree_arena_initialize(&pool_, &arena_);
  }
  void TearDown() override {
    iree_arena_deinitialize(&arena_);
    iree_arena_block_pool_deinitialize(&pool_);
  }

  void CheckPaths(const std::vector<std::vector<uint16_t>>& successors) {
    testing::CfgGraph fixture(successors);
    const auto* graph = fixture.get();
    loom_cfg_control_t control;
    IREE_ASSERT_OK(loom_cfg_control_build(graph, &arena_, &control));
    testing::CfgControlOracle oracle(graph);
    IREE_ASSERT_OK(oracle.CheckStructure(control));
    iree_arena_reset(&arena_);
  }

  // Reusable backing storage for analysis allocations.
  iree_arena_block_pool_t pool_;
  // One result lifetime, reset between graph cases.
  iree_arena_allocator_t arena_;
};

TEST_F(CfgControlTest, EmptyLinearDuplicateAndUnreachableGraphs) {
  CheckPaths({});
  CheckPaths({{}});
  CheckPaths({{1}, {2}, {}});
  CheckPaths({{1, 1}, {1}});
  CheckPaths({{}, {2, 3}, {1}, {}});
}

TEST_F(CfgControlTest, ExhaustiveThreeBlockGraphs) {
  for (uint32_t mask = 0; mask < 512; ++mask) {
    SCOPED_TRACE(mask);
    std::vector<std::vector<uint16_t>> successors(3);
    for (uint16_t source = 0; source < 3; ++source) {
      for (uint16_t target = 0; target < 3; ++target) {
        if (mask & (1u << (source * 3 + target))) {
          successors[source].push_back(target);
        }
      }
    }
    CheckPaths(successors);
  }
}

TEST_F(CfgControlTest, RandomCyclesMultipleExitsAndReorderedBlocks) {
  std::mt19937 random(541213);
  for (uint32_t trial = 0; trial < 4000; ++trial) {
    SCOPED_TRACE(trial);
    const uint32_t count = 1 + random() % 24;
    std::vector<std::vector<uint16_t>> successors(count);
    for (auto& edges : successors) {
      uint32_t edge_count = random() % 5;
      while (edge_count--) {
        edges.push_back(random() % count);
      }
    }
    CheckPaths(successors);
  }
}

TEST_F(CfgControlTest, SharedTailDoesNotMaterializeQuadraticControlRelation) {
  for (uint32_t width = 8; width <= 4096; width *= 2) {
    SCOPED_TRACE(width);
    std::vector<std::vector<uint16_t>> successors(2 * width + 1);
    const uint16_t exit = 2 * width;
    for (uint32_t i = 0; i < width; ++i) {
      successors[i] = {static_cast<uint16_t>(width),
                       i + 1 < width ? static_cast<uint16_t>(i + 1) : exit};
      successors[width + i] = {static_cast<uint16_t>(width + i + 1)};
    }
    testing::CfgGraph fixture(successors);
    loom_cfg_control_t control;
    IREE_ASSERT_OK(loom_cfg_control_build(fixture.get(), &arena_, &control));
    EXPECT_LE(control.node_count, 2 * successors.size() + 1);
    uint32_t logarithm = 0;
    for (uint32_t n = width; n > 1; n /= 2) {
      ++logarithm;
    }
    EXPECT_LE(control.binding_count, width * (logarithm + 2));
    iree_arena_reset(&arena_);
  }
}

TEST_F(CfgControlTest, FullBlockIndexSpaceWithDeepControl) {
  std::vector<std::vector<uint16_t>> successors(65535);
  successors[0] = {65534, 1};
  for (uint32_t i = 2; i < successors.size(); ++i) {
    successors[i] = {static_cast<uint16_t>(i - 1), 1};
  }
  testing::CfgGraph fixture(successors);
  loom_cfg_control_t control;
  IREE_ASSERT_OK(loom_cfg_control_build(fixture.get(), &arena_, &control));
  EXPECT_TRUE(control.available);
  EXPECT_LE(control.node_count, 2 * successors.size() + 1);
}

}  // namespace
}  // namespace loom
