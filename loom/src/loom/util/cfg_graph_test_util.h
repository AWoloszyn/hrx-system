// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_UTIL_CFG_GRAPH_TEST_UTIL_H_
#define LOOM_UTIL_CFG_GRAPH_TEST_UTIL_H_

#include <vector>

#include "loom/util/cfg_graph.h"

namespace loom::testing {

// Adjacency and entry-reachability fixture for graph-only analysis APIs.
// Authored IR and extraction contracts are exercised by loom-check fixtures.
class CfgGraph {
 public:
  explicit CfgGraph(const std::vector<std::vector<uint16_t>>& successors)
      : blocks_(successors.size()) {
    std::vector<std::vector<uint32_t>> incoming(blocks_.size());
    for (size_t i = 0; i < blocks_.size(); ++i) {
      auto& block = blocks_[i];
      block.successor_start = successor_indices_.size();
      block.successor_edge_start = successor_indices_.size();
      block.successor_count = successors[i].size();
      for (uint16_t target : successors[i]) {
        uint32_t edge = edges_.size();
        incoming[target].push_back(edge);
        loom_cfg_edge_info_t edge_info = {};
        edge_info.source_block_index = static_cast<uint16_t>(i);
        edge_info.target_block_index = target;
        edge_info.successor_index =
            static_cast<uint16_t>(edge - block.successor_start);
        edges_.push_back(edge_info);
        successor_indices_.push_back(target);
        successor_edges_.push_back(edge);
      }
    }
    for (size_t i = 0; i < blocks_.size(); ++i) {
      auto& block = blocks_[i];
      block.predecessor_start = predecessor_indices_.size();
      block.predecessor_edge_start = predecessor_indices_.size();
      block.predecessor_count = incoming[i].size();
      for (uint32_t edge : incoming[i]) {
        predecessor_indices_.push_back(edges_[edge].source_block_index);
        predecessor_edges_.push_back(edge);
      }
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
    graph_.edges = edges_.data();
    graph_.edge_count = edges_.size();
    graph_.successor_indices = successor_indices_.data();
    graph_.predecessor_indices = predecessor_indices_.data();
    graph_.successor_edge_indices = successor_edges_.data();
    graph_.predecessor_edge_indices = predecessor_edges_.data();
  }

  CfgGraph(const CfgGraph&) = delete;
  CfgGraph& operator=(const CfgGraph&) = delete;
  CfgGraph(CfgGraph&&) = delete;
  CfgGraph& operator=(CfgGraph&&) = delete;

  const loom_cfg_graph_t* get() const { return &graph_; }

 private:
  // Per-block adjacency and entry reachability.
  std::vector<loom_cfg_block_info_t> blocks_;
  // Stable edge identities and source/target ordinals.
  std::vector<loom_cfg_edge_info_t> edges_;
  // Dense outgoing block adjacency.
  std::vector<uint16_t> successor_indices_;
  // Dense incoming block adjacency.
  std::vector<uint16_t> predecessor_indices_;
  // Dense outgoing edge adjacency.
  std::vector<uint32_t> successor_edges_;
  // Dense incoming edge adjacency.
  std::vector<uint32_t> predecessor_edges_;
  // Non-owning graph view over the fixture arrays.
  loom_cfg_graph_t graph_ = {};
};

}  // namespace loom::testing

#endif  // LOOM_UTIL_CFG_GRAPH_TEST_UTIL_H_
