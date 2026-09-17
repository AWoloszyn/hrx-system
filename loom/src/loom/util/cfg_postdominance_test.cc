// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/util/cfg_postdominance.h"

#include <utility>
#include <vector>

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace loom {
namespace {

// Graph-data fixture for the structural API. Production extraction and control
// consumers are also exercised by the collective-legality loom-test fixtures.
class Graph {
 public:
  explicit Graph(std::vector<std::vector<uint16_t>> successors)
      : blocks_(successors.size()) {
    std::vector<std::vector<uint16_t>> predecessors(blocks_.size());
    for (size_t i = 0; i < blocks_.size(); ++i) {
      blocks_[i].successor_start = successor_indices_.size();
      blocks_[i].successor_count = successors[i].size();
      for (uint16_t target : successors[i]) {
        successor_indices_.push_back(target);
        predecessors[target].push_back(i);
      }
    }
    for (size_t i = 0; i < blocks_.size(); ++i) {
      blocks_[i].predecessor_start = predecessor_indices_.size();
      blocks_[i].predecessor_count = predecessors[i].size();
      predecessor_indices_.insert(predecessor_indices_.end(),
                                  predecessors[i].begin(),
                                  predecessors[i].end());
    }
    if (!blocks_.empty()) {
      std::vector<uint16_t> pending{0};
      blocks_[0].reachable = true;
      while (!pending.empty()) {
        uint16_t source = pending.back();
        pending.pop_back();
        for (uint16_t target : successors[source]) {
          if (!blocks_[target].reachable) {
            blocks_[target].reachable = true;
            pending.push_back(target);
          }
        }
      }
    }
    graph_.blocks = blocks_.data();
    graph_.block_count = blocks_.size();
    graph_.edge_count = successor_indices_.size();
    graph_.successor_indices = successor_indices_.data();
    graph_.predecessor_indices = predecessor_indices_.data();
  }

  const loom_cfg_graph_t* get() const { return &graph_; }

 private:
  // Adjacency ranges and entry reachability consumed by the analysis.
  std::vector<loom_cfg_block_info_t> blocks_;
  // Dense outgoing adjacency.
  std::vector<uint16_t> successor_indices_;
  // Dense incoming adjacency.
  std::vector<uint16_t> predecessor_indices_;
  // Non-owning view of the fixture's retained arrays.
  loom_cfg_graph_t graph_ = {};
};

class CfgPostdominanceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    iree_arena_block_pool_initialize(4096, iree_allocator_system(), &pool_);
    iree_arena_initialize(&pool_, &arena_);
  }
  void TearDown() override {
    iree_arena_deinitialize(&arena_);
    iree_arena_block_pool_deinitialize(&pool_);
  }

  // Independent oracle: a postdominator is a node whose removal prevents an
  // exit path. It uses forward reachability for every candidate and source;
  // there is no reverse DFS, semidominator, or tree-intersection algorithm.
  void CheckAgainstReachability(const Graph& fixture) {
    const auto* graph = fixture.get();
    const size_t exit = graph->block_count;
    const size_t count = exit + 1;
    loom_cfg_postdominance_t postdominance;
    IREE_ASSERT_OK(
        loom_cfg_postdominance_build(graph, &arena_, &postdominance));
    ASSERT_TRUE(postdominance.available);
    ASSERT_EQ(postdominance.exit_node, exit);
    std::vector<std::vector<size_t>> edges(count);
    for (size_t source = 0; source < exit; ++source) {
      if (!graph->blocks[source].reachable) {
        continue;
      }
      auto successors = loom_cfg_graph_successors(graph, source);
      for (size_t i = 0; i < successors.count; ++i) {
        edges[source].push_back(successors.values[i]);
      }
      bool reaches_exit = false;
      std::vector<bool> visited(exit);
      std::vector<size_t> pending{source};
      while (!pending.empty()) {
        size_t current = pending.back();
        pending.pop_back();
        if (visited[current]) {
          continue;
        }
        visited[current] = true;
        auto next = loom_cfg_graph_successors(graph, current);
        reaches_exit |= next.count == 0;
        for (size_t i = 0; i < next.count; ++i) {
          pending.push_back(next.values[i]);
        }
      }
      if (successors.count == 0 || !reaches_exit) {
        edges[source].push_back(exit);
      }
    }
    std::vector<std::vector<bool>> expected(count, std::vector<bool>(count));
    std::vector<uint32_t> depth(count);
    for (size_t source = 0; source < count; ++source) {
      if (source < exit && !graph->blocks[source].reachable) {
        continue;
      }
      for (size_t removed = 0; removed < count; ++removed) {
        std::vector<bool> reachable(count);
        std::vector<size_t> pending{source};
        while (!pending.empty()) {
          size_t current = pending.back();
          pending.pop_back();
          if (current == removed || reachable[current]) {
            continue;
          }
          reachable[current] = true;
          pending.insert(pending.end(), edges[current].begin(),
                         edges[current].end());
        }
        expected[source][removed] = !reachable[exit];
        depth[source] += expected[source][removed];
      }
      // The node itself contributes one to the removal count.
      --depth[source];
    }
    for (size_t source = 0; source < count; ++source) {
      uint32_t parent = LOOM_CFG_POSTDOMINATOR_INVALID;
      if (source == exit) {
        parent = exit;
      } else if (graph->blocks[source].reachable) {
        for (uint32_t candidate = 0; candidate < count; ++candidate) {
          if (candidate == source || !expected[source][candidate]) {
            continue;
          }
          if (parent == LOOM_CFG_POSTDOMINATOR_INVALID ||
              depth[candidate] > depth[parent]) {
            parent = candidate;
          }
        }
      }
      EXPECT_EQ(postdominance.nodes[source].immediate_postdominator, parent)
          << "source " << source;
      EXPECT_EQ(postdominance.nodes[source].depth, depth[source])
          << "source " << source;
    }
    iree_arena_reset(&arena_);
  }

  // Reusable allocation pool for analysis invocations.
  iree_arena_block_pool_t pool_;
  // Invocation-owned analysis arrays.
  iree_arena_allocator_t arena_;
};

TEST_F(CfgPostdominanceTest, EmptySingleBlockAndSelfLoop) {
  CheckAgainstReachability(Graph({}));
  CheckAgainstReachability(Graph({{}}));
  CheckAgainstReachability(Graph({{0}}));
}

TEST_F(CfgPostdominanceTest, ReverseChainAndDiamond) {
  CheckAgainstReachability(Graph({{3}, {}, {1}, {2}}));
  CheckAgainstReachability(Graph({{1, 2}, {3}, {3}, {}}));
}

TEST_F(CfgPostdominanceTest, MultipleExitsAndPartialNontermination) {
  CheckAgainstReachability(Graph({{1, 2}, {}, {3, 4}, {}, {}}));
  CheckAgainstReachability(Graph({{1, 2}, {1}, {}}));
  CheckAgainstReachability(Graph({{1, 2}, {2}, {1}}));
}

TEST_F(CfgPostdominanceTest, LoopsDuplicateEdgesAndUnreachablePredecessors) {
  CheckAgainstReachability(Graph({{1, 1}, {2, 3}, {1}, {}, {3, 4}}));
  CheckAgainstReachability(Graph({{1, 2}, {2, 3}, {1, 3}, {}}));
  CheckAgainstReachability(Graph({{}, {2}, {1}}));
}

TEST_F(CfgPostdominanceTest, EveryThreeBlockGraph) {
  for (uint32_t mask = 0; mask < 512; ++mask) {
    SCOPED_TRACE(mask);
    std::vector<std::vector<uint16_t>> edges(3);
    for (uint16_t source = 0; source < 3; ++source) {
      for (uint16_t target = 0; target < 3; ++target) {
        if (mask & (1u << (source * 3 + target))) {
          edges[source].push_back(target);
        }
      }
    }
    CheckAgainstReachability(Graph(std::move(edges)));
  }
}

TEST_F(CfgPostdominanceTest, ArbitraryGraphsAgainstReachability) {
  uint32_t seed = 4139;
  auto random = [&]() { return seed = seed * 1664525u + 1013904223u; };
  for (int iteration = 0; iteration < 4000; ++iteration) {
    SCOPED_TRACE(iteration);
    const size_t count = 1 + random() % 24;
    std::vector<std::vector<uint16_t>> edges(count);
    for (auto& successors : edges) {
      const size_t successor_count = random() % 5;
      for (size_t i = 0; i < successor_count; ++i) {
        successors.push_back(random() % count);
      }
    }
    CheckAgainstReachability(Graph(std::move(edges)));
  }
}

TEST_F(CfgPostdominanceTest, FullBlockIndexRangeWithoutRecursiveStack) {
  constexpr size_t count = UINT16_MAX;
  std::vector<std::vector<uint16_t>> edges(count);
  edges[0].push_back(count - 1);
  for (size_t i = count - 1; i > 1; --i) {
    edges[i].push_back(i - 1);
  }
  Graph fixture(std::move(edges));
  loom_cfg_postdominance_t postdominance;
  IREE_ASSERT_OK(
      loom_cfg_postdominance_build(fixture.get(), &arena_, &postdominance));
  ASSERT_TRUE(postdominance.available);
  ASSERT_EQ(postdominance.exit_node, count);
  EXPECT_EQ(postdominance.nodes[0].immediate_postdominator, count - 1);
  EXPECT_EQ(postdominance.nodes[0].depth, count);
  EXPECT_EQ(postdominance.nodes[1].immediate_postdominator, count);
  EXPECT_EQ(postdominance.nodes[1].depth, 1u);
  for (size_t i = 2; i < count; ++i) {
    EXPECT_EQ(postdominance.nodes[i].immediate_postdominator, i - 1);
    EXPECT_EQ(postdominance.nodes[i].depth, i);
  }
  EXPECT_EQ(postdominance.nodes[count].immediate_postdominator, count);
  EXPECT_EQ(postdominance.nodes[count].depth, 0u);
}

TEST_F(CfgPostdominanceTest, MalformedGraphIsUnavailable) {
  loom_cfg_graph_t graph = {};
  graph.malformed = true;
  loom_cfg_postdominance_t postdominance;
  IREE_ASSERT_OK(loom_cfg_postdominance_build(&graph, &arena_, &postdominance));
  EXPECT_FALSE(postdominance.available);
  EXPECT_EQ(postdominance.nodes, nullptr);
}

}  // namespace
}  // namespace loom
