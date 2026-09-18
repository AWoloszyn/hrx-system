// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_UTIL_CFG_CONTROL_TEST_UTIL_H_
#define LOOM_UTIL_CFG_CONTROL_TEST_UTIL_H_

#include <vector>

#include "loom/util/fact_control.h"

namespace loom::testing {

// Independent, deliberately exhaustive oracle for small CFGs. Postdominators
// are found by removing each candidate and testing exit reachability; control
// paths are explicit, and scope propagation starts from scratch on every call.
// There are no segment trees, SCCs, input buckets, or incremental caches here.
class CfgControlOracle {
 public:
  explicit CfgControlOracle(const loom_cfg_graph_t* graph);

  const std::vector<loom_cfg_postdominator_t>& postdominators() const {
    return postdominators_;
  }

  iree_status_t CheckStructure(const loom_cfg_control_t& structure) const;

  // Checks settled scope and current diagnostic provenance.
  iree_status_t CheckFacts(loom_value_fact_control_t* control,
                           const std::vector<uint8_t>& selectors) const;
  // Checks exactly-once notifications since the last publication. Callers may
  // settle several times before publishing to test reset/restore sequences.
  iree_status_t CheckPublication(loom_value_fact_control_t* control,
                                 const std::vector<uint8_t>& selectors,
                                 std::vector<uint8_t>* previous) const;

  std::vector<uint8_t> Solve(const std::vector<uint8_t>& selectors) const;

 private:
  // Borrowed adjacency and entry reachability, valid for this oracle's
  // lifetime.
  const loom_cfg_graph_t* graph_;
  // Independently computed immediate postdominators and depths.
  std::vector<loom_cfg_postdominator_t> postdominators_;
  // Explicit block membership of each CFG alternative's control path.
  std::vector<std::vector<bool>> paths_;
  // Transitive control reachability, used to validate diagnostic witnesses.
  std::vector<std::vector<bool>> controllers_;
};

// Test ordinals: varying, unknown, subgroup, workgroup, cluster.
loom_value_facts_t ControlDistribution(uint8_t ordinal);

}  // namespace loom::testing

#endif  // LOOM_UTIL_CFG_CONTROL_TEST_UTIL_H_
