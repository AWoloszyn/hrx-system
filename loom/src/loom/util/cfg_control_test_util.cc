// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/util/cfg_control_test_util.h"

#include <algorithm>

namespace loom::testing {

CfgControlOracle::CfgControlOracle(const loom_cfg_graph_t* graph)
    : graph_(graph),
      postdominators_(graph->block_count + 1),
      paths_(graph->edge_count, std::vector<bool>(graph->block_count)),
      controllers_(graph->block_count, std::vector<bool>(graph->block_count)) {
  const size_t exit = graph->block_count;
  const size_t count = exit + 1;
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
  std::vector<std::vector<bool>> dominates(count, std::vector<bool>(count));
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
      dominates[source][removed] = !reachable[exit];
      postdominators_[source].depth += dominates[source][removed];
    }
    --postdominators_[source].depth;  // Exclude the removed node itself.
  }
  for (size_t source = 0; source < count; ++source) {
    uint32_t parent = LOOM_CFG_POSTDOMINATOR_INVALID;
    if (source == exit) {
      parent = exit;
    } else if (graph->blocks[source].reachable) {
      for (uint32_t candidate = 0; candidate < count; ++candidate) {
        if (candidate == source || !dominates[source][candidate]) {
          continue;
        }
        if (parent == LOOM_CFG_POSTDOMINATOR_INVALID ||
            postdominators_[candidate].depth > postdominators_[parent].depth) {
          parent = candidate;
        }
      }
    }
    postdominators_[source].immediate_postdominator = parent;
  }
  for (uint32_t edge = 0; edge < graph->edge_count; ++edge) {
    const auto& alternative = graph->edges[edge];
    const uint16_t source = alternative.source_block_index;
    if (!graph->blocks[source].reachable) {
      continue;
    }
    auto successors = loom_cfg_graph_successors(graph, source);
    bool distinct = false;
    for (size_t i = 1; i < successors.count; ++i) {
      distinct |= successors.values[i] != successors.values[0];
    }
    if (!distinct) {
      continue;
    }
    const auto stop = postdominators_[source].immediate_postdominator;
    auto node = static_cast<uint32_t>(alternative.target_block_index);
    while (postdominators_[node].depth > postdominators_[stop].depth) {
      paths_[edge][node] = true;
      controllers_[source][node] = true;
      node = postdominators_[node].immediate_postdominator;
    }
  }
  for (size_t via = 0; via < exit; ++via) {
    for (size_t source = 0; source < exit; ++source) {
      for (size_t target = 0; target < exit; ++target) {
        controllers_[source][target] =
            controllers_[source][target] ||
            (controllers_[source][via] && controllers_[via][target]);
      }
    }
  }
}

iree_status_t CfgControlOracle::CheckStructure(
    const loom_cfg_control_t& structure) const {
  if (!structure.available) {
    return iree_make_status(IREE_STATUS_INTERNAL, "valid control unavailable");
  }
  if (!structure.node_count) {
    for (const auto& path : paths_) {
      if (std::any_of(path.begin(), path.end(),
                      [](bool value) { return value; })) {
        return iree_make_status(IREE_STATUS_INTERNAL, "missing control path");
      }
    }
    return iree_ok_status();
  }
  for (size_t i = 0; i < postdominators_.size(); ++i) {
    const auto& expected = postdominators_[i];
    const auto& actual = structure.postdominance.nodes[i];
    if (actual.immediate_postdominator != expected.immediate_postdominator ||
        actual.depth != expected.depth) {
      return iree_make_status(IREE_STATUS_INTERNAL,
                              "postdominator mismatch at %zu", i);
    }
  }
  std::vector<uint32_t> leaf_blocks(structure.leaf_base + 1,
                                    graph_->block_count);
  for (uint32_t i = 0; i < graph_->block_count; ++i) {
    if (graph_->blocks[i].reachable) {
      leaf_blocks[structure.blocks[i].node - structure.leaf_base] = i;
    }
  }
  for (uint32_t edge = 0; edge < graph_->edge_count; ++edge) {
    std::vector<bool> actual(graph_->block_count + 1);
    std::vector<uint32_t> pending;
    const auto& block =
        structure.blocks[graph_->edges[edge].source_block_index];
    for (uint32_t i = block.binding_start;
         i < block.binding_start + block.binding_count; ++i) {
      if (structure.bindings[i].edge == edge) {
        pending.push_back(structure.bindings[i].node);
      }
    }
    while (!pending.empty()) {
      uint32_t node = pending.back();
      pending.pop_back();
      if (node < structure.leaf_base) {
        pending.push_back(2 * node + 1);
        pending.push_back(2 * node + 2);
      } else {
        actual[leaf_blocks[node - structure.leaf_base]] = true;
      }
    }
    if (actual.back() ||
        !std::equal(paths_[edge].begin(), paths_[edge].end(), actual.begin())) {
      return iree_make_status(IREE_STATUS_INTERNAL,
                              "control path mismatch at edge %u", edge);
    }
  }
  for (uint32_t i = 0; i < structure.input_count; ++i) {
    const auto& input = structure.inputs[i];
    if (input.source_component != LOOM_CFG_CONTROL_INVALID &&
        input.source_component <= input.target_component) {
      return iree_make_status(IREE_STATUS_INTERNAL,
                              "control component order at input %u", i);
    }
  }
  return iree_ok_status();
}

std::vector<uint8_t> CfgControlOracle::Solve(
    const std::vector<uint8_t>& selectors) const {
  std::vector<uint8_t> result(graph_->block_count, 4);
  for (size_t i = 0; i < result.size(); ++i) {
    if (!graph_->blocks[i].reachable) {
      result[i] = 1;
    }
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t edge = 0; edge < paths_.size(); ++edge) {
      const auto source = graph_->edges[edge].source_block_index;
      for (size_t target = 0; target < result.size(); ++target) {
        if (!paths_[edge][target]) {
          continue;
        }
        uint8_t next =
            std::min({result[target], result[source], selectors[source]});
        changed |= next != result[target];
        result[target] = next;
      }
    }
  }
  return result;
}

loom_value_facts_t ControlDistribution(uint8_t ordinal) {
  auto facts = loom_value_facts_unknown();
  if (ordinal == 0) {
    loom_value_facts_mark_lane_varying(&facts);
  } else {
    loom_value_facts_mark_uniform_at_scope(
        &facts, static_cast<loom_value_fact_uniform_scope_t>(ordinal - 1));
  }
  return facts;
}

iree_status_t CfgControlOracle::CheckFacts(
    loom_value_fact_control_t* control,
    const std::vector<uint8_t>& selectors) const {
  loom_value_fact_control_prepare_diagnostics(control);
  auto expected = Solve(selectors);
  for (size_t i = 0; i < expected.size(); ++i) {
    if (loom_value_fact_control_execution(control, i).flags !=
        ControlDistribution(expected[i]).flags) {
      return iree_make_status(IREE_STATUS_INTERNAL,
                              "execution scope mismatch at block %zu", i);
    }
    const auto edge = loom_value_fact_control_controller(control, i);
    if (graph_->blocks[i].reachable && expected[i] < 4) {
      if (edge >= graph_->edge_count) {
        return iree_make_status(IREE_STATUS_INTERNAL,
                                "missing controller at block %zu", i);
      }
      const auto source = graph_->edges[edge].source_block_index;
      if (selectors[source] != expected[i] || !controllers_[source][i]) {
        return iree_make_status(IREE_STATUS_INTERNAL,
                                "invalid controller at block %zu", i);
      }
    } else if (edge != LOOM_CFG_EDGE_INDEX_INVALID) {
      return iree_make_status(IREE_STATUS_INTERNAL,
                              "unexpected controller at block %zu", i);
    }
  }
  return iree_ok_status();
}

iree_status_t CfgControlOracle::CheckPublication(
    loom_value_fact_control_t* control, const std::vector<uint8_t>& selectors,
    std::vector<uint8_t>* previous) const {
  auto expected = Solve(selectors);
  std::vector<bool> published(expected.size());
  uint16_t block = 0;
  while (loom_value_fact_control_take_changed_block(control, &block)) {
    if (block >= expected.size() || published[block]) {
      return iree_make_status(IREE_STATUS_INTERNAL,
                              "invalid notification for block %u", block);
    }
    published[block] = true;
  }
  for (size_t i = 0; i < expected.size(); ++i) {
    if (published[i] != ((*previous)[i] != expected[i])) {
      return iree_make_status(IREE_STATUS_INTERNAL,
                              "notification mismatch at block %zu", i);
    }
  }
  *previous = std::move(expected);
  return iree_ok_status();
}

}  // namespace loom::testing
