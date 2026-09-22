// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/analysis/conditioned_value_facts.h"

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/ir/context.h"
#include "loom/ir/module.h"
#include "loom/ops/cfg/ops.h"
#include "loom/ops/scalar/ops.h"
#include "loom/ops/test/ops.h"
#include "loom/pass/value_facts.h"

namespace loom {
namespace {

class ConditionedValueFactsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    iree_arena_block_pool_initialize(4096, iree_allocator_system(), &pool_);
    loom_context_initialize(iree_allocator_system(), &context_);
    RegisterDialect(LOOM_DIALECT_TEST, loom_test_dialect_vtables);
    RegisterDialect(LOOM_DIALECT_CFG, loom_cfg_dialect_vtables);
    RegisterDialect(LOOM_DIALECT_SCALAR, loom_scalar_dialect_vtables);
    IREE_ASSERT_OK(loom_context_finalize(&context_));
    IREE_ASSERT_OK(loom_module_allocate(&context_, IREE_SV("conditions"),
                                        &pool_, nullptr,
                                        iree_allocator_system(), &module_));
    loom_builder_initialize(module_, &module_->arena,
                            loom_module_block(module_), &builder_);
    loom_string_id_t name = LOOM_STRING_ID_INVALID;
    IREE_ASSERT_OK(
        loom_builder_intern_string(&builder_, IREE_SV("count"), &name));
    uint16_t symbol = LOOM_SYMBOL_ID_INVALID;
    IREE_ASSERT_OK(loom_module_add_symbol(module_, name, &symbol));
    loom_op_t* function = nullptr;
    IREE_ASSERT_OK(loom_test_func_build(&builder_, 0, 0, 0, {0, symbol}, &i32_,
                                        1, nullptr, 0, nullptr, 0, nullptr, 0,
                                        LOOM_LOCATION_UNKNOWN, &function));
    function_ = loom_func_like_cast(module_, function);
    body_ = loom_func_like_body(function_);
    body_->flags |= LOOM_REGION_INSTANCE_FLAG_CFG;
    SetBlock(loom_region_entry_block(body_));
    input_ = loom_block_arg_id(builder_.ip.block, 0);
    loom_pass_value_fact_owner_initialize(&pool_, &owner_);
  }

  void TearDown() override {
    loom_pass_value_fact_owner_deinitialize(&owner_);
    loom_module_free(module_);
    loom_context_deinitialize(&context_);
    iree_arena_block_pool_deinitialize(&pool_);
  }

  void RegisterDialect(loom_dialect_id_t id, const loom_op_vtable_t* const* (
                                                 *dialect)(iree_host_size_t*)) {
    iree_host_size_t count = 0;
    const loom_op_vtable_t* const* vtables = dialect(&count);
    IREE_ASSERT_OK(
        loom_context_register_dialect(&context_, id, vtables, (uint16_t)count));
  }

  loom_block_t* AppendBlock() {
    loom_block_t* block = nullptr;
    IREE_CHECK_OK(loom_region_append_block(module_, body_, &block));
    return block;
  }

  void SetBlock(loom_block_t* block) {
    loom_builder_set_block(&builder_, block);
    builder_.ip.parent_op = function_.op;
  }

  loom_value_id_t Constant(int64_t value) {
    loom_op_t* op = nullptr;
    IREE_CHECK_OK(loom_scalar_constant_build(&builder_, loom_attr_i64(value),
                                             i32_, LOOM_LOCATION_UNKNOWN, &op));
    return loom_scalar_constant_result(op);
  }

  loom_value_id_t Count(loom_value_id_t value) {
    loom_op_t* op = nullptr;
    IREE_CHECK_OK(loom_scalar_cttzi_build(&builder_, value, i32_,
                                          LOOM_LOCATION_UNKNOWN, &op));
    return loom_scalar_cttzi_result(op);
  }

  loom_op_t* Guard(loom_value_id_t value, loom_value_id_t zero,
                   loom_block_t* live, loom_block_t* empty) {
    loom_op_t* compare = nullptr;
    IREE_CHECK_OK(
        loom_scalar_cmpi_build(&builder_, LOOM_SCALAR_CMPI_PREDICATE_NE, value,
                               zero, LOOM_LOCATION_UNKNOWN, &compare));
    loom_op_t* branch = nullptr;
    IREE_CHECK_OK(
        loom_cfg_cond_br_build(&builder_, loom_scalar_cmpi_result(compare),
                               live, empty, LOOM_LOCATION_UNKNOWN, &branch));
    return compare;
  }

  void Branch(loom_block_t* destination,
              const loom_value_id_t* argument = nullptr) {
    loom_op_t* op = nullptr;
    IREE_ASSERT_OK(loom_cfg_br_build(&builder_, destination, argument,
                                     argument ? 1 : 0, LOOM_LOCATION_UNKNOWN,
                                     &op));
  }

  void Yield() {
    loom_op_t* op = nullptr;
    IREE_ASSERT_OK(loom_test_yield_build(&builder_, nullptr, 0,
                                         LOOM_LOCATION_UNKNOWN, &op));
  }

  loom_value_fact_table_t* Acquire(loom_pass_value_fact_scope_kind_t kind) {
    auto scope = loom_pass_value_fact_scope_function(function_);
    scope.kind = kind;
    loom_value_fact_table_t* table = nullptr;
    IREE_CHECK_OK(
        loom_pass_value_fact_owner_acquire(&owner_, module_, scope, &table));
    return table;
  }

  static void ExpectRange(const loom_value_fact_table_t* table,
                          loom_value_id_t value, int64_t low, int64_t high) {
    const auto facts = loom_value_fact_table_lookup(table, value);
    EXPECT_EQ(facts.range_lo, low);
    EXPECT_EQ(facts.range_hi, high);
  }

  // Shared allocation pool outliving module and analysis storage.
  iree_arena_block_pool_t pool_ = {};
  // Dialect registry for the minimal function fixture.
  loom_context_t context_ = {};
  // Owned IR containing one function with an arbitrary i32 input.
  loom_module_t* module_ = nullptr;
  // Builder whose insertion point follows each test's CFG.
  loom_builder_t builder_ = {};
  // Function retained for scope acquisition.
  loom_func_like_t function_ = {};
  // Function body containing the test's CFG.
  loom_region_t* body_ = nullptr;
  // Argument with no authored range or nonzero assumptions.
  loom_value_id_t input_ = LOOM_VALUE_ID_INVALID;
  // Reusable owner whose scope transitions are part of the contract under test.
  loom_pass_value_fact_owner_t owner_ = {};
  // Scalar type used by the signature, counts, and loop-carried value.
  const loom_type_t i32_ = loom_type_scalar(LOOM_SCALAR_TYPE_I32);
};

TEST_F(ConditionedValueFactsTest,
       GuardedDefinitionsDoNotStrengthenOtherScopes) {
  auto* live = AppendBlock();
  auto* empty = AppendBlock();
  auto* join = AppendBlock();
  auto* guarded = AppendBlock();
  auto* guard = Guard(input_, Constant(0), live, empty);
  SetBlock(live);
  Branch(guarded);
  SetBlock(guarded);
  const auto live_count = Count(input_);
  Branch(join);
  SetBlock(empty);
  const auto empty_count = Count(input_);
  Branch(join);
  SetBlock(join);
  const auto joined_count = Count(input_);
  Yield();

  loom_pass_value_fact_lifecycle_counts_t counts = {};
  owner_.lifecycle_counts = &counts;
  auto* table = Acquire(LOOM_PASS_VALUE_FACT_SCOPE_FUNCTION);
  const auto input_facts = loom_value_fact_table_lookup(table, input_);
  ExpectRange(table, live_count, 0, 32);
  table = Acquire(LOOM_PASS_VALUE_FACT_SCOPE_CONDITIONED_FUNCTION);
  EXPECT_EQ(counts.scope_clear_count, 0u);
  ExpectRange(table, live_count, 0, 31);
  ExpectRange(table, empty_count, 32, 32);
  ExpectRange(table, joined_count, 0, 32);
  EXPECT_TRUE(loom_value_facts_equal(
      input_facts, loom_value_fact_table_lookup(table, input_)));

  // A different scope kind must not reuse the stronger cached result.
  table = Acquire(LOOM_PASS_VALUE_FACT_SCOPE_FUNCTION);
  ExpectRange(table, live_count, 0, 32);
  ExpectRange(table, empty_count, 0, 32);
  table = Acquire(LOOM_PASS_VALUE_FACT_SCOPE_CONDITIONED_FUNCTION);
  ExpectRange(table, live_count, 0, 31);
  loom_pass_value_fact_owner_invalidate(&owner_);
  loom_op_attrs(guard)[loom_scalar_cmpi_predicate_ATTR_INDEX] =
      loom_attr_enum(LOOM_SCALAR_CMPI_PREDICATE_EQ);
  table = Acquire(LOOM_PASS_VALUE_FACT_SCOPE_CONDITIONED_FUNCTION);
  ExpectRange(table, live_count, 32, 32);
  ExpectRange(table, empty_count, 0, 31);
  ExpectRange(table, joined_count, 0, 32);
  owner_.lifecycle_counts = nullptr;
}

TEST_F(ConditionedValueFactsTest, BackedgeKeepsItsGuardOnTheCurrentIteration) {
  auto* header = AppendBlock();
  auto* live = AppendBlock();
  auto* exit = AppendBlock();
  loom_value_id_t mask = LOOM_VALUE_ID_INVALID;
  IREE_ASSERT_OK(loom_module_define_value(module_, i32_, &mask));
  IREE_ASSERT_OK(loom_block_add_arg(module_, header, mask));
  const auto zero = Constant(0);
  const auto one = Constant(1);
  Branch(header, &input_);
  SetBlock(header);
  Guard(mask, zero, live, exit);
  SetBlock(live);
  const auto live_count = Count(mask);
  loom_op_t* shift = nullptr;
  IREE_ASSERT_OK(loom_scalar_shrui_build(&builder_, mask, one, i32_,
                                         LOOM_LOCATION_UNKNOWN, &shift));
  const auto next_mask = loom_scalar_shrui_result(shift);
  Branch(header, &next_mask);
  SetBlock(exit);
  const auto exit_count = Count(mask);
  const auto original_count = Count(input_);
  Yield();

  const auto* table = Acquire(LOOM_PASS_VALUE_FACT_SCOPE_CONDITIONED_FUNCTION);
  ExpectRange(table, live_count, 0, 31);
  ExpectRange(table, exit_count, 32, 32);
  ExpectRange(table, original_count, 0, 32);
  EXPECT_FALSE(
      loom_value_facts_is_non_zero(loom_value_fact_table_lookup(table, mask)));
}

TEST_F(ConditionedValueFactsTest, ParallelOutcomesDoNotEstablishAGuard) {
  auto* join = AppendBlock();
  Guard(input_, Constant(0), join, join);
  SetBlock(join);
  const auto count = Count(input_);
  Yield();
  ExpectRange(Acquire(LOOM_PASS_VALUE_FACT_SCOPE_CONDITIONED_FUNCTION), count,
              0, 32);
}

}  // namespace
}  // namespace loom
