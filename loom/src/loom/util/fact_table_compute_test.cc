// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/base/internal/arena.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/ir/context.h"
#include "loom/ir/module.h"
#include "loom/ops/index/ops.h"
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

TEST_F(FactTableComputeTest, IdentityCloneUndefineAndScopeReuse) {
  loom_op_t* alias = nullptr;
  IREE_ASSERT_OK(BuildIdentity(inputs_[0], &alias));
  IREE_ASSERT_OK(loom_value_fact_table_compute_op(&table_, module_, alias));
  const loom_value_id_t result = loom_op_const_results(alias)[0];
  loom_value_fact_table_t clone = {};
  IREE_ASSERT_OK(loom_value_fact_table_initialize(&clone, &arena_, 0));
  IREE_ASSERT_OK(
      loom_value_fact_table_clone_defined_facts(&clone, &table_, module_));
  EXPECT_EQ(loom_value_fact_table_query_identity(&clone, result), inputs_[0]);
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
