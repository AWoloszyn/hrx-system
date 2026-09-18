// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/util/fact_control.h"

#include <random>
#include <vector>

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/util/cfg_control_test_util.h"
#include "loom/util/cfg_graph_test_util.h"

namespace loom {
namespace {

using testing::ControlDistribution;

class FactControlTest : public ::testing::Test {
 protected:
  void SetUp() override {
    iree_arena_block_pool_initialize(4096, iree_allocator_system(), &pool_);
    iree_arena_initialize(&pool_, &arena_);
  }
  void TearDown() override {
    iree_arena_deinitialize(&arena_);
    iree_arena_block_pool_deinitialize(&pool_);
  }

  void CheckUpdates(const std::vector<std::vector<uint16_t>>& successors,
                    std::mt19937& random) {
    testing::CfgGraph fixture(successors);
    loom_cfg_control_t structure;
    IREE_ASSERT_OK(loom_cfg_control_build(fixture.get(), &arena_, &structure));
    loom_value_fact_control_t control;
    IREE_ASSERT_OK(
        loom_value_fact_control_initialize(&structure, &arena_, &control));
    const size_t allocation_size = arena_.used_allocation_size;
    std::vector<uint8_t> selectors(successors.size(), 4);
    testing::CfgControlOracle oracle(fixture.get());
    IREE_ASSERT_OK(oracle.CheckStructure(structure));
    auto previous = oracle.Solve(selectors);
    for (uint32_t batch = 0; batch < 30; ++batch) {
      SCOPED_TRACE(batch);
      const uint32_t count = 1 + random() % successors.size();
      for (uint32_t i = 0; i < count; ++i) {
        const uint32_t block = random() % successors.size();
        selectors[block] = random() % 5;
        loom_value_fact_control_set_selector(
            &control, block, ControlDistribution(selectors[block]));
      }
      loom_value_fact_control_settle(&control);
      IREE_ASSERT_OK(oracle.CheckFacts(&control, selectors));
      IREE_ASSERT_OK(oracle.CheckPublication(&control, selectors, &previous));
      EXPECT_EQ(arena_.used_allocation_size, allocation_size);
    }
    iree_arena_reset(&arena_);
  }

  // Reusable backing storage for the graph and mutable solver.
  iree_arena_block_pool_t pool_;
  // Analysis lifetime; mutation must not increase its allocation count.
  iree_arena_allocator_t arena_;
};

TEST_F(FactControlTest, NestedScopesStrengthenAndWeakenAcrossReconvergence) {
  std::mt19937 random(763234);
  CheckUpdates({{1, 6}, {2, 3}, {4}, {4}, {5}, {6}, {}}, random);
  CheckUpdates({{1, 2}, {3}, {3}, {4, 5}, {}, {}}, random);
  CheckUpdates({{3, 4}, {5}, {5}, {1, 2}, {5}, {}}, random);
}

TEST_F(FactControlTest, CyclesIrreducibilityAndUnreachableSelectors) {
  std::mt19937 random(54323);
  CheckUpdates({{1}, {2, 3}, {1}, {}}, random);
  CheckUpdates({{1, 2}, {2, 3}, {1, 3}, {}}, random);
  CheckUpdates({{1, 2}, {3, 4}, {4, 5}, {1}, {2, 5}, {}, {0, 6}}, random);
  CheckUpdates({{1, 2}, {1}, {2}}, random);
}

TEST_F(FactControlTest, ExhaustiveThreeBlockGraphsWithNonmonotoneUpdates) {
  std::mt19937 random(738552);
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
    CheckUpdates(successors, random);
  }
}

TEST_F(FactControlTest, RandomGraphsAndMixedScopeUpdates) {
  std::mt19937 random(996137);
  for (uint32_t trial = 0; trial < 3000; ++trial) {
    SCOPED_TRACE(trial);
    const uint32_t count = 1 + random() % 20;
    std::vector<std::vector<uint16_t>> successors(count);
    for (auto& edges : successors) {
      uint32_t edge_count = random() % 5;
      while (edge_count--) {
        edges.push_back(random() % count);
      }
    }
    CheckUpdates(successors, random);
  }
}

TEST_F(FactControlTest, TransientResetDoesNotPublishAChange) {
  testing::CfgGraph fixture({{1, 2}, {3}, {3}, {}});
  loom_cfg_control_t structure;
  IREE_ASSERT_OK(loom_cfg_control_build(fixture.get(), &arena_, &structure));
  loom_value_fact_control_t control;
  IREE_ASSERT_OK(
      loom_value_fact_control_initialize(&structure, &arena_, &control));
  for (uint8_t initial = 0; initial < 5; ++initial) {
    loom_value_fact_control_set_selector(&control, 0,
                                         ControlDistribution(initial));
    loom_value_fact_control_settle(&control);
    uint16_t block = 0;
    while (loom_value_fact_control_take_changed_block(&control, &block)) {
    }
    loom_value_fact_control_set_selector(&control, 0, ControlDistribution(4));
    loom_value_fact_control_settle(&control);
    loom_value_fact_control_set_selector(&control, 0,
                                         ControlDistribution(initial));
    loom_value_fact_control_settle(&control);
    EXPECT_FALSE(loom_value_fact_control_take_changed_block(&control, &block));
  }
}

TEST_F(FactControlTest,
       EqualMinimumKeepsItsWitnessUntilThatSelectorStrengthens) {
  testing::CfgGraph fixture({{1, 4}, {2, 4}, {3, 4}, {4}, {}});
  loom_cfg_control_t structure;
  IREE_ASSERT_OK(loom_cfg_control_build(fixture.get(), &arena_, &structure));
  loom_value_fact_control_t control;
  IREE_ASSERT_OK(
      loom_value_fact_control_initialize(&structure, &arena_, &control));
  loom_value_fact_control_set_selector(&control, 0, ControlDistribution(1));
  loom_value_fact_control_settle(&control);
  loom_value_fact_control_prepare_diagnostics(&control);
  const auto original = loom_value_fact_control_controller(&control, 3);
  ASSERT_NE(original, LOOM_CFG_EDGE_INDEX_INVALID);
  EXPECT_EQ(fixture.get()->edges[original].source_block_index, 0);
  for (uint16_t block : {1, 2}) {
    loom_value_fact_control_set_selector(&control, block,
                                         ControlDistribution(1));
    loom_value_fact_control_settle(&control);
    loom_value_fact_control_prepare_diagnostics(&control);
    EXPECT_EQ(loom_value_fact_control_controller(&control, 3), original);
  }
  loom_value_fact_control_set_selector(&control, 0, ControlDistribution(4));
  loom_value_fact_control_settle(&control);
  loom_value_fact_control_prepare_diagnostics(&control);
  auto replacement = loom_value_fact_control_controller(&control, 3);
  ASSERT_NE(replacement, LOOM_CFG_EDGE_INDEX_INVALID);
  EXPECT_NE(fixture.get()->edges[replacement].source_block_index, 0);
  EXPECT_EQ(loom_value_fact_control_execution(&control, 3).flags,
            ControlDistribution(1).flags);
}

}  // namespace
}  // namespace loom
