// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/util/cfg_loop_nest.h"

#include <algorithm>
#include <random>
#include <vector>

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/util/cfg_graph_test_util.h"

namespace loom {
namespace {

using testing::CfgGraph;

class CfgLoopNestTest : public ::testing::Test {
 protected:
  void SetUp() override {
    iree_arena_block_pool_initialize(4096, iree_allocator_system(), &pool_);
    iree_arena_initialize(&pool_, &arena_);
  }
  void TearDown() override {
    iree_arena_deinitialize(&arena_);
    iree_arena_block_pool_deinitialize(&pool_);
  }

  loom_cfg_loop_nest_t Build(const CfgGraph& fixture) {
    iree_arena_reset(&arena_);
    loom_cfg_loop_nest_t nest;
    IREE_CHECK_OK(loom_cfg_loop_nest_build(fixture.get(), &arena_, &nest));
    return nest;
  }

  // Independent small-graph oracle: remove each possible header to determine
  // dominance, then close each backedge over predecessors. Explicit sets make
  // this deliberately different from contraction and boundary cancellation.
  void CheckOracle(const std::vector<std::vector<uint16_t>>& successors) {
    CfgGraph fixture(successors);
    const auto* graph = fixture.get();
    const auto nest = Build(fixture);
    size_t expected_count = 0;
    std::vector<std::vector<bool>> membership;
    std::vector<uint16_t> headers;
    std::vector<bool> natural_backedges(graph->edge_count);
    for (uint16_t header = 0; header < successors.size(); ++header) {
      if (!graph->blocks[header].reachable) {
        continue;
      }
      std::vector<bool> reached(successors.size());
      std::vector<uint16_t> pending{0};
      while (!pending.empty()) {
        uint16_t block = pending.back();
        pending.pop_back();
        if (block == header || reached[block]) {
          continue;
        }
        reached[block] = true;
        for (uint16_t target : successors[block]) {
          pending.push_back(target);
        }
      }
      std::vector<uint32_t> entries;
      std::vector<uint32_t> backedges;
      auto predecessors = loom_cfg_graph_predecessor_edges(graph, header);
      for (size_t i = 0; i < predecessors.count; ++i) {
        uint32_t edge = predecessors.values[i];
        uint16_t source = graph->edges[edge].source_block_index;
        if (!graph->blocks[source].reachable) {
          continue;
        }
        if (!reached[source]) {
          backedges.push_back(edge);
          natural_backedges[edge] = true;
        } else {
          entries.push_back(edge);
        }
      }
      if (backedges.empty()) {
        continue;
      }
      ++expected_count;
      uint16_t loop_index = loom_cfg_loop_nest_innermost(&nest, header);
      ASSERT_NE(loop_index, LOOM_CFG_LOOP_NEST_NONE);
      const auto& loop = nest.loops[loop_index];
      EXPECT_EQ(loop.header_index, header);
      std::vector<bool> members(successors.size());
      members[header] = true;
      for (uint32_t edge : backedges) {
        pending.push_back(graph->edges[edge].source_block_index);
      }
      while (!pending.empty()) {
        uint16_t block = pending.back();
        pending.pop_back();
        if (members[block] || !graph->blocks[block].reachable) {
          continue;
        }
        members[block] = true;
        auto incoming = loom_cfg_graph_predecessors(graph, block);
        for (size_t i = 0; i < incoming.count; ++i) {
          pending.push_back(incoming.values[i]);
        }
      }
      std::vector<uint32_t> exits;
      for (size_t i = 0; i < graph->edge_count; ++i) {
        const auto& edge = graph->edges[i];
        if (members[edge.source_block_index] &&
            !members[edge.target_block_index]) {
          exits.push_back(i);
        }
      }
      auto check_edges = [](const loom_cfg_loop_edge_summary_t& summary,
                            const std::vector<uint32_t>& edges) {
        EXPECT_EQ(summary.count, edges.size());
        EXPECT_EQ(summary.unique_index,
                  edges.size() == 1 ? edges[0] : LOOM_CFG_EDGE_INDEX_INVALID);
      };
      check_edges(loop.entries, entries);
      check_edges(loop.backedges, backedges);
      check_edges(loop.exits, exits);
      for (size_t block = 0; block < members.size(); ++block) {
        EXPECT_EQ(loom_cfg_loop_nest_contains(&nest, loop_index, block),
                  members[block])
            << "header " << header << ", block " << block;
      }
      headers.push_back(header);
      membership.push_back(std::move(members));
    }
    EXPECT_EQ(nest.loop_count, expected_count);
    // Removing all natural backedges leaves a DAG exactly when the reachable
    // graph is reducible. Topological elimination avoids the producer's DFS
    // ancestor classification entirely.
    std::vector<size_t> indegree(successors.size());
    for (size_t i = 0; i < graph->edge_count; ++i) {
      const auto& edge = graph->edges[i];
      if (graph->blocks[edge.source_block_index].reachable &&
          !natural_backedges[i]) {
        ++indegree[edge.target_block_index];
      }
    }
    std::vector<uint16_t> ready;
    size_t reachable_count = 0;
    for (uint16_t block = 0; block < successors.size(); ++block) {
      if (!graph->blocks[block].reachable) {
        continue;
      }
      ++reachable_count;
      if (indegree[block] == 0) {
        ready.push_back(block);
      }
    }
    for (size_t i = 0; i < ready.size(); ++i) {
      auto outgoing = loom_cfg_graph_successor_edges(graph, ready[i]);
      for (size_t j = 0; j < outgoing.count; ++j) {
        uint32_t edge = outgoing.values[j];
        uint16_t target = graph->edges[edge].target_block_index;
        if (!natural_backedges[edge] && --indegree[target] == 0) {
          ready.push_back(target);
        }
      }
    }
    EXPECT_EQ(nest.reducible, ready.size() == reachable_count);
    // Check immediate parents and innermost membership, not just ancestor
    // containment: skipped levels could otherwise hide a boundary error.
    for (size_t block = 0; block < successors.size(); ++block) {
      uint16_t expected = LOOM_CFG_LOOP_NEST_NONE;
      size_t smallest = successors.size() + 1;
      for (size_t i = 0; i < membership.size(); ++i) {
        size_t size =
            std::count(membership[i].begin(), membership[i].end(), true);
        if (membership[i][block] && size < smallest) {
          expected = loom_cfg_loop_nest_innermost(&nest, headers[i]);
          smallest = size;
        }
      }
      EXPECT_EQ(loom_cfg_loop_nest_innermost(&nest, block), expected);
    }
    for (size_t i = 0; i < membership.size(); ++i) {
      uint16_t expected = LOOM_CFG_LOOP_NEST_NONE;
      size_t smallest = successors.size() + 1;
      for (size_t j = 0; j < membership.size(); ++j) {
        if (i == j || !membership[j][headers[i]]) {
          continue;
        }
        size_t size =
            std::count(membership[j].begin(), membership[j].end(), true);
        if (size < smallest) {
          expected = loom_cfg_loop_nest_innermost(&nest, headers[j]);
          smallest = size;
        }
      }
      EXPECT_EQ(nest.loops[loom_cfg_loop_nest_innermost(&nest, headers[i])]
                    .parent_loop_index,
                expected);
    }
  }

  // Pool shared by successive independent immutable snapshots.
  iree_arena_block_pool_t pool_;
  // Retained storage for the active test snapshot.
  iree_arena_allocator_t arena_;
};

TEST_F(CfgLoopNestTest, EmptyAcyclicAndUnreachableCycles) {
  CheckOracle({});
  CheckOracle({{}});
  CheckOracle({{3}, {}, {1}, {2}});
  CheckOracle({{1}, {}, {2}});
}

TEST_F(CfgLoopNestTest, DelayedGuardAndReorderedBlocks) {
  // Entry -> header diamond -> guard -> latch -> header; guard also exits.
  CheckOracle({{1}, {2, 3}, {4}, {4}, {5, 6}, {1}, {}});
  CheckOracle({{4}, {3}, {}, {6, 2}, {1, 5}, {3}, {4}});
}

TEST_F(CfgLoopNestTest, ParallelEdgesMultipleEntriesLatchesAndExits) {
  CheckOracle({{1, 1}, {2, 3}, {1}, {1, 4}, {}});
  CheckOracle({{1}, {2, 5}, {3, 4}, {1, 5}, {1, 6}, {}, {}});
  CheckOracle({{1}, {1, 2}, {}});
  CheckOracle({{0}});
}

TEST_F(CfgLoopNestTest, NestedSiblingAndSharedExits) {
  CheckOracle({{1}, {2, 6}, {3, 4}, {2}, {5}, {1}, {7}, {7, 8}, {}});
  CheckOracle({{1}, {2}, {3, 6}, {2, 4}, {1, 5}, {6}, {}});
}

TEST_F(CfgLoopNestTest, IrreducibleCyclesRetainNaturalSubloops) {
  CheckOracle({{1, 2}, {2, 3}, {1, 3}, {}});
  CheckOracle({{1, 2}, {3}, {3}, {4, 5}, {3, 1}, {2}});
}

TEST_F(CfgLoopNestTest, EveryThreeBlockGraph) {
  for (uint32_t bits = 0; bits < (1u << 9); ++bits) {
    SCOPED_TRACE(bits);
    std::vector<std::vector<uint16_t>> successors(3);
    for (uint16_t source = 0; source < 3; ++source) {
      for (uint16_t target = 0; target < 3; ++target) {
        if (bits & (1u << (source * 3 + target))) {
          successors[source].push_back(target);
        }
      }
    }
    CheckOracle(successors);
  }
}

TEST_F(CfgLoopNestTest, DeterministicArbitraryGraphs) {
  std::mt19937 random(715321);
  for (size_t sample = 0; sample < 2000; ++sample) {
    SCOPED_TRACE(sample);
    uint16_t count = 2 + random() % 15;
    std::vector<std::vector<uint16_t>> successors(count);
    for (auto& outgoing : successors) {
      size_t edges = random() % 5;
      for (size_t i = 0; i < edges; ++i) {
        outgoing.push_back(random() % count);
      }
    }
    CheckOracle(successors);
  }
}

TEST_F(CfgLoopNestTest, DeepNestingUsesCompactMembership) {
  constexpr uint16_t depth = 8192;
  std::vector<std::vector<uint16_t>> successors(depth * 2 + 2);
  successors[0] = {1};
  for (uint16_t i = 0; i < depth; ++i) {
    uint16_t header = i + 1;
    uint16_t latch = depth + i + 1;
    successors[header] = {uint16_t(i + 1 < depth ? header + 1 : latch),
                          uint16_t(i ? depth + i : successors.size() - 1)};
    successors[latch] = {header};
  }
  CfgGraph fixture(successors);
  const auto nest = Build(fixture);
  ASSERT_TRUE(nest.reducible);
  ASSERT_EQ(nest.loop_count, depth);
  for (uint16_t i = 0; i < depth; ++i) {
    uint16_t loop_index = loom_cfg_loop_nest_innermost(&nest, i + 1);
    const auto& loop = nest.loops[loop_index];
    EXPECT_EQ(loop.entries.count, 1u);
    EXPECT_EQ(loop.backedges.count, 1u);
    EXPECT_EQ(loop.exits.count, 1u);
    EXPECT_EQ(loop.parent_loop_index, i ? loom_cfg_loop_nest_innermost(&nest, i)
                                        : LOOM_CFG_LOOP_NEST_NONE);
    EXPECT_TRUE(loom_cfg_loop_nest_contains(&nest, loop_index, depth));
    EXPECT_FALSE(loom_cfg_loop_nest_contains(&nest, loop_index, 0));
    EXPECT_FALSE(loom_cfg_loop_nest_contains(&nest, loop_index, depth * 2 + 1));
  }
}

TEST_F(CfgLoopNestTest, NestedExecutionCountsDoNotDependOnBlockOrder) {
  // Headers 4 and 2, with latches 1 and 5. Block 7 is unreachable.
  CfgGraph fixture({{4}, {4}, {5, 1}, {}, {2, 6}, {2}, {3}, {7}});
  const auto nest = Build(fixture);
  ASSERT_EQ(nest.loop_count, 2u);
  std::vector<uint64_t> trips(2);
  trips[loom_cfg_loop_nest_innermost(&nest, 4)] = 4;
  trips[loom_cfg_loop_nest_innermost(&nest, 2)] = 8;
  std::vector<uint64_t> counts(8);
  EXPECT_TRUE(loom_cfg_loop_nest_calculate_block_execution_counts(
      &nest, trips.data(), counts.data()));
  EXPECT_EQ(counts, (std::vector<uint64_t>{1, 4, 36, 1, 5, 32, 1, 0}));
  trips[loom_cfg_loop_nest_innermost(&nest, 4)] = 0;
  EXPECT_TRUE(loom_cfg_loop_nest_calculate_block_execution_counts(
      &nest, trips.data(), counts.data()));
  EXPECT_EQ(counts, (std::vector<uint64_t>{1, 0, 0, 1, 1, 0, 1, 0}));
  trips[loom_cfg_loop_nest_innermost(&nest, 4)] = UINT64_MAX;
  EXPECT_FALSE(loom_cfg_loop_nest_calculate_block_execution_counts(
      &nest, trips.data(), counts.data()));
}

TEST_F(CfgLoopNestTest, UnmodeledPathsDoNotProduceExactCounts) {
  const std::vector<std::vector<std::vector<uint16_t>>> cases = {
      {{1}, {2, 3}, {4}, {4}, {5, 6}, {1}, {}},
      {{1}, {2, 5}, {3, 4}, {1}, {1}, {}},
      {{1}, {2, 4}, {1, 4}, {}, {}},
      {{1, 2}, {2, 3}, {1, 3}, {}},
      {{0}},
  };
  for (const auto& successors : cases) {
    CfgGraph fixture(successors);
    const auto nest = Build(fixture);
    std::vector<uint64_t> trips(nest.loop_count, 4);
    std::vector<uint64_t> counts(successors.size());
    EXPECT_FALSE(loom_cfg_loop_nest_calculate_block_execution_counts(
        &nest, trips.data(), counts.data()));
  }
}

}  // namespace
}  // namespace loom
