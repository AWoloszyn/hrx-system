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

    int evaluations = 0;
    EXPECT_EQ(loom_test_record_has_kind((++evaluations, op)), present);
    EXPECT_EQ(evaluations, 1);
  }
}

}  // namespace
}  // namespace loom
