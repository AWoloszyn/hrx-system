// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/ir/value_refs.h"

#include <array>
#include <set>
#include <vector>

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
    loom_type_use_iterator_t users;
    loom_attribute_users_begin(&module_->type_uses, value, &users);
    uint32_t count = 0;
    std::set<std::pair<loom_op_t*, uint8_t>> owners;
    for (auto user = loom_attribute_users_next(&users); user.op;
         user = loom_attribute_users_next(&users)) {
      EXPECT_LT(user.attribute_index, user.op->attribute_count);
      EXPECT_TRUE(owners.emplace(user.op, user.attribute_index).second);
      ++count;
    }
    return count;
  }

  std::vector<loom_value_id_t> Outgoing(loom_op_t* op,
                                        uint8_t attribute_index) {
    std::vector<loom_value_id_t> result;
    loom_type_use_iterator_t dependencies;
    loom_attribute_dependencies_begin(&module_->type_uses, op, attribute_index,
                                      &dependencies);
    for (auto provider = loom_type_dependencies_next(&dependencies);
         provider != LOOM_VALUE_ID_INVALID;
         provider = loom_type_dependencies_next(&dependencies)) {
      result.push_back(provider);
    }
    return result;
  }

  bool HasUses(loom_value_id_t value) {
    return loom_value_has_attribute_uses(loom_module_value(module_, value));
  }

  loom_op_t* TypeOwner(loom_value_id_t input, loom_type_t type) {
    loom_type_id_t type_id;
    IREE_CHECK_OK(loom_module_intern_type_id(module_, type, &type_id));
    loom_string_id_t key;
    IREE_CHECK_OK(loom_module_intern_string(module_, IREE_SV("shape"), &key));
    const loom_named_attr_t attributes[] = {{key, {}, loom_attr_type(type_id)}};
    loom_op_t* owner = nullptr;
    IREE_CHECK_OK(loom_test_attrs_build(
        &builder_, LOOM_TEST_ATTRS_BUILD_FLAG_HAS_DICT, input,
        loom_make_named_attr_slice(attributes, 1),
        loom_type_scalar(LOOM_SCALAR_TYPE_INDEX), LOOM_LOCATION_NONE, &owner));
    return owner;
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

TEST_F(ValueRefsTest, ReplacesReferenceAfterFullFunctionArgumentSequence) {
  const auto index_type = loom_type_scalar(LOOM_SCALAR_TYPE_INDEX);
  loom_value_id_t source_dimension = LOOM_VALUE_ID_INVALID;
  loom_value_id_t target_dimension = LOOM_VALUE_ID_INVALID;
  IREE_ASSERT_OK(
      loom_module_define_value(module_, index_type, &source_dimension));
  IREE_ASSERT_OK(
      loom_module_define_value(module_, index_type, &target_dimension));
  std::vector<loom_type_t> arguments(UINT16_MAX, index_type);
  const auto result_type =
      loom_type_shaped_1d(LOOM_TYPE_VECTOR, LOOM_SCALAR_TYPE_F32,
                          loom_dim_pack_dynamic(source_dimension), 0);
  loom_type_t original = {};
  IREE_ASSERT_OK(loom_module_intern_function_type(
      module_, arguments.data(), UINT16_MAX, &result_type, 1, &original));
  loom_type_t replaced = {};
  bool changed = false;
  IREE_ASSERT_OK(loom_module_replace_type_value_references(
      module_, original, source_dimension, target_dimension, &replaced,
      &changed));

  EXPECT_TRUE(changed);
  ASSERT_EQ(loom_type_func_arg_count(replaced), UINT16_MAX);
  ASSERT_EQ(loom_type_func_result_count(replaced), 1u);
  EXPECT_EQ(
      loom_type_dim_value_id_at(loom_type_func_result_types(replaced)[0], 0),
      target_dimension);
  EXPECT_EQ(
      loom_type_dim_value_id_at(loom_type_func_result_types(original)[0], 0),
      source_dimension);
}

TEST_F(ValueRefsTest, SubtreeWalkIncludesOperandTypeAndPredicateAttributes) {
  const loom_value_id_t input = Constant(1);
  const loom_value_id_t width = Constant(16);
  const loom_value_id_t bound = Constant(64);
  const loom_type_t type =
      loom_type_shaped_1d(LOOM_TYPE_VECTOR, LOOM_SCALAR_TYPE_INDEX,
                          loom_dim_pack_dynamic(width), 0);
  loom_type_id_t type_id = LOOM_TYPE_ID_INVALID;
  IREE_ASSERT_OK(loom_module_intern_type_id(module_, type, &type_id));
  loom_string_id_t constraint_key = LOOM_STRING_ID_INVALID;
  loom_string_id_t shape_key = LOOM_STRING_ID_INVALID;
  IREE_ASSERT_OK(loom_module_intern_string(module_, IREE_SV("constraint"),
                                           &constraint_key));
  IREE_ASSERT_OK(
      loom_module_intern_string(module_, IREE_SV("shape"), &shape_key));
  loom_predicate_t predicate = Predicate(bound);
  const loom_named_attr_t attributes[] = {
      {constraint_key, {}, loom_attr_predicate_list(&predicate, 1)},
      {shape_key, {}, loom_attr_type(type_id)},
  };
  loom_op_t* owner = nullptr;
  IREE_ASSERT_OK(loom_test_attrs_build(
      &builder_, LOOM_TEST_ATTRS_BUILD_FLAG_HAS_DICT, input,
      loom_make_named_attr_slice(attributes, 2),
      loom_type_scalar(LOOM_SCALAR_TYPE_INDEX), LOOM_LOCATION_UNKNOWN, &owner));

  std::vector<uint32_t> visits(module_->values.count, 0);
  const iree_host_size_t allocation_size = module_->arena.used_allocation_size;
  fail_allocations_ = true;
  iree_status_t status = loom_op_walk_subtree_value_refs(
      module_, owner,
      [](loom_value_id_t value, void* user_data) {
        ++(*static_cast<std::vector<uint32_t>*>(user_data))[value];
        return iree_ok_status();
      },
      &visits);
  fail_allocations_ = false;
  IREE_ASSERT_OK(status);
  EXPECT_EQ(visits[input], 1u);
  EXPECT_EQ(visits[width], 1u);
  EXPECT_EQ(visits[bound], 1u);
  EXPECT_EQ(visits[loom_test_attrs_result(owner)], 0u);
  EXPECT_EQ(module_->arena.used_allocation_size, allocation_size);
  EXPECT_EQ(failed_allocations_, 0u);
}

TEST_F(ValueRefsTest, SubtreeWalkIncludesDeclarationArgumentTypes) {
  const loom_value_id_t width = Constant(16);
  const loom_type_t storage_type = loom_type_pool(loom_dim_pack_dynamic(width));
  loom_string_id_t name = LOOM_STRING_ID_INVALID;
  IREE_ASSERT_OK(loom_module_intern_string(module_, IREE_SV("storage"), &name));
  uint16_t symbol = LOOM_SYMBOL_ID_INVALID;
  IREE_ASSERT_OK(loom_module_add_symbol(module_, name, &symbol));
  loom_op_t* declaration = nullptr;
  IREE_ASSERT_OK(loom_test_decl_build(
      &builder_, /*build_flags=*/0, /*visibility=*/0, /*cc=*/0, {0, symbol},
      &storage_type, 1, /*result_types=*/nullptr, /*result_count=*/0,
      /*tied_results=*/nullptr, /*tied_result_count=*/0, LOOM_LOCATION_UNKNOWN,
      &declaration));
  const loom_value_id_t argument = loom_test_decl_args(declaration).values[0];

  std::vector<uint32_t> visits(module_->values.count, 0);
  const iree_host_size_t arena_bytes = module_->arena.used_allocation_size;
  fail_allocations_ = true;
  iree_status_t status = loom_op_walk_subtree_value_refs(
      module_, declaration,
      [](loom_value_id_t value, void* user_data) {
        ++(*static_cast<std::vector<uint32_t>*>(user_data))[value];
        return iree_ok_status();
      },
      &visits);
  fail_allocations_ = false;
  IREE_ASSERT_OK(status);
  EXPECT_EQ(visits[width], 1u);
  EXPECT_EQ(visits[argument], 0u);
  EXPECT_EQ(module_->arena.used_allocation_size, arena_bytes);
  EXPECT_EQ(failed_allocations_, 0u);
}

TEST_F(ValueRefsTest, SubtreeWalkStopsAtCallbackFailure) {
  loom_op_t* owner = Assume(Constant(1));
  uint32_t visit_count = 0;
  IREE_EXPECT_STATUS_IS(IREE_STATUS_ABORTED,
                        loom_op_walk_subtree_value_refs(
                            module_, owner,
                            [](loom_value_id_t value, void* user_data) {
                              ++*static_cast<uint32_t*>(user_data);
                              return iree_make_status(
                                  IREE_STATUS_ABORTED,
                                  "callback failed at value %u", value);
                            },
                            &visit_count));
  EXPECT_EQ(visit_count, 1u);
}

TEST_F(ValueRefsTest, SubtreeWalkStopsInsideRetainedMembershipBatch) {
  const auto input = Constant(1);
  loom_predicate_t predicates[] = {
      Predicate(Constant(2)), Predicate(Constant(3)), Predicate(Constant(4))};
  auto* owner = Assume(input);
  IREE_ASSERT_OK(loom_op_set_attr(module_, owner, 0,
                                  loom_attr_predicate_list(predicates, 3)));
  uint32_t visit_count = 0;
  IREE_EXPECT_STATUS_IS(IREE_STATUS_ABORTED,
                        loom_op_walk_subtree_value_refs(
                            module_, owner,
                            [](loom_value_id_t, void* user_data) {
                              if (++*static_cast<uint32_t*>(user_data) == 3) {
                                return iree_make_status(
                                    IREE_STATUS_ABORTED,
                                    "callback failed inside a batch");
                              }
                              return iree_ok_status();
                            },
                            &visit_count));
  // The ordinary operand and two batch members precede the failure. The
  // remaining batch member is not visited and the index remains unchanged.
  EXPECT_EQ(visit_count, 3u);
  EXPECT_EQ(Outgoing(owner, 0).size(), 3u);
}

TEST_F(ValueRefsTest, DuplicateAndSharedOwnersUnlinkExactly) {
  const loom_value_id_t original = Constant(1);
  const loom_value_id_t replacement = Constant(2);
  loom_op_t* first = Assume(original);
  loom_op_t* middle = Assume(original);
  loom_op_t* last = Assume(original);
  EXPECT_EQ(IncomingCount(original), 3u);

  IREE_ASSERT_OK(loom_op_erase(module_, middle));
  EXPECT_EQ(IncomingCount(original), 2u);
  loom_predicate_t predicate = Predicate(replacement);
  IREE_ASSERT_OK(loom_op_set_attr(module_, first, 0,
                                  loom_attr_predicate_list(&predicate, 1)));
  EXPECT_EQ(IncomingCount(original), 1u);
  EXPECT_EQ(IncomingCount(replacement), 1u);
  IREE_ASSERT_OK(loom_op_erase(module_, last));
  EXPECT_FALSE(HasUses(original));
  EXPECT_TRUE(HasUses(replacement));

  IREE_ASSERT_OK(
      loom_value_replace_all_uses_with(module_, replacement, original));
  const auto attribute = loom_op_const_attrs(first)[0];
  EXPECT_EQ(attribute.predicate_list[0].args[0], original);
  EXPECT_EQ(attribute.predicate_list[0].args[1], original);
  EXPECT_EQ(IncomingCount(original), 1u);
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
  IREE_ASSERT_OK(loom_module_compute_uses(module_));
  EXPECT_FALSE(HasUses(original));
  EXPECT_EQ(IncomingCount(replacement), 1u);
  const auto retained = module_->type_uses.arena.used_allocation_size;
  IREE_ASSERT_OK(loom_module_compute_uses(module_));
  EXPECT_EQ(module_->type_uses.arena.used_allocation_size, retained);
  EXPECT_EQ(IncomingCount(replacement), 1u);
  IREE_ASSERT_OK(loom_op_erase(module_, owner));
  EXPECT_FALSE(HasUses(replacement));
}

TEST_F(ValueRefsTest, ReplacementMergesMembershipAndPreservesEveryOccurrence) {
  const loom_value_id_t original = Constant(1);
  const loom_value_id_t replacement = Constant(2);
  const loom_value_id_t other = Constant(3);
  loom_op_t* owner = Assume(original);
  loom_predicate_t predicates[] = {Predicate(original), Predicate(replacement),
                                   Predicate(other)};
  predicates[1].args[0] = original;
  predicates[2].args[1] = original;
  IREE_ASSERT_OK(loom_op_set_attr(module_, owner, 0,
                                  loom_attr_predicate_list(predicates, 3)));
  EXPECT_EQ(Outgoing(owner, 0),
            (std::vector<loom_value_id_t>{original, replacement, other}));
  EXPECT_EQ(IncomingCount(original), 1u);
  EXPECT_EQ(IncomingCount(replacement), 1u);
  EXPECT_EQ(IncomingCount(other), 1u);

  IREE_ASSERT_OK(
      loom_value_replace_all_uses_with(module_, original, replacement));
  EXPECT_EQ(Outgoing(owner, 0),
            (std::vector<loom_value_id_t>{replacement, other}));
  EXPECT_FALSE(HasUses(original));
  EXPECT_EQ(IncomingCount(replacement), 1u);
  EXPECT_EQ(IncomingCount(other), 1u);
  const auto updated = loom_op_const_attrs(owner)[0];
  for (uint16_t i = 0; i < updated.count; ++i) {
    for (uint8_t j = 0; j < 2; ++j) {
      EXPECT_EQ(updated.predicate_list[i].args[j],
                i == 2 && j == 0 ? other : replacement);
    }
  }

  // A second merge into an already-referenced value leaves one membership
  // without dropping any payload occurrence.
  IREE_ASSERT_OK(loom_value_replace_all_uses_with(module_, replacement, other));
  EXPECT_EQ(Outgoing(owner, 0), (std::vector<loom_value_id_t>{other}));
  EXPECT_FALSE(HasUses(replacement));
  EXPECT_EQ(IncomingCount(other), 1u);
  IREE_ASSERT_OK(loom_op_erase(module_, owner));
  EXPECT_FALSE(HasUses(other));
}

TEST_F(ValueRefsTest, ReplacementReusesOwnersAndCanonicalMembership) {
  const loom_value_id_t original = Constant(1);
  const loom_value_id_t replacement = Constant(2);
  // Retain the replacement singleton, then fill a complete owner page.
  IREE_ASSERT_OK(loom_op_erase(module_, Assume(replacement)));
  std::array<loom_op_t*, 128> owners;
  for (auto& owner : owners) {
    owner = Assume(original);
  }
  const auto retained = module_->type_uses.arena.used_allocation_size;
  IREE_ASSERT_OK(
      loom_value_replace_all_uses_with(module_, original, replacement));
  EXPECT_EQ(module_->type_uses.arena.used_allocation_size, retained);
  EXPECT_FALSE(HasUses(original));
  EXPECT_EQ(IncomingCount(replacement), owners.size());
  for (auto* owner : owners) {
    const auto attribute = loom_op_const_attrs(owner)[0];
    EXPECT_EQ(attribute.predicate_list[0].args[0], replacement);
    EXPECT_EQ(attribute.predicate_list[0].args[1], replacement);
    IREE_ASSERT_OK(loom_op_erase(module_, owner));
  }
  EXPECT_FALSE(HasUses(replacement));
}

TEST_F(ValueRefsTest, FailedReplacementPayloadPreservesEdgesAndCanRetry) {
  const loom_value_id_t original = Constant(1);
  const loom_value_id_t replacement = Constant(2);
  loom_op_t* owner = Assume(original);
  std::array<loom_predicate_t, 128> predicates;
  predicates.fill(Predicate(original));
  ASSERT_GT(sizeof(predicates),
            iree_arena_block_pool_max_allocation_size(&pool_));
  IREE_ASSERT_OK(loom_op_set_attr(
      module_, owner, 0,
      loom_attr_predicate_list(predicates.data(), predicates.size())));
  const auto edges = Outgoing(owner, 0);
  const auto attribute = loom_op_const_attrs(owner)[0];
  fail_allocations_ = true;
  iree_status_t status =
      loom_value_replace_all_uses_with(module_, original, replacement);
  fail_allocations_ = false;
  IREE_EXPECT_STATUS_IS(IREE_STATUS_RESOURCE_EXHAUSTED, status);
  EXPECT_GT(failed_allocations_, 0u);
  EXPECT_EQ(Outgoing(owner, 0), edges);
  EXPECT_TRUE(loom_attribute_equal(&attribute, &loom_op_const_attrs(owner)[0]));
  EXPECT_EQ(IncomingCount(original), 1u);
  EXPECT_FALSE(HasUses(replacement));
  EXPECT_EQ(loom_op_const_operands(owner)[0], original);

  IREE_ASSERT_OK(
      loom_value_replace_all_uses_with(module_, original, replacement));
  EXPECT_EQ(Outgoing(owner, 0), (std::vector<loom_value_id_t>{replacement}));
  EXPECT_FALSE(HasUses(original));
  EXPECT_EQ(IncomingCount(replacement), 1u);
  EXPECT_EQ(loom_op_const_operands(owner)[0], replacement);
  const auto updated = loom_op_const_attrs(owner)[0];
  for (uint16_t i = 0; i < updated.count; ++i) {
    EXPECT_EQ(updated.predicate_list[i].args[0], replacement);
    EXPECT_EQ(updated.predicate_list[i].args[1], replacement);
  }
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
  EXPECT_EQ(IncomingCount(original), 1u);
  EXPECT_EQ(IncomingCount(replacement), 0u);
  EXPECT_FALSE(HasUses(replacement));
  // Immutable membership prepared before the validation error remains reusable.
  const auto retained = module_->type_uses.arena.used_allocation_size;
  IREE_ASSERT_OK(loom_op_set_attr(module_, owner, 0,
                                  loom_attr_predicate_list(&predicate, 1)));
  EXPECT_EQ(module_->type_uses.arena.used_allocation_size, retained);
  EXPECT_FALSE(HasUses(original));
  EXPECT_EQ(IncomingCount(replacement), 1u);
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
  EXPECT_EQ(IncomingCount(value), 1u);
  loom_op_attrs(owner)[loom_test_attrs_dict_ATTR_INDEX] = loom_attr_absent();
  IREE_ASSERT_OK(loom_module_refresh_op_attribute_uses(module_, owner));
  EXPECT_FALSE(HasUses(value));
  EXPECT_EQ(IncomingCount(value), 0u);
  const auto retained = module_->type_uses.arena.used_allocation_size;
  IREE_ASSERT_OK(loom_module_refresh_op_attribute_uses(module_, owner));
  EXPECT_EQ(module_->type_uses.arena.used_allocation_size, retained);
  IREE_ASSERT_OK(loom_op_erase(module_, owner));
}

TEST_F(ValueRefsTest, FailedIndexGrowthPreservesOldOwnersAndCanRetry) {
  const loom_value_id_t original = Constant(1);
  const loom_value_id_t replacement = Constant(2);
  loom_op_t* owner = Assume(original);
  const auto old_attribute = loom_op_const_attrs(owner)[0];
  std::array<loom_predicate_t, 128> predicates;
  predicates[0] = Predicate(replacement);
  for (size_t i = 1; i < predicates.size(); ++i) {
    predicates[i] = Predicate(Constant(i + 2));
  }
  const auto attribute =
      loom_attr_predicate_list(predicates.data(), predicates.size());
  fail_allocations_ = true;
  iree_status_t status = loom_op_set_attr(module_, owner, 0, attribute);
  fail_allocations_ = false;
  IREE_EXPECT_STATUS_IS(IREE_STATUS_RESOURCE_EXHAUSTED, status);
  EXPECT_GT(failed_allocations_, 0u);
  EXPECT_TRUE(
      loom_attribute_equal(&old_attribute, &loom_op_const_attrs(owner)[0]));
  EXPECT_EQ(IncomingCount(original), 1u);
  for (const auto& predicate : predicates) {
    EXPECT_FALSE(HasUses(predicate.args[0]));
  }
  IREE_ASSERT_OK(loom_op_set_attr(module_, owner, 0, attribute));
  EXPECT_FALSE(HasUses(original));
  for (const auto& predicate : predicates) {
    EXPECT_EQ(IncomingCount(predicate.args[0]), 1u);
  }
  IREE_ASSERT_OK(
      loom_value_replace_all_uses_with(module_, replacement, original));
  EXPECT_FALSE(HasUses(replacement));
  EXPECT_EQ(IncomingCount(original), 1u);
  const auto updated = loom_op_const_attrs(owner)[0];
  for (uint16_t i = 0; i < updated.count; ++i) {
    const auto expected = i == 0 ? original : predicates[i].args[0];
    EXPECT_EQ(updated.predicate_list[i].args[0], expected);
    EXPECT_EQ(updated.predicate_list[i].args[1], expected);
  }
  IREE_ASSERT_OK(loom_op_erase(module_, owner));
  EXPECT_FALSE(HasUses(original));
  for (const auto& predicate : predicates) {
    EXPECT_FALSE(HasUses(predicate.args[0]));
  }
}

TEST_F(ValueRefsTest, RepackingResultsPreservesAttributeOwnership) {
  loom_string_id_t name = LOOM_STRING_ID_INVALID;
  uint16_t symbol = 0;
  IREE_ASSERT_OK(
      loom_module_intern_string(module_, IREE_SV("function"), &name));
  IREE_ASSERT_OK(loom_module_add_symbol(module_, name, &symbol));
  const loom_type_t index = loom_type_scalar(LOOM_SCALAR_TYPE_INDEX);
  const loom_type_t arguments[] = {index, index};
  const loom_type_t results[] = {index, index, index};
  loom_op_t* function = nullptr;
  IREE_ASSERT_OK(loom_test_func_build(
      &builder_, 0, 0, 0, {0, symbol}, arguments, 2, results, 3, nullptr, 0,
      nullptr, 0, LOOM_LOCATION_UNKNOWN, &function));
  const loom_value_id_t argument = loom_block_arg_id(
      loom_region_entry_block(loom_test_func_body(function)), 0);
  const loom_value_id_t replacement = loom_block_arg_id(
      loom_region_entry_block(loom_test_func_body(function)), 1);
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
  EXPECT_EQ(IncomingCount(argument), 1u);
  EXPECT_EQ(Outgoing(function, loom_test_func_predicates_ATTR_INDEX),
            (std::vector<loom_value_id_t>{argument}));
  IREE_ASSERT_OK(
      loom_value_replace_all_uses_with(module_, argument, replacement));
  EXPECT_EQ(Outgoing(function, loom_test_func_predicates_ATTR_INDEX),
            (std::vector<loom_value_id_t>{replacement}));
  EXPECT_FALSE(HasUses(argument));
  EXPECT_EQ(IncomingCount(replacement), 1u);
  EXPECT_EQ(loom_op_const_attrs(function)[loom_test_func_predicates_ATTR_INDEX]
                .predicate_list[0]
                .args[0],
            replacement);
  IREE_ASSERT_OK(loom_op_set_attr(module_, function,
                                  loom_test_func_predicates_ATTR_INDEX,
                                  loom_attr_absent()));
  EXPECT_FALSE(HasUses(replacement));
}

TEST_F(ValueRefsTest, PredicateNestedInsideTypeKeepsAttributeOwnership) {
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

  // Pre-intern the replacement to exercise canonical payload reuse through
  // the same active attribute ownership boundary.
  loom_predicate_t expected_predicate = Predicate(replacement);
  const loom_named_attr_t expected_metadata[] = {
      {key, {}, loom_attr_predicate_list(&expected_predicate, 1)},
  };
  loom_type_t expected_type = {};
  IREE_ASSERT_OK(loom_test_array_type_make(
      module_, LOOM_TEST_ARRAY_TYPE_BUILD_FLAG_HAS_METADATA, element_type, 0,
      loom_make_named_attr_slice(expected_metadata, 1), &expected_type));
  loom_type_id_t expected_type_id = LOOM_TYPE_ID_INVALID;
  IREE_ASSERT_OK(
      loom_module_intern_type_id(module_, expected_type, &expected_type_id));

  const loom_named_attr_t attributes[] = {{key, {}, loom_attr_type(type_id)}};
  loom_op_t* owner = nullptr;
  IREE_ASSERT_OK(loom_test_attrs_build(
      &builder_, LOOM_TEST_ATTRS_BUILD_FLAG_HAS_DICT, original,
      loom_make_named_attr_slice(attributes, 1),
      loom_type_scalar(LOOM_SCALAR_TYPE_INDEX), LOOM_LOCATION_UNKNOWN, &owner));
  EXPECT_TRUE(HasUses(original));
  EXPECT_FALSE(loom_module_value_has_type_uses(module_, original));
  EXPECT_EQ(Outgoing(owner, loom_test_attrs_dict_ATTR_INDEX),
            (std::vector<loom_value_id_t>{original}));
  IREE_ASSERT_OK(
      loom_value_replace_all_uses_with(module_, original, replacement));
  EXPECT_EQ(Outgoing(owner, loom_test_attrs_dict_ATTR_INDEX),
            (std::vector<loom_value_id_t>{replacement}));
  EXPECT_FALSE(HasUses(original));
  EXPECT_EQ(IncomingCount(replacement), 1u);
  EXPECT_FALSE(loom_module_value_has_type_uses(module_, replacement));
  const auto dictionary = loom_test_attrs_dict(owner);
  EXPECT_EQ(dictionary.entries[0].value.type_id, expected_type_id);
  const auto updated_type =
      module_->types.entries[dictionary.entries[0].value.type_id];
  const auto updated_metadata = loom_test_array_type_metadata(updated_type);
  EXPECT_EQ(updated_metadata.entries[0].value.predicate_list[0].args[0],
            replacement);
  IREE_ASSERT_OK(loom_op_erase(module_, owner));
  EXPECT_FALSE(HasUses(replacement));
}

TEST_F(ValueRefsTest, OneOwnerRetargetsTypeAndPredicateListsTogether) {
  const loom_value_id_t original = Constant(1);
  const loom_value_id_t replacement = Constant(2);
  loom_predicate_t predicate = Predicate(original);
  const loom_type_t type =
      loom_type_shaped_1d(LOOM_TYPE_VECTOR, LOOM_SCALAR_TYPE_INDEX,
                          loom_dim_pack_dynamic(original), 0);
  loom_type_id_t type_id = LOOM_TYPE_ID_INVALID;
  IREE_ASSERT_OK(loom_module_intern_type_id(module_, type, &type_id));
  loom_string_id_t constraint_key = LOOM_STRING_ID_INVALID;
  loom_string_id_t shape_key = LOOM_STRING_ID_INVALID;
  IREE_ASSERT_OK(loom_module_intern_string(module_, IREE_SV("constraint"),
                                           &constraint_key));
  IREE_ASSERT_OK(
      loom_module_intern_string(module_, IREE_SV("shape"), &shape_key));
  const loom_named_attr_t attributes[] = {
      {constraint_key, {}, loom_attr_predicate_list(&predicate, 1)},
      {shape_key, {}, loom_attr_type(type_id)},
  };
  loom_op_t* owner = nullptr;
  IREE_ASSERT_OK(loom_test_attrs_build(
      &builder_, LOOM_TEST_ATTRS_BUILD_FLAG_HAS_DICT, original,
      loom_make_named_attr_slice(attributes, 2),
      loom_type_scalar(LOOM_SCALAR_TYPE_INDEX), LOOM_LOCATION_UNKNOWN, &owner));
  EXPECT_EQ(Outgoing(owner, loom_test_attrs_dict_ATTR_INDEX),
            (std::vector<loom_value_id_t>{original}));
  EXPECT_EQ(IncomingCount(original), 1u);
  IREE_ASSERT_OK(
      loom_value_replace_all_uses_with(module_, original, replacement));
  EXPECT_EQ(Outgoing(owner, loom_test_attrs_dict_ATTR_INDEX),
            (std::vector<loom_value_id_t>{replacement}));
  EXPECT_FALSE(HasUses(original));
  EXPECT_EQ(IncomingCount(replacement), 1u);
  const auto updated = loom_test_attrs_dict(owner);
  EXPECT_EQ(updated.entries[0].value.predicate_list[0].args[0], replacement);
  EXPECT_EQ(loom_type_dim_value_id_at(
                module_->types.entries[updated.entries[1].value.type_id], 0),
            replacement);
  IREE_ASSERT_OK(loom_op_erase(module_, owner));
  EXPECT_FALSE(HasUses(replacement));
}

TEST_F(ValueRefsTest, StructuralWalkPreservesOrderedDuplicateTypeReferences) {
  const auto earlier = Constant(1);
  const auto later = Constant(2);
  const auto vector = loom_type_shaped_2d(
      LOOM_TYPE_VECTOR, LOOM_SCALAR_TYPE_F32, loom_dim_pack_dynamic(later),
      loom_dim_pack_dynamic(earlier), 0);
  const loom_type_t children[] = {vector, vector};
  loom_type_t function;
  IREE_ASSERT_OK(loom_module_intern_function_type(module_, children, 2, nullptr,
                                                  0, &function));
  loom_type_id_t type_id;
  IREE_ASSERT_OK(loom_module_intern_type_id(module_, function, &type_id));
  std::vector<loom_value_id_t> occurrences;
  IREE_ASSERT_OK(loom_module_walk_attribute_value_refs(
      module_, loom_attr_type(type_id),
      [](loom_value_id_t value, void* user_data) {
        static_cast<std::vector<loom_value_id_t>*>(user_data)->push_back(value);
        return iree_ok_status();
      },
      &occurrences));
  EXPECT_EQ(occurrences,
            (std::vector<loom_value_id_t>{later, earlier, later, earlier}));
}

TEST_F(ValueRefsTest, SharedTypePathsRetainOneOwnerAndOneProvider) {
  const auto original = Constant(1);
  const auto replacement = Constant(2);
  auto type = loom_type_shaped_1d(LOOM_TYPE_VECTOR, LOOM_SCALAR_TYPE_F32,
                                  loom_dim_pack_dynamic(original), 0);
  for (int i = 0; i < 64; ++i) {
    const loom_type_t children[] = {type, type};
    IREE_ASSERT_OK(loom_module_intern_function_type(module_, children, 2,
                                                    nullptr, 0, &type));
  }
  auto* first = TypeOwner(original, type);
  auto* second = TypeOwner(original, type);
  EXPECT_EQ(IncomingCount(original), 2u);
  EXPECT_EQ(Outgoing(first, loom_test_attrs_dict_ATTR_INDEX),
            (std::vector<loom_value_id_t>{original}));
  EXPECT_FALSE(loom_module_value_has_type_uses(module_, original));
  IREE_ASSERT_OK(
      loom_value_replace_all_uses_with(module_, original, replacement));
  EXPECT_EQ(IncomingCount(original), 0u);
  EXPECT_EQ(IncomingCount(replacement), 2u);
  EXPECT_EQ(loom_test_attrs_dict(first).entries[0].value.type_id,
            loom_test_attrs_dict(second).entries[0].value.type_id);
  IREE_ASSERT_OK(loom_op_erase(module_, first));
  EXPECT_EQ(IncomingCount(replacement), 1u);
  IREE_ASSERT_OK(loom_op_erase(module_, second));
  EXPECT_FALSE(HasUses(replacement));
}

TEST_F(ValueRefsTest, ForwardProvidersBecomeActiveAtRefreshAndRebuild) {
  const auto available = Constant(1);
  const auto type = loom_type_shaped_2d(LOOM_TYPE_VECTOR, LOOM_SCALAR_TYPE_F32,
                                        loom_dim_pack_dynamic(available),
                                        loom_dim_pack_dynamic(8), 0);
  auto* owner = TypeOwner(available, type);
  EXPECT_EQ(Outgoing(owner, loom_test_attrs_dict_ATTR_INDEX),
            (std::vector<loom_value_id_t>{available}));
  while (module_->values.count <= 8) {
    Constant(module_->values.count);
  }
  EXPECT_FALSE(HasUses(8));
  IREE_ASSERT_OK(loom_module_refresh_op_attribute_uses(module_, owner));
  EXPECT_EQ(Outgoing(owner, loom_test_attrs_dict_ATTR_INDEX),
            (std::vector<loom_value_id_t>{available, 8}));
  loom_module_drop_op_attribute_uses(module_, owner);
  EXPECT_FALSE(HasUses(available));
  EXPECT_FALSE(HasUses(8));
  IREE_ASSERT_OK(loom_module_compute_uses(module_));
  EXPECT_EQ(IncomingCount(available), 1u);
  EXPECT_EQ(IncomingCount(8), 1u);
  IREE_ASSERT_OK(loom_op_erase(module_, owner));
  EXPECT_FALSE(HasUses(8));
}

TEST_F(ValueRefsTest, FailedOwnerGrowthLeavesPayloadAndOtherOwnersIntact) {
  const auto original = Constant(1);
  std::array<loom_op_t*, 129> owners;
  const auto index = loom_type_scalar(LOOM_SCALAR_TYPE_INDEX);
  for (auto& owner : owners) {
    IREE_ASSERT_OK(loom_test_assume_build(&builder_, &original, 1, nullptr, 0,
                                          &index, 1, LOOM_LOCATION_NONE,
                                          &owner));
  }
  auto predicate = Predicate(original);
  const auto attribute = loom_attr_predicate_list(&predicate, 1);
  for (size_t i = 0; i + 1 < owners.size(); ++i) {
    IREE_ASSERT_OK(loom_op_set_attr(module_, owners[i], 0, attribute));
  }
  const auto previous = loom_op_const_attrs(owners.back())[0];
  fail_allocations_ = true;
  auto status = loom_op_set_attr(module_, owners.back(), 0, attribute);
  fail_allocations_ = false;
  IREE_EXPECT_STATUS_IS(IREE_STATUS_RESOURCE_EXHAUSTED, status);
  EXPECT_GT(failed_allocations_, 0u);
  EXPECT_TRUE(
      loom_attribute_equal(&previous, &loom_op_const_attrs(owners.back())[0]));
  EXPECT_EQ(IncomingCount(original), owners.size() - 1);
  EXPECT_TRUE(Outgoing(owners.back(), 0).empty());
  IREE_ASSERT_OK(loom_op_set_attr(module_, owners.back(), 0, attribute));
  EXPECT_EQ(IncomingCount(original), owners.size());
  for (auto* owner : owners) {
    IREE_ASSERT_OK(loom_op_erase(module_, owner));
  }
  EXPECT_FALSE(HasUses(original));
}

TEST_F(ValueRefsTest, SparseMembershipCrossesBitmapAndValuePageBoundaries) {
  while (module_->values.count <= 256) {
    Constant(module_->values.count);
  }
  std::array<loom_predicate_t, 9> predicates;
  const std::vector<loom_value_id_t> providers = {1,   62,  63,  64, 65,
                                                  127, 128, 255, 256};
  for (size_t i = 0; i < providers.size(); ++i) {
    predicates[i] = Predicate(providers[providers.size() - i - 1]);
  }
  auto* owner = Assume(1);
  IREE_ASSERT_OK(loom_op_set_attr(
      module_, owner, 0,
      loom_attr_predicate_list(predicates.data(), predicates.size())));
  EXPECT_EQ(Outgoing(owner, 0), providers);
  for (const auto provider : providers) {
    EXPECT_EQ(IncomingCount(provider), 1u);
  }
  EXPECT_EQ(IncomingCount(2), 0u);
  EXPECT_EQ(IncomingCount(126), 0u);
  IREE_ASSERT_OK(loom_value_replace_all_uses_with(module_, 63, 64));
  EXPECT_EQ(Outgoing(owner, 0),
            (std::vector<loom_value_id_t>{1, 62, 64, 65, 127, 128, 255, 256}));
  EXPECT_FALSE(HasUses(63));
  EXPECT_EQ(IncomingCount(64), 1u);
  IREE_ASSERT_OK(loom_module_compute_uses(module_));
  EXPECT_EQ(Outgoing(owner, 0),
            (std::vector<loom_value_id_t>{1, 62, 64, 65, 127, 128, 255, 256}));
  IREE_ASSERT_OK(loom_op_erase(module_, owner));
  for (const auto provider : providers) {
    EXPECT_FALSE(HasUses(provider));
  }
}

TEST_F(ValueRefsTest, OverlappingMembershipMatchesOwnersAcrossMutation) {
  std::array<loom_value_id_t, 16> providers;
  std::array<loom_value_id_t, 16> carriers;
  std::array<loom_op_t*, 16> owners;
  std::array<std::array<loom_predicate_t, 4>, 32> predicates;
  std::array<int, 16> selected;
  selected.fill(-1);
  const auto index = loom_type_scalar(LOOM_SCALAR_TYPE_INDEX);
  for (auto& provider : providers) {
    provider = Constant(1);
  }
  for (size_t i = 0; i < providers.size(); ++i) {
    const auto type =
        loom_type_shaped_1d(LOOM_TYPE_VECTOR, LOOM_SCALAR_TYPE_F32,
                            loom_dim_pack_dynamic(providers[i]), 0);
    IREE_ASSERT_OK(loom_module_define_value(module_, type, &carriers[i]));
    IREE_ASSERT_OK(
        loom_block_add_arg(module_, loom_module_block(module_), carriers[i]));
    IREE_ASSERT_OK(loom_test_assume_build(&builder_, &providers[i], 1, nullptr,
                                          0, &index, 1, LOOM_LOCATION_NONE,
                                          &owners[i]));
  }
  for (size_t i = 0; i < predicates.size(); ++i) {
    for (size_t j = 0; j < predicates[i].size(); ++j) {
      predicates[i][j] = Predicate(providers[(i + j * (i / 16 + 1)) % 16]);
    }
  }
  uint32_t random = 0x172a91f3;
  for (int iteration = 0; iteration < 512; ++iteration) {
    random ^= random << 13;
    random ^= random >> 17;
    random ^= random << 5;
    const auto owner_index = random % owners.size();
    selected[owner_index] = static_cast<int>((random >> 8) % 33) - 1;
    const auto pattern = selected[owner_index];
    const auto attribute =
        pattern < 0 ? loom_attr_absent()
                    : loom_attr_predicate_list(predicates[pattern].data(), 4);
    IREE_ASSERT_OK(
        loom_op_set_attr(module_, owners[owner_index], 0, attribute));
    if (iteration % 17 == 0) {
      IREE_ASSERT_OK(loom_module_compute_uses(module_));
    }
    for (size_t i = 0; i < providers.size(); ++i) {
      std::set<loom_op_t*> expected;
      for (size_t j = 0; j < owners.size(); ++j) {
        if (selected[j] < 0) {
          continue;
        }
        for (const auto& predicate : predicates[selected[j]]) {
          if (predicate.args[0] == providers[i]) {
            expected.insert(owners[j]);
          }
        }
      }
      std::set<loom_op_t*> actual;
      loom_type_use_iterator_t users;
      loom_attribute_users_begin(&module_->type_uses, providers[i], &users);
      for (auto user = loom_attribute_users_next(&users); user.op;
           user = loom_attribute_users_next(&users)) {
        EXPECT_EQ(user.attribute_index, 0);
        EXPECT_TRUE(actual.insert(user.op).second);
      }
      EXPECT_EQ(actual, expected);
      EXPECT_EQ(HasUses(providers[i]), !expected.empty());
      loom_module_value_type_users(module_, providers[i], &users);
      EXPECT_EQ(loom_type_users_next(&users), carriers[i]);
      EXPECT_EQ(loom_type_users_next(&users), LOOM_VALUE_ID_INVALID);
    }
  }
  loom_module_reset_attribute_uses(module_);
  for (const auto provider : providers) {
    EXPECT_FALSE(HasUses(provider));
    EXPECT_TRUE(loom_module_value_has_type_uses(module_, provider));
  }
}

}  // namespace
}  // namespace loom
