// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/util/cfg_regions.h"

#include <algorithm>
#include <random>
#include <set>
#include <vector>

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/util/cfg_graph_test_util.h"

namespace loom {
namespace {

using testing::CfgGraph;

class CfgRegionsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    iree_arena_block_pool_initialize(4096, iree_allocator_system(), &pool_);
    iree_arena_initialize(&pool_, &arena_);
  }
  void TearDown() override {
    iree_arena_deinitialize(&arena_);
    iree_arena_block_pool_deinitialize(&pool_);
  }

  loom_cfg_regions_t Build(const CfgGraph& fixture) {
    iree_arena_reset(&arena_);
    IREE_CHECK_OK(
        loom_cfg_dominance_build(fixture.get(), &arena_, &dominance_));
    loom_cfg_regions_t regions;
    IREE_CHECK_OK(
        loom_cfg_regions_build(fixture.get(), &dominance_, &arena_, &regions));
    return regions;
  }

  // Vertex removal establishes domination without consulting the production
  // tree or its intervals. Enumerating that set's outgoing edges then gives
  // an independent continuation and exact edge-identity oracle.
  void CheckOracle(const std::vector<std::vector<uint16_t>>& successors) {
    CfgGraph fixture(successors);
    auto regions = Build(fixture);
    auto* graph = fixture.get();
    auto reachable = [&](uint16_t removed) {
      std::vector<bool> reached(successors.size(), false);
      std::vector<uint16_t> pending;
      if (!successors.empty() && removed != 0) {
        pending.push_back(0);
      }
      while (!pending.empty()) {
        uint16_t block = pending.back();
        pending.pop_back();
        if (block == removed || reached[block]) {
          continue;
        }
        reached[block] = true;
        pending.insert(pending.end(), successors[block].begin(),
                       successors[block].end());
      }
      return reached;
    };
    auto live = reachable(UINT16_MAX);
    for (uint16_t entry = 0; entry < successors.size(); ++entry) {
      SCOPED_TRACE(entry);
      auto without_entry = reachable(entry);
      std::vector<bool> members(successors.size(), false);
      std::set<uint32_t> destinations;
      size_t return_count = 0;
      for (size_t block = 0; block < successors.size(); ++block) {
        members[block] = live[block] && !without_entry[block];
        if (members[block] && successors[block].empty()) {
          destinations.insert(UINT16_MAX);
          ++return_count;
        }
      }
      std::vector<loom_cfg_edge_index_t> exits;
      for (uint32_t edge = 0; edge < graph->edge_count; ++edge) {
        const auto& info = graph->edges[edge];
        if (members[info.source_block_index] &&
            !members[info.target_block_index]) {
          destinations.insert(info.target_block_index);
          exits.push_back(edge);
        }
      }
      const auto& region = regions.blocks[entry];
      EXPECT_EQ(region.exit_count, exits.size() + return_count);
      for (uint16_t destination = 0; destination < successors.size();
           ++destination) {
        std::vector<uint32_t> expected;
        for (uint32_t edge = 0; edge < graph->edge_count; ++edge) {
          const auto& info = graph->edges[edge];
          if (members[info.source_block_index] &&
              info.target_block_index == destination) {
            expected.push_back(edge);
          }
        }
        auto span = loom_cfg_regions_edges_to(graph, &dominance_, &regions,
                                              entry, destination);
        std::vector<uint32_t> actual;
        if (span.count) {
          actual.assign(span.values, span.values + span.count);
        }
        std::sort(actual.begin(), actual.end());
        EXPECT_EQ(actual, expected);
      }
      if (destinations.size() != 1 || *destinations.begin() == UINT16_MAX) {
        EXPECT_EQ(region.continuation_index, LOOM_CFG_REGION_CONTINUATION_NONE);
        EXPECT_EQ(region.exit_edges.count, 0u);
        continue;
      }
      EXPECT_EQ(region.continuation_index, *destinations.begin());
      std::vector<loom_cfg_edge_index_t> actual(
          region.exit_edges.values,
          region.exit_edges.values + region.exit_edges.count);
      std::sort(actual.begin(), actual.end());
      EXPECT_EQ(actual, exits);
    }
  }

  // Allocation pool reused across independent CFG snapshots.
  iree_arena_block_pool_t pool_;
  // Storage retaining the current graph analysis result.
  iree_arena_allocator_t arena_;
  // Dominance snapshot sharing arena_ with its derived region index.
  loom_cfg_dominance_t dominance_;
};

TEST_F(CfgRegionsTest, EmptyUnreachableAndReturningRegions) {
  CheckOracle({});
  CheckOracle({{}});
  CheckOracle({{1, 2}, {}, {}});
  CheckOracle({{1}, {}, {1}});
  CheckOracle({{0}});
}

TEST_F(CfgRegionsTest, SharedTailsAndParallelExits) {
  CheckOracle({{1, 2}, {3, 2}, {4}, {4}, {}});
  CheckOracle({{1, 2}, {3}, {3, 4}, {4}, {}});
  CheckOracle({{1, 2}, {3, 3}, {3}, {}});
  CheckOracle({{4, 1}, {2}, {3}, {}, {2}});
}

TEST_F(CfgRegionsTest, NestedLoopsAndIrreducibleControl) {
  CheckOracle({{1, 6}, {2, 5}, {3, 4}, {2}, {1}, {6}, {}});
  CheckOracle({{1, 2}, {2, 3}, {1, 3}, {}});
  CheckOracle({{1, 2}, {1}, {3}, {}});
}

TEST_F(CfgRegionsTest, EveryFourBlockGraph) {
  for (uint32_t bits = 0; bits < (1u << 16); ++bits) {
    SCOPED_TRACE(bits);
    std::vector<std::vector<uint16_t>> successors(4);
    for (uint16_t source = 0; source < 4; ++source) {
      for (uint16_t target = 0; target < 4; ++target) {
        if (bits & (1u << (source * 4 + target))) {
          successors[source].push_back(target);
        }
      }
    }
    CheckOracle(successors);
  }
}

TEST_F(CfgRegionsTest, DeterministicArbitraryGraphs) {
  std::mt19937 random(634);
  for (size_t sample = 0; sample < 2000; ++sample) {
    SCOPED_TRACE(sample);
    uint16_t count = 2 + random() % 31;
    std::vector<std::vector<uint16_t>> successors(count);
    for (auto& outgoing : successors) {
      size_t edge_count = random() % 5;
      for (size_t i = 0; i < edge_count; ++i) {
        outgoing.push_back(random() % count);
      }
    }
    CheckOracle(successors);
  }
}

TEST_F(CfgRegionsTest, DeepDominanceWithoutInclusiveRegionStorage) {
  constexpr uint16_t count = 12001;
  std::vector<std::vector<uint16_t>> successors(count);
  for (uint16_t block = 0; block + 3 < count; block += 3) {
    successors[block] = {uint16_t(block + 1), uint16_t(block + 2)};
    successors[block + 1] = {uint16_t(block + 3)};
    successors[block + 2] = {uint16_t(block + 3)};
  }
  CfgGraph fixture(successors);
  auto regions = Build(fixture);
  for (uint16_t block = 0; block + 3 < count; block += 3) {
    EXPECT_EQ(regions.blocks[block].continuation_index,
              LOOM_CFG_REGION_CONTINUATION_NONE);
    EXPECT_EQ(regions.blocks[block + 1].continuation_index, block + 3);
    EXPECT_EQ(regions.blocks[block + 2].continuation_index, block + 3);
    EXPECT_EQ(regions.blocks[block + 1].exit_edges.count, 1u);
    EXPECT_EQ(regions.blocks[block + 2].exit_edges.count, 1u);
  }
}

}  // namespace
}  // namespace loom
