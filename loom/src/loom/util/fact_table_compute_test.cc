// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <vector>

#include "iree/base/internal/arena.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/ir/context.h"
#include "loom/ir/module.h"
#include "loom/ops/index/ops.h"
#include "loom/ops/scf/ops.h"
#include "loom/util/fact_table.h"

namespace loom {
namespace {

class FactTableComputeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    iree_arena_block_pool_initialize(4096, iree_allocator_system(), &pool_);
    iree_arena_initialize(&pool_, &arena_);
    loom_context_initialize(iree_allocator_system(), &context_);
    iree_host_size_t count = 0;
    const loom_op_vtable_t* const* vtables = loom_index_dialect_vtables(&count);
    IREE_ASSERT_OK(loom_context_register_dialect(
        &context_, LOOM_DIALECT_INDEX, vtables, static_cast<uint16_t>(count)));
    vtables = loom_scf_dialect_vtables(&count);
    IREE_ASSERT_OK(loom_context_register_dialect(
        &context_, LOOM_DIALECT_SCF, vtables, static_cast<uint16_t>(count)));
    IREE_ASSERT_OK(loom_context_finalize(&context_));
    IREE_ASSERT_OK(loom_module_allocate(&context_, IREE_SV("facts"), &pool_,
                                        nullptr, iree_allocator_system(),
                                        &module_));
    loom_builder_initialize(module_, &module_->arena,
                            loom_module_block(module_), &builder_);
    IREE_ASSERT_OK(loom_value_fact_table_initialize(&table_, &arena_, 4));
    for (loom_value_id_t& input : inputs_) {
      IREE_ASSERT_OK(loom_builder_define_value(&builder_, type_, &input));
      IREE_ASSERT_OK(loom_value_fact_table_define(&table_, input,
                                                  loom_value_facts_unknown()));
    }
  }

  void TearDown() override {
    loom_module_free(module_);
    loom_context_deinitialize(&context_);
    iree_arena_deinitialize(&arena_);
    iree_arena_block_pool_deinitialize(&pool_);
  }

  iree_status_t BuildIdentity(loom_value_id_t input, loom_op_t** out_op) {
    return loom_index_assume_build(&builder_, &input, 1, nullptr, 0, &type_, 1,
                                   LOOM_LOCATION_UNKNOWN, out_op);
  }

  loom_value_id_t DefineValue(loom_type_t type) {
    loom_value_id_t value = LOOM_VALUE_ID_INVALID;
    IREE_CHECK_OK(loom_builder_define_value(&builder_, type, &value));
    IREE_CHECK_OK(loom_value_fact_table_define(&table_, value,
                                               loom_value_facts_unknown()));
    return value;
  }

  loom_op_t* BuildSelect(loom_value_id_t condition, loom_value_id_t true_value,
                         loom_value_id_t false_value) {
    loom_op_t* op = nullptr;
    IREE_CHECK_OK(loom_scf_select_build(&builder_, condition, true_value,
                                        false_value, type_,
                                        LOOM_LOCATION_UNKNOWN, &op));
    return op;
  }

  static std::vector<loom_value_id_t> SelectDependencies(
      const loom_value_fact_table_t& table, loom_value_id_t value) {
    loom_value_set_cursor_t cursor;
    loom_value_fact_table_select_dependencies_begin(&table, value, &cursor);
    std::vector<loom_value_id_t> dependencies;
    for (loom_value_id_t dependency = loom_value_set_cursor_next(&cursor);
         dependency != LOOM_VALUE_ID_INVALID;
         dependency = loom_value_set_cursor_next(&cursor)) {
      dependencies.push_back(dependency);
    }
    return dependencies;
  }

  static loom_value_set_id_t SelectDependencyRoot(
      const loom_value_fact_table_t& table, loom_value_id_t value) {
    loom_value_set_cursor_t cursor;
    return loom_value_fact_table_select_dependencies_begin(&table, value,
                                                           &cursor);
  }

  iree_arena_block_pool_t pool_;
  iree_arena_allocator_t arena_;
  loom_context_t context_;
  loom_module_t* module_ = nullptr;
  loom_builder_t builder_;
  loom_value_fact_table_t table_;
  const loom_type_t type_ = loom_type_scalar(LOOM_SCALAR_TYPE_INDEX);
  loom_value_id_t inputs_[2];
};

TEST_F(FactTableComputeTest, IdentityOnlyMutationReportsChangedFacts) {
  EXPECT_EQ(table_.identities.entries, nullptr);
  EXPECT_EQ(loom_value_fact_table_query_identity(&table_, inputs_[0]),
            inputs_[0]);
  EXPECT_EQ(loom_value_fact_table_query_identity(nullptr, inputs_[0]),
            inputs_[0]);
  loom_op_t* first = nullptr;
  IREE_ASSERT_OK(BuildIdentity(inputs_[0], &first));
  bool changed = false;
  IREE_ASSERT_OK(loom_value_fact_table_compute_op_and_report(&table_, module_,
                                                             first, &changed));
  EXPECT_TRUE(changed);
  EXPECT_EQ(table_.select_dependencies.index, nullptr);
  EXPECT_EQ(table_.select_dependencies.roots, nullptr);
  const loom_value_id_t first_result = loom_op_const_results(first)[0];
  loom_op_t* second = nullptr;
  IREE_ASSERT_OK(BuildIdentity(first_result, &second));
  IREE_ASSERT_OK(loom_value_fact_table_compute_op(&table_, module_, second));
  const loom_value_id_t second_result = loom_op_const_results(second)[0];
  EXPECT_EQ(loom_value_fact_table_query_identity(&table_, second_result),
            inputs_[0]);
  IREE_ASSERT_OK(loom_value_fact_table_compute_op_and_report(&table_, module_,
                                                             first, &changed));
  EXPECT_FALSE(changed);

  IREE_ASSERT_OK(loom_op_set_operand(module_, first, 0, inputs_[1]));
  IREE_ASSERT_OK(loom_value_fact_table_compute_op_and_report(&table_, module_,
                                                             first, &changed));
  EXPECT_TRUE(changed);
  IREE_ASSERT_OK(loom_value_fact_table_compute_op_and_report(&table_, module_,
                                                             second, &changed));
  EXPECT_TRUE(changed);
  EXPECT_EQ(loom_value_fact_table_query_identity(&table_, second_result),
            inputs_[1]);
  EXPECT_TRUE(loom_value_facts_is_unknown(
      loom_value_fact_table_lookup(&table_, second_result)));
}

TEST_F(FactTableComputeTest,
       SelectDependenciesPropagateCloneAndTrackOperandMutation) {
  const loom_type_t i1_type = loom_type_scalar(LOOM_SCALAR_TYPE_I1);
  const loom_value_id_t first_condition = DefineValue(i1_type);
  const loom_value_id_t second_condition = DefineValue(i1_type);
  loom_op_t* select = BuildSelect(first_condition, inputs_[0], inputs_[1]);
  bool changed = false;
  IREE_ASSERT_OK(loom_value_fact_table_compute_op_and_report(&table_, module_,
                                                             select, &changed));
  EXPECT_TRUE(changed);
  const loom_value_id_t selected = loom_scf_select_result(select);
  EXPECT_EQ(SelectDependencies(table_, selected),
            std::vector<loom_value_id_t>({first_condition}));

  loom_op_t* add = nullptr;
  IREE_ASSERT_OK(loom_index_add_build(&builder_, selected, inputs_[0], type_,
                                      LOOM_LOCATION_UNKNOWN, &add));
  IREE_ASSERT_OK(loom_value_fact_table_compute_op(&table_, module_, add));
  const loom_value_id_t sum = loom_index_add_result(add);
  EXPECT_EQ(SelectDependencies(table_, sum),
            std::vector<loom_value_id_t>({first_condition}));

  IREE_ASSERT_OK(loom_op_set_operand(module_, select, 0, second_condition));
  IREE_ASSERT_OK(loom_value_fact_table_compute_op_and_report(&table_, module_,
                                                             select, &changed));
  EXPECT_TRUE(changed);
  IREE_ASSERT_OK(loom_value_fact_table_compute_op_and_report(&table_, module_,
                                                             add, &changed));
  EXPECT_TRUE(changed);
  EXPECT_EQ(SelectDependencies(table_, sum),
            std::vector<loom_value_id_t>({second_condition}));
  IREE_ASSERT_OK(loom_value_fact_table_compute_op_and_report(&table_, module_,
                                                             add, &changed));
  EXPECT_FALSE(changed);

  loom_value_fact_table_t clone = {};
  IREE_ASSERT_OK(loom_value_fact_table_initialize(&clone, &arena_, 0));
  IREE_ASSERT_OK(
      loom_value_fact_table_clone_values(&clone, {&table_, &sum, 1}, module_));
  EXPECT_EQ(SelectDependencies(clone, sum),
            std::vector<loom_value_id_t>({second_condition}));

  loom_value_fact_table_undefine(&table_, sum);
  EXPECT_EQ(SelectDependencies(table_, sum),
            std::vector<loom_value_id_t>({second_condition}));
  loom_value_fact_table_t undefined_clone = {};
  IREE_ASSERT_OK(
      loom_value_fact_table_initialize(&undefined_clone, &arena_, 0));
  IREE_ASSERT_OK(loom_value_fact_table_clone_values(
      &undefined_clone, {&table_, &sum, 1}, module_));
  EXPECT_FALSE(loom_value_fact_table_has_entry(&undefined_clone, sum));
  EXPECT_EQ(SelectDependencies(undefined_clone, sum),
            std::vector<loom_value_id_t>({second_condition}));
  loom_value_fact_table_clear_scope(&table_);
  EXPECT_TRUE(SelectDependencies(table_, sum).empty());
  EXPECT_EQ(table_.select_dependencies.index, nullptr);
  EXPECT_EQ(table_.select_dependencies.roots, nullptr);
}

TEST_F(FactTableComputeTest,
       FactIdentityPredicateReferencesContributeDependencies) {
  const loom_type_t i1_type = loom_type_scalar(LOOM_SCALAR_TYPE_I1);
  const loom_value_id_t first_condition = DefineValue(i1_type);
  const loom_value_id_t second_condition = DefineValue(i1_type);
  loom_op_t* first_select =
      BuildSelect(first_condition, inputs_[0], inputs_[1]);
  loom_op_t* second_select =
      BuildSelect(second_condition, inputs_[0], inputs_[1]);
  IREE_ASSERT_OK(
      loom_value_fact_table_compute_op(&table_, module_, first_select));
  IREE_ASSERT_OK(
      loom_value_fact_table_compute_op(&table_, module_, second_select));

  loom_predicate_t predicate = {
      /*.kind=*/LOOM_PREDICATE_GE,
      /*.arg_count=*/2,
      /*.arg_tags=*/{LOOM_PRED_ARG_VALUE, LOOM_PRED_ARG_VALUE},
      /*.reserved=*/{},
      /*.args=*/{inputs_[0], loom_scf_select_result(first_select)},
  };
  loom_op_t* assume = nullptr;
  IREE_ASSERT_OK(loom_index_assume_build(&builder_, &inputs_[0], 1, &predicate,
                                         1, &type_, 1, LOOM_LOCATION_UNKNOWN,
                                         &assume));
  IREE_ASSERT_OK(loom_value_fact_table_compute_op(&table_, module_, assume));
  const loom_value_id_t result = loom_index_assume_results(assume).values[0];
  EXPECT_EQ(SelectDependencies(table_, result),
            std::vector<loom_value_id_t>({first_condition}));

  predicate.args[1] = loom_scf_select_result(second_select);
  IREE_ASSERT_OK(loom_op_set_attr(module_, assume, 0,
                                  loom_attr_predicate_list(&predicate, 1)));
  bool changed = false;
  IREE_ASSERT_OK(loom_value_fact_table_compute_op_and_report(&table_, module_,
                                                             assume, &changed));
  EXPECT_TRUE(changed);
  EXPECT_EQ(SelectDependencies(table_, result),
            std::vector<loom_value_id_t>({second_condition}));

  predicate.args[1] = inputs_[1];
  IREE_ASSERT_OK(loom_op_set_attr(module_, assume, 0,
                                  loom_attr_predicate_list(&predicate, 1)));
  IREE_ASSERT_OK(loom_value_fact_table_compute_op_and_report(&table_, module_,
                                                             assume, &changed));
  EXPECT_TRUE(changed);
  EXPECT_TRUE(SelectDependencies(table_, result).empty());
}

TEST_F(FactTableComputeTest, NonBooleanSelectorDoesNotCreateDependency) {
  const int64_t case_key = 0;
  const loom_value_id_t values[] = {inputs_[0], inputs_[1]};
  loom_op_t* lookup = nullptr;
  IREE_ASSERT_OK(loom_scf_lookup_build(
      &builder_, inputs_[0], &case_key, 1, values, IREE_ARRAYSIZE(values),
      &type_, 1, /*tied_results=*/nullptr, /*tied_result_count=*/0,
      LOOM_LOCATION_UNKNOWN, &lookup));
  IREE_ASSERT_OK(loom_value_fact_table_compute_op(&table_, module_, lookup));
  EXPECT_TRUE(
      SelectDependencies(table_, loom_scf_lookup_results(lookup).values[0])
          .empty());
  EXPECT_EQ(table_.select_dependencies.index, nullptr);
  EXPECT_EQ(table_.select_dependencies.roots, nullptr);
}

TEST_F(FactTableComputeTest,
       VariadicMultiResultProducerSharesCompleteDependencySet) {
  constexpr uint16_t kCaseCount = 47;
  constexpr uint16_t kConditionCount = 96;
  static_assert(kConditionCount == (kCaseCount + 1) * 2);
  std::vector<loom_value_id_t> conditions;
  std::vector<loom_value_id_t> selected_values;
  conditions.reserve(kConditionCount);
  selected_values.reserve(kConditionCount);
  const loom_type_t i1_type = loom_type_scalar(LOOM_SCALAR_TYPE_I1);
  for (uint16_t i = 0; i < kConditionCount; ++i) {
    const loom_value_id_t condition = DefineValue(i1_type);
    conditions.push_back(condition);
    loom_op_t* select = BuildSelect(condition, inputs_[0], inputs_[1]);
    IREE_ASSERT_OK(loom_value_fact_table_compute_op(&table_, module_, select));
    selected_values.push_back(loom_scf_select_result(select));
  }

  int64_t case_keys[kCaseCount];
  for (uint16_t i = 0; i < kCaseCount; ++i) {
    case_keys[i] = i;
  }
  const loom_type_t result_types[] = {type_, type_};
  loom_op_t* lookup = nullptr;
  IREE_ASSERT_OK(loom_scf_lookup_build(
      &builder_, inputs_[0], case_keys, IREE_ARRAYSIZE(case_keys),
      selected_values.data(), selected_values.size(), result_types,
      IREE_ARRAYSIZE(result_types), /*tied_results=*/nullptr,
      /*tied_result_count=*/0, LOOM_LOCATION_UNKNOWN, &lookup));
  IREE_ASSERT_OK(loom_value_fact_table_compute_op(&table_, module_, lookup));
  const loom_value_id_t* results = loom_op_const_results(lookup);
  EXPECT_EQ(SelectDependencies(table_, results[0]), conditions);
  EXPECT_EQ(SelectDependencies(table_, results[1]), conditions);
  const loom_value_set_id_t shared_root =
      SelectDependencyRoot(table_, results[0]);
  EXPECT_EQ(SelectDependencyRoot(table_, results[1]), shared_root);

  loom_op_t* shared = nullptr;
  IREE_ASSERT_OK(loom_index_add_build(&builder_, results[0], results[0], type_,
                                      LOOM_LOCATION_UNKNOWN, &shared));
  IREE_ASSERT_OK(loom_value_fact_table_compute_op(&table_, module_, shared));
  const loom_value_id_t shared_result = loom_index_add_result(shared);
  EXPECT_EQ(SelectDependencyRoot(table_, shared_result), shared_root);
  EXPECT_EQ(SelectDependencies(table_, shared_result), conditions);
}

TEST_F(FactTableComputeTest, IdentityCloneUndefineAndScopeReuse) {
  loom_op_t* alias = nullptr;
  IREE_ASSERT_OK(BuildIdentity(inputs_[0], &alias));
  IREE_ASSERT_OK(loom_value_fact_table_compute_op(&table_, module_, alias));
  const loom_value_id_t result = loom_op_const_results(alias)[0];
  loom_value_fact_table_t clone = {};
  IREE_ASSERT_OK(loom_value_fact_table_initialize(&clone, &arena_, 0));
  IREE_ASSERT_OK(loom_value_fact_table_clone_values(
      &clone, {&table_, &result, 1}, module_));
  EXPECT_EQ(loom_value_fact_table_query_identity(&clone, result), inputs_[0]);
  EXPECT_FALSE(loom_value_fact_table_has_entry(&clone, inputs_[0]));
  EXPECT_FALSE(loom_value_fact_table_has_entry(&clone, inputs_[1]));
  loom_value_fact_table_undefine(&table_, result);
  EXPECT_EQ(loom_value_fact_table_query_identity(&table_, result), result);
  bool changed = false;
  IREE_ASSERT_OK(loom_value_fact_table_compute_op_and_report(&table_, module_,
                                                             alias, &changed));
  EXPECT_TRUE(changed);
  EXPECT_EQ(loom_value_fact_table_query_identity(&table_, result), inputs_[0]);

  const auto* storage = table_.identities.entries;
  loom_value_fact_table_clear_scope(&table_);
  EXPECT_EQ(table_.identities.entries, storage);
  EXPECT_EQ(loom_value_fact_table_query_identity(&table_, result), result);
  EXPECT_EQ(loom_value_fact_table_query_identity(&clone, result), inputs_[0]);
  IREE_ASSERT_OK(loom_op_set_operand(module_, alias, 0, inputs_[1]));
  IREE_ASSERT_OK(loom_value_fact_table_compute_op(&table_, module_, alias));
  EXPECT_EQ(table_.identities.entries, storage);
  EXPECT_EQ(loom_value_fact_table_query_identity(&table_, result), inputs_[1]);
}

TEST_F(FactTableComputeTest, NumericEqualityDoesNotCreateSSAIdentity) {
  loom_op_t* first = nullptr;
  loom_op_t* second = nullptr;
  IREE_ASSERT_OK(loom_index_constant_build(&builder_, loom_attr_i64(7), type_,
                                           LOOM_LOCATION_UNKNOWN, &first));
  IREE_ASSERT_OK(loom_index_constant_build(&builder_, loom_attr_i64(7), type_,
                                           LOOM_LOCATION_UNKNOWN, &second));
  IREE_ASSERT_OK(loom_value_fact_table_compute_op(&table_, module_, first));
  IREE_ASSERT_OK(loom_value_fact_table_compute_op(&table_, module_, second));
  const loom_value_id_t first_result = loom_op_const_results(first)[0];
  const loom_value_id_t second_result = loom_op_const_results(second)[0];
  EXPECT_NE(loom_value_fact_table_query_identity(&table_, first_result),
            loom_value_fact_table_query_identity(&table_, second_result));
  EXPECT_EQ(table_.identities.entries, nullptr);
}

}  // namespace
}  // namespace loom
