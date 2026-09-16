// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/ir/value_refs.h"

#include <array>

#include "iree/base/internal/arena.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/ir/context.h"
#include "loom/ir/module.h"
#include "loom/ops/test/ops.h"
#include "loom/ops/test/types.h"

namespace loom {
namespace {

class ValueRefsTest : public ::testing::Test {
 protected:
  static iree_status_t Allocate(void* self, iree_allocator_command_t command,
                                const void* parameters, void** pointer) {
    auto* test = static_cast<ValueRefsTest*>(self);
    if (test->fail_allocations_ && command != IREE_ALLOCATOR_COMMAND_FREE) {
      ++test->failed_allocations_;
      return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                              "injected allocation failure");
    }
    const iree_allocator_t allocator = iree_allocator_system();
    return allocator.ctl(allocator.self, command, parameters, pointer);
  }

  void SetUp() override {
    iree_arena_block_pool_initialize(4096, {this, Allocate}, &pool_);
    loom_context_initialize(iree_allocator_system(), &context_);
    iree_host_size_t count = 0;
    const loom_op_vtable_t* const* vtables = loom_test_dialect_vtables(&count);
    IREE_ASSERT_OK(loom_context_register_dialect(
        &context_, LOOM_DIALECT_TEST, vtables, static_cast<uint16_t>(count)));
    IREE_ASSERT_OK(loom_context_finalize(&context_));
    IREE_ASSERT_OK(loom_module_allocate(&context_, IREE_SV("references"),
                                        &pool_, nullptr,
                                        iree_allocator_system(), &module_));
    loom_builder_initialize(module_, &module_->arena,
                            loom_module_block(module_), &builder_);
  }

  void TearDown() override {
    loom_module_free(module_);
    loom_context_deinitialize(&context_);
    iree_arena_block_pool_deinitialize(&pool_);
  }

  loom_value_id_t Constant(int64_t value) {
    loom_op_t* op = nullptr;
    IREE_CHECK_OK(loom_test_constant_build(
        &builder_, loom_attr_i64(value),
        loom_type_scalar(LOOM_SCALAR_TYPE_INDEX), LOOM_LOCATION_UNKNOWN, &op));
    return loom_test_constant_result(op);
  }

  static loom_predicate_t Predicate(loom_value_id_t value) {
    return {LOOM_PREDICATE_EQ,
            2,
            {LOOM_PRED_ARG_VALUE, LOOM_PRED_ARG_VALUE},
            {},
            {value, value}};
  }

  loom_op_t* Assume(loom_value_id_t value) {
    const loom_type_t type = loom_type_scalar(LOOM_SCALAR_TYPE_INDEX);
    loom_predicate_t predicate = Predicate(value);
    loom_op_t* op = nullptr;
    IREE_CHECK_OK(loom_test_assume_build(&builder_, &value, 1, &predicate, 1,
                                         &type, 1, LOOM_LOCATION_UNKNOWN, &op));
    return op;
  }

  uint32_t IncomingCount(loom_value_id_t value) {
    const auto* heads = loom_module_value_attribute_use_heads(module_, value);
    uint32_t count = 0;
    for (loom_attribute_use_id_t first : {heads->type, heads->predicate}) {
      for (loom_attribute_use_id_t id = first; id;
           id = module_->attribute_uses.records[id - 1].next_incoming) {
        ++count;
      }
    }
    return count;
  }

  bool HasUses(loom_value_id_t value) {
    return loom_value_has_attribute_uses(loom_module_value(module_, value));
  }

  // Allocation failure injection for module arena growth, not the subject API.
  bool fail_allocations_ = false;
  // Number of allocator calls rejected during failure injection.
  uint32_t failed_allocations_ = 0;
  // Backing blocks shared by the module and temporary API fixtures.
  iree_arena_block_pool_t pool_ = {};
  // Minimal registered test-dialect context.
  loom_context_t context_ = {};
  // Module whose reference ownership is under test.
  loom_module_t* module_ = nullptr;
  // Production builder used to finalize fixture operations.
  loom_builder_t builder_ = {};
};

TEST_F(ValueRefsTest, DuplicateAndSharedOwnersUnlinkExactly) {
  const loom_value_id_t original = Constant(1);
  const loom_value_id_t replacement = Constant(2);
  loom_op_t* first = Assume(original);
  loom_op_t* middle = Assume(original);
  loom_op_t* last = Assume(original);
  EXPECT_EQ(IncomingCount(original), 6u);

  IREE_ASSERT_OK(loom_op_erase(module_, middle));
  EXPECT_EQ(IncomingCount(original), 4u);
  loom_predicate_t predicate = Predicate(replacement);
  IREE_ASSERT_OK(loom_op_set_attr(module_, first, 0,
                                  loom_attr_predicate_list(&predicate, 1)));
  EXPECT_EQ(IncomingCount(original), 2u);
  EXPECT_EQ(IncomingCount(replacement), 2u);
  IREE_ASSERT_OK(loom_op_erase(module_, last));
  EXPECT_FALSE(HasUses(original));
  EXPECT_TRUE(HasUses(replacement));

  IREE_ASSERT_OK(
      loom_value_replace_all_uses_with(module_, replacement, original));
  const auto attribute = loom_op_const_attrs(first)[0];
  EXPECT_EQ(attribute.predicate_list[0].args[0], original);
  EXPECT_EQ(attribute.predicate_list[0].args[1], original);
  EXPECT_EQ(IncomingCount(original), 2u);
  EXPECT_FALSE(HasUses(replacement));
  IREE_ASSERT_OK(loom_op_erase(module_, first));
  EXPECT_FALSE(HasUses(original));
}

TEST_F(ValueRefsTest, BulkRebuildReplacesOldOwnersAndReusesStorage) {
  const loom_value_id_t original = Constant(1);
  const loom_value_id_t replacement = Constant(2);
  loom_op_t* owner = Assume(original);
  loom_predicate_t predicate = Predicate(replacement);
  // Bulk readers populate payloads directly before their use-def rebuild.
  loom_op_attrs(owner)[0] = loom_attr_predicate_list(&predicate, 1);
  const auto* storage = module_->attribute_uses.records;
  IREE_ASSERT_OK(loom_module_compute_uses(module_));
  EXPECT_EQ(module_->attribute_uses.records, storage);
  EXPECT_EQ(module_->attribute_uses.count, 2u);
  EXPECT_FALSE(HasUses(original));
  EXPECT_EQ(IncomingCount(replacement), 2u);
  IREE_ASSERT_OK(loom_module_compute_uses(module_));
  EXPECT_EQ(module_->attribute_uses.count, 2u);
  EXPECT_EQ(IncomingCount(replacement), 2u);
  IREE_ASSERT_OK(loom_op_erase(module_, owner));
  EXPECT_FALSE(HasUses(replacement));
}

TEST_F(ValueRefsTest, FailedPayloadWalkPreservesOldAttributeAndIndex) {
  const loom_value_id_t original = Constant(1);
  const loom_value_id_t replacement = Constant(2);
  loom_op_t* owner = Assume(original);
  const auto old_attribute = loom_op_const_attrs(owner)[0];
  loom_predicate_t predicate = Predicate(replacement);
  loom_string_id_t predicate_key = LOOM_STRING_ID_INVALID;
  loom_string_id_t type_key = LOOM_STRING_ID_INVALID;
  IREE_ASSERT_OK(
      loom_module_intern_string(module_, IREE_SV("predicate"), &predicate_key));
  IREE_ASSERT_OK(
      loom_module_intern_string(module_, IREE_SV("type"), &type_key));
  const loom_named_attr_t entries[] = {
      {predicate_key, {}, loom_attr_predicate_list(&predicate, 1)},
      {type_key, {}, loom_attr_type(LOOM_TYPE_ID_INVALID)},
  };
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      loom_module_set_op_attribute(module_, owner, 0,
                                   loom_make_canonical_attr_dict(entries, 2)));
  EXPECT_TRUE(
      loom_attribute_equal(&old_attribute, &loom_op_const_attrs(owner)[0]));
  EXPECT_EQ(IncomingCount(original), 2u);
  EXPECT_EQ(IncomingCount(replacement), 0u);
  EXPECT_FALSE(HasUses(replacement));
  // The partially built records are recycled, so the retry needs no growth.
  const uint32_t count = module_->attribute_uses.count;
  IREE_ASSERT_OK(loom_op_set_attr(module_, owner, 0,
                                  loom_attr_predicate_list(&predicate, 1)));
  EXPECT_EQ(module_->attribute_uses.count, count);
  EXPECT_FALSE(HasUses(original));
  EXPECT_EQ(IncomingCount(replacement), 2u);
}

TEST_F(ValueRefsTest, ScalarRefreshDropsFormerReferenceRecords) {
  const loom_value_id_t value = Constant(1);
  loom_predicate_t predicate = Predicate(value);
  loom_string_id_t key = LOOM_STRING_ID_INVALID;
  IREE_ASSERT_OK(
      loom_module_intern_string(module_, IREE_SV("constraint"), &key));
  const loom_named_attr_t attributes[] = {
      {key, {}, loom_attr_predicate_list(&predicate, 1)},
  };
  loom_op_t* owner = nullptr;
  IREE_ASSERT_OK(loom_test_attrs_build(
      &builder_, LOOM_TEST_ATTRS_BUILD_FLAG_HAS_DICT, value,
      loom_make_named_attr_slice(attributes, 1),
      loom_type_scalar(LOOM_SCALAR_TYPE_INDEX), LOOM_LOCATION_UNKNOWN, &owner));
  EXPECT_EQ(IncomingCount(value), 2u);
  loom_op_attrs(owner)[loom_test_attrs_dict_ATTR_INDEX] = loom_attr_absent();
  IREE_ASSERT_OK(loom_module_refresh_op_attribute_uses(module_, owner));
  EXPECT_FALSE(HasUses(value));
  EXPECT_EQ(IncomingCount(value), 0u);
  const auto record_count = module_->attribute_uses.count;
  IREE_ASSERT_OK(loom_module_refresh_op_attribute_uses(module_, owner));
  EXPECT_EQ(module_->attribute_uses.count, record_count);
  IREE_ASSERT_OK(loom_op_erase(module_, owner));
}

TEST_F(ValueRefsTest, FailedIndexGrowthPreservesOldOwnersAndCanRetry) {
  const loom_value_id_t original = Constant(1);
  const loom_value_id_t replacement = Constant(2);
  loom_op_t* owner = Assume(original);
  const auto old_attribute = loom_op_const_attrs(owner)[0];
  std::array<loom_predicate_t, 128> predicates;
  predicates.fill(Predicate(replacement));
  const auto attribute =
      loom_attr_predicate_list(predicates.data(), predicates.size());
  fail_allocations_ = true;
  iree_status_t status = loom_op_set_attr(module_, owner, 0, attribute);
  fail_allocations_ = false;
  IREE_EXPECT_STATUS_IS(IREE_STATUS_RESOURCE_EXHAUSTED, status);
  EXPECT_GT(failed_allocations_, 0u);
  EXPECT_TRUE(
      loom_attribute_equal(&old_attribute, &loom_op_const_attrs(owner)[0]));
  EXPECT_EQ(IncomingCount(original), 2u);
  EXPECT_FALSE(HasUses(replacement));
  IREE_ASSERT_OK(loom_op_set_attr(module_, owner, 0, attribute));
  EXPECT_FALSE(HasUses(original));
  EXPECT_EQ(IncomingCount(replacement), 256u);
  IREE_ASSERT_OK(
      loom_value_replace_all_uses_with(module_, replacement, original));
  EXPECT_FALSE(HasUses(replacement));
  EXPECT_EQ(IncomingCount(original), 256u);
  const auto updated = loom_op_const_attrs(owner)[0];
  for (uint16_t i = 0; i < updated.count; ++i) {
    EXPECT_EQ(updated.predicate_list[i].args[0], original);
    EXPECT_EQ(updated.predicate_list[i].args[1], original);
  }
}

TEST_F(ValueRefsTest, RepackingResultsPreservesAttributeOwnership) {
  loom_string_id_t name = LOOM_STRING_ID_INVALID;
  uint16_t symbol = 0;
  IREE_ASSERT_OK(
      loom_module_intern_string(module_, IREE_SV("function"), &name));
  IREE_ASSERT_OK(loom_module_add_symbol(module_, name, &symbol));
  const loom_type_t index = loom_type_scalar(LOOM_SCALAR_TYPE_INDEX);
  const loom_type_t results[] = {index, index, index};
  loom_op_t* function = nullptr;
  IREE_ASSERT_OK(loom_test_func_build(&builder_, 0, 0, 0, {0, symbol}, &index,
                                      1, results, 3, nullptr, 0, nullptr, 0,
                                      LOOM_LOCATION_UNKNOWN, &function));
  const loom_value_id_t argument = loom_block_arg_id(
      loom_region_entry_block(loom_test_func_body(function)), 0);
  loom_predicate_t predicate = Predicate(argument);
  IREE_ASSERT_OK(loom_op_set_attr(module_, function,
                                  loom_test_func_predicates_ATTR_INDEX,
                                  loom_attr_predicate_list(&predicate, 1)));
  const auto* old_attributes = loom_op_const_attrs(function);
  const bool remove[] = {false, true, false};
  uint16_t removed_count = 0;
  iree_arena_allocator_t scratch = {};
  iree_arena_initialize(&pool_, &scratch);
  iree_status_t status = loom_op_remove_results(module_, function, remove,
                                                &scratch, &removed_count);
  iree_arena_deinitialize(&scratch);
  IREE_ASSERT_OK(status);
  EXPECT_EQ(removed_count, 1u);
  EXPECT_NE(loom_op_const_attrs(function), old_attributes);
  EXPECT_EQ(IncomingCount(argument), 2u);
  IREE_ASSERT_OK(loom_op_set_attr(module_, function,
                                  loom_test_func_predicates_ATTR_INDEX,
                                  loom_attr_absent()));
  EXPECT_FALSE(HasUses(argument));
}

TEST_F(ValueRefsTest,
       PredicateNestedInsideTypeKeepsTypeReferenceClassification) {
  const loom_value_id_t original = Constant(1);
  const loom_value_id_t replacement = Constant(2);
  loom_predicate_t predicate = Predicate(original);
  loom_string_id_t key = LOOM_STRING_ID_INVALID;
  IREE_ASSERT_OK(
      loom_module_intern_string(module_, IREE_SV("constraint"), &key));
  const loom_named_attr_t metadata[] = {
      {key, {}, loom_attr_predicate_list(&predicate, 1)},
  };
  loom_type_id_t element_type = LOOM_TYPE_ID_INVALID;
  IREE_ASSERT_OK(loom_module_intern_type_id(
      module_, loom_type_scalar(LOOM_SCALAR_TYPE_INDEX), &element_type));
  loom_type_t type = {};
  IREE_ASSERT_OK(loom_test_array_type_make(
      module_, LOOM_TEST_ARRAY_TYPE_BUILD_FLAG_HAS_METADATA, element_type, 0,
      loom_make_named_attr_slice(metadata, 1), &type));
  loom_type_id_t type_id = LOOM_TYPE_ID_INVALID;
  IREE_ASSERT_OK(loom_module_intern_type_id(module_, type, &type_id));
  const loom_named_attr_t attributes[] = {{key, {}, loom_attr_type(type_id)}};
  loom_op_t* owner = nullptr;
  IREE_ASSERT_OK(loom_test_attrs_build(
      &builder_, LOOM_TEST_ATTRS_BUILD_FLAG_HAS_DICT, original,
      loom_make_named_attr_slice(attributes, 1),
      loom_type_scalar(LOOM_SCALAR_TYPE_INDEX), LOOM_LOCATION_UNKNOWN, &owner));
  EXPECT_TRUE(HasUses(original));
  EXPECT_FALSE(
      loom_module_value_has_predicate_attribute_uses(module_, original));
  IREE_ASSERT_OK(
      loom_value_replace_all_uses_with(module_, original, replacement));
  EXPECT_FALSE(HasUses(original));
  EXPECT_EQ(IncomingCount(replacement), 2u);
  EXPECT_FALSE(
      loom_module_value_has_predicate_attribute_uses(module_, replacement));
  const auto dictionary = loom_test_attrs_dict(owner);
  const auto updated_type =
      module_->types.entries[dictionary.entries[0].value.type_id];
  const auto updated_metadata = loom_test_array_type_metadata(updated_type);
  EXPECT_EQ(updated_metadata.entries[0].value.predicate_list[0].args[0],
            replacement);
  IREE_ASSERT_OK(loom_op_erase(module_, owner));
  EXPECT_FALSE(HasUses(replacement));
}

}  // namespace
}  // namespace loom
