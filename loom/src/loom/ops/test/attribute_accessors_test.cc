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
#include "loom/ops/test/ops.h"
#include "loom/rewrite/rewriter.h"

namespace loom {
namespace {

class AttributeAccessorTest : public ::testing::Test {
 protected:
  void SetUp() override {
    iree_arena_block_pool_initialize(4096, iree_allocator_system(),
                                     &block_pool_);
    loom_context_initialize(iree_allocator_system(), &context_);
    iree_host_size_t vtable_count = 0;
    const loom_op_vtable_t* const* vtables =
        loom_test_dialect_vtables(&vtable_count);
    IREE_ASSERT_OK(loom_context_register_dialect(
        &context_, LOOM_DIALECT_TEST, vtables, (uint16_t)vtable_count));
    IREE_ASSERT_OK(loom_context_finalize(&context_));
    IREE_ASSERT_OK(loom_module_allocate(&context_, IREE_SV("test"),
                                        &block_pool_, nullptr,
                                        iree_allocator_system(), &module_));
    loom_builder_initialize(module_, &module_->arena,
                            loom_module_block(module_), &builder_);
  }

  void TearDown() override {
    loom_module_free(module_);
    loom_context_deinitialize(&context_);
    iree_arena_block_pool_deinitialize(&block_pool_);
  }

  // Storage backing the test module's arena.
  iree_arena_block_pool_t block_pool_;
  // Registry containing the generated test dialect.
  loom_context_t context_;
  // Module owning the operations and their attribute storage.
  loom_module_t* module_ = nullptr;
  // Builder inserting records into the module body.
  loom_builder_t builder_;
};

TEST_F(AttributeAccessorTest, PresenceDistinguishesZeroAndEmptyFromAbsence) {
  for (bool present : {false, true}) {
    loom_string_id_t name = LOOM_STRING_ID_INVALID;
    IREE_ASSERT_OK(loom_module_intern_string(
        module_, present ? IREE_SV("present") : IREE_SV("absent"), &name));
    loom_symbol_id_t symbol = LOOM_SYMBOL_ID_INVALID;
    IREE_ASSERT_OK(loom_module_add_symbol(module_, name, &symbol));
    const loom_test_record_build_flags_t flags =
        present ? LOOM_TEST_RECORD_BUILD_FLAG_HAS_KIND |
                      LOOM_TEST_RECORD_BUILD_FLAG_HAS_DICT
                : 0;
    loom_op_t* op = nullptr;
    IREE_ASSERT_OK(loom_test_record_build(
        &builder_, flags, /*kind=*/0, loom_symbol_ref_t{0, symbol},
        loom_named_attr_slice_empty(), LOOM_LOCATION_UNKNOWN, &op));

    EXPECT_EQ(loom_test_record_kind(op), 0);
    EXPECT_EQ(loom_test_record_dict(op).count, 0u);
    EXPECT_EQ(loom_test_record_has_kind(op), present);
    EXPECT_EQ(loom_test_record_has_dict(op), present);

    static const loom_attr_field_t fields[] = {
        loom_test_record_kind_field(),
        loom_test_record_dict_field(),
    };
    const loom_attr_kind_t expected_kinds[] = {LOOM_ATTR_ENUM, LOOM_ATTR_DICT};
    for (size_t i = 0; i < IREE_ARRAYSIZE(fields); ++i) {
      const auto attribute = loom_op_attr(op, fields[i]);
      EXPECT_EQ(attribute.kind, present ? expected_kinds[i] : LOOM_ATTR_ABSENT);
    }
    const auto location =
        loom_attr_field_diagnostic_ref(loom_test_record_kind_field());
    EXPECT_EQ(location.kind, LOOM_DIAGNOSTIC_FIELD_ATTRIBUTE);
    const auto* descriptor =
        &loom_op_vtable(module_, op)->attr_descriptors[location.index];
    EXPECT_TRUE(iree_string_view_equal(loom_attr_descriptor_name(descriptor),
                                       IREE_SV("kind")));
    EXPECT_EQ(location.occurrence, 0);

    int evaluations = 0;
    EXPECT_EQ(loom_test_record_has_kind((++evaluations, op)), present);
    EXPECT_EQ(evaluations, 1);
  }
}

TEST_F(AttributeAccessorTest, RewritingTransfersAndClearsAttributeReferences) {
  loom_string_id_t name = LOOM_STRING_ID_INVALID;
  IREE_ASSERT_OK(
      loom_module_intern_string(module_, IREE_SV("function"), &name));
  loom_symbol_id_t symbol = LOOM_SYMBOL_ID_INVALID;
  IREE_ASSERT_OK(loom_module_add_symbol(module_, name, &symbol));
  const loom_type_t index = loom_type_scalar(LOOM_SCALAR_TYPE_INDEX);
  const loom_type_t arguments[] = {index, index};
  loom_op_t* function = nullptr;
  IREE_ASSERT_OK(loom_test_func_build(
      &builder_, 0, 0, 0, {0, symbol}, arguments, 2, nullptr, 0, nullptr, 0,
      nullptr, 0, LOOM_LOCATION_UNKNOWN, &function));
  const loom_block_t* entry =
      loom_region_entry_block(loom_test_func_body(function));
  const loom_value_id_t first = loom_block_arg_id(entry, 0);
  const loom_value_id_t second = loom_block_arg_id(entry, 1);
  loom_predicate_t predicates[] = {
      {LOOM_PREDICATE_LT,
       2,
       {LOOM_PRED_ARG_VALUE, LOOM_PRED_ARG_CONST},
       {},
       {first, 16}},
      {LOOM_PREDICATE_LT,
       2,
       {LOOM_PRED_ARG_VALUE, LOOM_PRED_ARG_CONST},
       {},
       {second, 16}},
  };

  iree_arena_allocator_t scratch;
  iree_arena_initialize(&block_pool_, &scratch);
  loom_rewriter_t rewriter;
  loom_rewriter_initialize(&rewriter, module_, &scratch);
  int rewriter_evaluations = 0;
  int operation_evaluations = 0;
  int attribute_evaluations = 0;
  IREE_ASSERT_OK(loom_test_func_rewrite_predicates(
      (++rewriter_evaluations, &rewriter), (++operation_evaluations, function),
      (++attribute_evaluations, loom_attr_predicate_list(&predicates[0], 1))));
  EXPECT_EQ(rewriter_evaluations, 1);
  EXPECT_EQ(operation_evaluations, 1);
  EXPECT_EQ(attribute_evaluations, 1);
  EXPECT_TRUE(loom_test_func_has_predicates(function));
  EXPECT_TRUE(loom_value_has_attribute_uses(loom_module_value(module_, first)));
  EXPECT_FALSE(
      loom_value_has_attribute_uses(loom_module_value(module_, second)));
  EXPECT_TRUE(iree_any_bit_set(rewriter.flags, LOOM_REWRITER_FLAG_CHANGED));

  IREE_ASSERT_OK(loom_test_func_rewrite_predicates(
      &rewriter, function, loom_attr_predicate_list(&predicates[1], 1)));
  EXPECT_FALSE(
      loom_value_has_attribute_uses(loom_module_value(module_, first)));
  EXPECT_TRUE(
      loom_value_has_attribute_uses(loom_module_value(module_, second)));
  EXPECT_EQ(loom_test_func_predicates(function).predicate_list[0].args[0],
            second);

  IREE_ASSERT_OK(loom_test_func_rewrite_predicates(&rewriter, function,
                                                   loom_attr_absent()));
  EXPECT_FALSE(loom_test_func_has_predicates(function));
  EXPECT_FALSE(
      loom_value_has_attribute_uses(loom_module_value(module_, second)));
  loom_rewriter_deinitialize(&rewriter);
  iree_arena_deinitialize(&scratch);
}

}  // namespace
}  // namespace loom
