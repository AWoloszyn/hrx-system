// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/analysis/condition_fact_scope.h"

#include "iree/testing/gtest.h"

namespace loom {
namespace {

static loom_condition_integer_operand_t ValueOperand(loom_value_id_t value) {
  return loom_condition_integer_operand_t{
      /*.kind=*/LOOM_CONDITION_INTEGER_OPERAND_VALUE,
      /*.value_id=*/value,
  };
}

static loom_condition_derivation_t Derivation(
    loom_condition_integer_relation_t* relations,
    iree_host_size_t relation_count) {
  return loom_condition_derivation_t{
      /*.integer_facts=*/
      {
          /*.integer_relations=*/relations,
          /*.integer_relation_count=*/relation_count,
          /*.integer_relation_capacity=*/relation_count,
      },
  };
}

TEST(ConditionFactScopeTest, ConjoinsRelationsAcrossFragments) {
  const loom_value_id_t left = 1;
  const loom_value_id_t right = 2;
  loom_condition_integer_relation_t parent_relations[] = {{
      /*.relation=*/LOOM_SYMBOLIC_INTEGER_RELATION_LE,
      /*.left=*/ValueOperand(left),
      /*.right=*/ValueOperand(right),
  }};
  loom_condition_integer_relation_t child_relations[] = {{
      /*.relation=*/LOOM_SYMBOLIC_INTEGER_RELATION_NE,
      /*.left=*/ValueOperand(left),
      /*.right=*/ValueOperand(right),
  }};
  const loom_condition_derivation_t parent_derivation =
      Derivation(parent_relations, IREE_ARRAYSIZE(parent_relations));
  const loom_condition_derivation_t child_derivation =
      Derivation(child_relations, IREE_ARRAYSIZE(child_relations));
  loom_condition_fact_scope_t parent_scope = {};
  loom_condition_fact_scope_initialize_local(nullptr, &parent_derivation,
                                             &parent_scope);
  loom_condition_fact_scope_t child_scope = {};
  loom_condition_fact_scope_initialize_local(&parent_scope, &child_derivation,
                                             &child_scope);

  const loom_condition_integer_relation_t query = {
      /*.relation=*/LOOM_SYMBOLIC_INTEGER_RELATION_LT,
      /*.left=*/ValueOperand(left),
      /*.right=*/ValueOperand(right),
  };
  bool result = false;
  EXPECT_TRUE(loom_condition_fact_scope_proves_integer_relation(
      &child_scope, nullptr, &query, &result));
  EXPECT_TRUE(result);
}

TEST(ConditionFactScopeTest, ContradictoryFragmentsRemainUnknown) {
  const loom_value_id_t left = 1;
  const loom_value_id_t right = 2;
  loom_condition_integer_relation_t parent_relations[] = {{
      /*.relation=*/LOOM_SYMBOLIC_INTEGER_RELATION_LT,
      /*.left=*/ValueOperand(left),
      /*.right=*/ValueOperand(right),
  }};
  loom_condition_integer_relation_t child_relations[] = {{
      /*.relation=*/LOOM_SYMBOLIC_INTEGER_RELATION_GE,
      /*.left=*/ValueOperand(left),
      /*.right=*/ValueOperand(right),
  }};
  const loom_condition_derivation_t parent_derivation =
      Derivation(parent_relations, IREE_ARRAYSIZE(parent_relations));
  const loom_condition_derivation_t child_derivation =
      Derivation(child_relations, IREE_ARRAYSIZE(child_relations));
  loom_condition_fact_scope_t parent_scope = {};
  loom_condition_fact_scope_initialize_local(nullptr, &parent_derivation,
                                             &parent_scope);
  loom_condition_fact_scope_t child_scope = {};
  loom_condition_fact_scope_initialize_local(&parent_scope, &child_derivation,
                                             &child_scope);

  const loom_condition_integer_relation_t query = {
      /*.relation=*/LOOM_SYMBOLIC_INTEGER_RELATION_LT,
      /*.left=*/ValueOperand(left),
      /*.right=*/ValueOperand(right),
  };
  bool result = false;
  EXPECT_FALSE(loom_condition_fact_scope_proves_integer_relation(
      &child_scope, nullptr, &query, &result));
}

}  // namespace
}  // namespace loom
