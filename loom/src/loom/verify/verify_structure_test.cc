// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/verify/verify_structure.h"

#include <cstdio>
#include <vector>

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/ir/context.h"
#include "loom/ir/module.h"
#include "loom/ops/test/ops.h"

namespace loom {
namespace {

// One operation exercises the dictionary representation API. Authored programs
// and bytecode round trips live in test/operand_dictionary.loom-test.
class OperandDictionaryTest : public ::testing::Test {
 protected:
  static iree_status_t Allocate(void* self, iree_allocator_command_t command,
                                const void* parameters, void** pointer) {
    auto* test = static_cast<OperandDictionaryTest*>(self);
    if (test->fail_allocations_ && command != IREE_ALLOCATOR_COMMAND_FREE) {
      return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                              "injected allocation failure");
    }
    const iree_allocator_t allocator = iree_allocator_system();
    return allocator.ctl(allocator.self, command, parameters, pointer);
  }

  void SetUp() override {
    iree_arena_block_pool_initialize(4096, iree_allocator_system(),
                                     &module_pool_);
    iree_arena_block_pool_initialize(4096, {this, Allocate}, &scratch_pool_);
    iree_arena_initialize(&scratch_pool_, &state_.arena);
    loom_context_initialize(iree_allocator_system(), &context_);
    iree_host_size_t count = 0;
    const loom_op_vtable_t* const* vtables = loom_test_dialect_vtables(&count);
    IREE_ASSERT_OK(loom_context_register_dialect(
        &context_, LOOM_DIALECT_TEST, vtables, static_cast<uint16_t>(count)));
    IREE_ASSERT_OK(loom_context_finalize(&context_));
    IREE_ASSERT_OK(loom_module_allocate(&context_, IREE_SV("dictionary"),
                                        &module_pool_, nullptr,
                                        iree_allocator_system(), &module_));
    loom_builder_initialize(module_, &module_->arena,
                            loom_module_block(module_), &builder_);
    IREE_ASSERT_OK(loom_module_define_value(
        module_, loom_type_scalar(LOOM_SCALAR_TYPE_F32), &input_));
    state_.module = module_;
    state_.result = &result_;
  }

  void TearDown() override {
    iree_arena_deinitialize(&state_.arena);
    iree_arena_block_pool_deinitialize(&scratch_pool_);
    loom_module_free(module_);
    loom_context_deinitialize(&context_);
    iree_arena_block_pool_deinitialize(&module_pool_);
  }

  loom_op_t* Dictionary(uint16_t count) {
    std::vector<loom_named_value_t> parameters(count);
    for (uint16_t i = 0; i < count; ++i) {
      char name[32];
      snprintf(name, sizeof(name), "parameter_%05u", static_cast<unsigned>(i));
      IREE_CHECK_OK(loom_builder_intern_string(
          &builder_, iree_make_cstring_view(name), &parameters[i].name_id));
      parameters[i].value_id = input_;
    }
    loom_op_t* op = nullptr;
    IREE_CHECK_OK(loom_test_operand_dict_build(
        &builder_, input_, parameters.data(), count,
        loom_type_scalar(LOOM_SCALAR_TYPE_F32), LOOM_LOCATION_UNKNOWN, &op));
    return op;
  }

  std::vector<loom_named_attr_t> Names(loom_op_t* op) {
    const auto names = loom_test_operand_dict_param_names(op);
    return {names.entries, names.entries + names.count};
  }

  void Check(loom_op_t* op, uint32_t expected_errors) {
    result_ = {};
    IREE_ASSERT_OK(
        loom_verify_operand_dicts(&state_, op, loom_op_vtable(module_, op)));
    EXPECT_EQ(result_.error_count, expected_errors);
  }

  // Allocation failure injection affects only verifier scratch.
  bool fail_allocations_ = false;
  // Arena blocks holding the operation fixture.
  iree_arena_block_pool_t module_pool_ = {};
  // Independent scratch pool so fixture construction cannot mask a failure.
  iree_arena_block_pool_t scratch_pool_ = {};
  // Registered production metadata for the test dialect.
  loom_context_t context_ = {};
  // Module owning the operation and interned dictionary keys.
  loom_module_t* module_ = nullptr;
  // Builder that establishes ordinary operation layout and operand segments.
  loom_builder_t builder_ = {};
  // Shared scalar operand; dictionary ordinals are independent of SSA identity.
  loom_value_id_t input_ = LOOM_VALUE_ID_INVALID;
  // Diagnostic counters without source rendering for malformed API fixtures.
  loom_verify_result_t result_ = {};
  // Production verifier scratch reused across calls.
  loom_verify_state_t state_ = {};
};

TEST_F(OperandDictionaryTest, PermutationsAndScratchReuse) {
  Check(Dictionary(0), 0);
  for (uint16_t count : {1, 63, 64, 65, 129, 128, 32, 129}) {
    SCOPED_TRACE(count);
    loom_op_t* op = Dictionary(count);
    auto names = Names(op);
    for (uint16_t i = 0; i < count; ++i) {
      names[i].value = loom_attr_i64(count - i - 1);
    }
    loom_op_attrs(op)[0] = loom_make_canonical_attr_dict(names.data(), count);
    const auto used_before = state_.arena.used_allocation_size;
    const auto capacity_before = state_.operand_dictionary.word_capacity;
    Check(op, 0);
    if (count <= 64 || loom_bitset_word_count(count) <= capacity_before) {
      EXPECT_EQ(state_.arena.used_allocation_size, used_before);
      EXPECT_EQ(state_.operand_dictionary.word_capacity, capacity_before);
    }
    Check(op, 0);
    for (uint16_t i = 0; i < count; ++i) {
      EXPECT_EQ(names[i].value.i64, count - i - 1);
    }
  }
}

TEST_F(OperandDictionaryTest, DuplicateOrdinalsAcrossWords) {
  for (uint16_t count : {2, 64, 65, 128, 129}) {
    SCOPED_TRACE(count);
    loom_op_t* op = Dictionary(count);
    auto names = Names(op);
    loom_op_attrs(op)[0] = loom_make_canonical_attr_dict(names.data(), count);
    for (uint16_t duplicate : {0, count - 2}) {
      names.back().value = loom_attr_i64(duplicate);
      Check(op, 1);
    }
    names.back().value = loom_attr_i64(count - 1);
    Check(op, 0);
  }
}

TEST_F(OperandDictionaryTest, EveryRepeatedOrdinalIsDiagnosed) {
  loom_op_t* op = Dictionary(65);
  auto names = Names(op);
  for (auto& entry : names) {
    entry.value = loom_attr_i64(64);
  }
  loom_op_attrs(op)[0] =
      loom_make_canonical_attr_dict(names.data(), names.size());
  Check(op, 64);
}

TEST_F(OperandDictionaryTest, InvalidOrdinalsDoNotClaimBits) {
  loom_op_t* op = Dictionary(65);
  auto names = Names(op);
  loom_op_attrs(op)[0] =
      loom_make_canonical_attr_dict(names.data(), names.size());
  names[0].value = loom_attr_f64(0.0);
  names[1].value = loom_attr_i64(-1);
  names[2].value = loom_attr_i64(INT64_MAX);
  names.back().value = loom_attr_i64(0);
  Check(op, 3);
  names[2].value = loom_attr_i64(names.size());
  Check(op, 3);
}

TEST_F(OperandDictionaryTest, InvalidKeyStillClaimsValidOrdinal) {
  loom_op_t* op = Dictionary(2);
  auto names = Names(op);
  loom_op_attrs(op)[0] =
      loom_make_canonical_attr_dict(names.data(), names.size());
  names.back().value = loom_attr_i64(0);
  names[0].name_id = LOOM_STRING_ID_INVALID;
  Check(op, 2);
  names[0].name_id = module_->strings.count;
  Check(op, 2);
}

TEST_F(OperandDictionaryTest, AllocationFailurePreservesScratch) {
  fail_allocations_ = true;
  Check(Dictionary(64), 0);
  EXPECT_EQ(state_.arena.used_allocation_size, 0u);

  loom_op_t* op = Dictionary(65);
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_RESOURCE_EXHAUSTED,
      loom_verify_operand_dicts(&state_, op, loom_op_vtable(module_, op)));
  EXPECT_EQ(state_.operand_dictionary.bits, nullptr);
  EXPECT_EQ(state_.operand_dictionary.word_capacity, 0u);
  fail_allocations_ = false;
  Check(op, 0);

  // Consume the current block so growing scratch requires an allocator call.
  void* unused = nullptr;
  IREE_ASSERT_OK(iree_arena_allocate(
      &state_.arena, state_.arena.block_bytes_remaining, &unused));
  auto* bits = state_.operand_dictionary.bits;
  const auto capacity = state_.operand_dictionary.word_capacity;
  fail_allocations_ = true;
  op = Dictionary(129);
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_RESOURCE_EXHAUSTED,
      loom_verify_operand_dicts(&state_, op, loom_op_vtable(module_, op)));
  EXPECT_EQ(state_.operand_dictionary.bits, bits);
  EXPECT_EQ(state_.operand_dictionary.word_capacity, capacity);
  Check(Dictionary(65), 0);
  fail_allocations_ = false;
  Check(op, 0);
}

TEST_F(OperandDictionaryTest, AllocationFailurePreservesPendingDiagnostic) {
  loom_op_t* op = Dictionary(65);
  fail_allocations_ = true;
  state_.diagnostic_status =
      iree_make_status(IREE_STATUS_ABORTED, "injected diagnostic sink failure");
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_ABORTED,
      loom_verify_operand_dicts(&state_, op, loom_op_vtable(module_, op)));
  IREE_EXPECT_OK(loom_verify_take_diagnostic_status(&state_));
  EXPECT_EQ(state_.operand_dictionary.bits, nullptr);
  EXPECT_EQ(state_.operand_dictionary.word_capacity, 0u);
}

}  // namespace
}  // namespace loom
