// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/transforms/vector/bank_sroa.h"

#include "loom/target/pass_environment.h"
#include "loom/transforms/boundary/projection_driver.h"
#include "loom/transforms/vector/bank_sroa_projection.h"

#define LOOM_VECTOR_BANK_SROA_STATISTICS(V, statistics_type)        \
  V(statistics_type, loops_checked, "loops-checked",                \
    "Number of LoopLike operations checked for vector banks.")      \
  V(statistics_type, loops_scalarized, "loops-scalarized",          \
    "Number of LoopLike operations rebuilt with scalarized banks.") \
  V(statistics_type, banks_scalarized, "banks-scalarized",          \
    "Number of loop-carried vector banks scalarized.")              \
  V(statistics_type, slots_materialized, "slots-materialized",      \
    "Number of scalar or tail-vector bank slots materialized.")     \
  V(statistics_type, extracts_eliminated, "extracts-eliminated",    \
    "Number of vector.extract operations eliminated.")              \
  V(statistics_type, inserts_eliminated, "inserts-eliminated",      \
    "Number of vector.insert operations eliminated.")

LOOM_PASS_STATISTICS_DEFINE(loom_vector_bank_sroa_statistics,
                            loom_vector_bank_sroa_statistics_t,
                            LOOM_VECTOR_BANK_SROA_STATISTICS)

static const loom_pass_info_t kPassInfo = {
    .name = IREE_SVL("sroa-vector-banks"),
    .description = IREE_SVL(
        "Split statically addressed loop-carried vector banks into slots."),
    .kind = LOOM_PASS_MODULE,
    .statistic_layout = &loom_vector_bank_sroa_statistics_layout,
};

const loom_pass_info_t* loom_vector_bank_sroa_pass_info(void) {
  return &kPassInfo;
}

iree_status_t loom_vector_bank_sroa_run(loom_pass_t* pass,
                                        loom_module_t* module) {
  const loom_target_pass_capability_t* capability =
      loom_target_pass_capability_from_pass(pass);
  const loom_function_version_list_t* version_list =
      capability ? loom_target_pass_capability_function_versions(capability)
                 : NULL;
  const loom_boundary_projection_rule_t* rules[] = {
      loom_vector_bank_sroa_boundary_projection_rule(),
  };
  loom_boundary_projection_statistics_t projection_statistics;
  iree_status_t status =
      loom_boundary_projection_run(pass, module, version_list,
                                   (loom_boundary_projection_rule_list_t){
                                       .values = rules,
                                       .count = IREE_ARRAYSIZE(rules),
                                   },
                                   &projection_statistics);
  if (iree_status_is_ok(status)) {
    IREE_ASSERT_EQ(projection_statistics.rule_count, IREE_ARRAYSIZE(rules));
    const loom_boundary_projection_rule_statistics_t* rule_statistics =
        &projection_statistics.rules[0];
    loom_vector_bank_sroa_statistics_t* statistics =
        loom_vector_bank_sroa_statistics(pass);
    statistics->loops_checked += projection_statistics.loops_checked;
    statistics->loops_scalarized += projection_statistics.loops_rewritten;
    statistics->banks_scalarized += rule_statistics->projections;
    statistics->slots_materialized += rule_statistics->components;
    statistics->extracts_eliminated +=
        rule_statistics->destination_uses_rewritten;
    statistics->inserts_eliminated +=
        rule_statistics->source_operations_eliminated;
  }
  return status;
}
