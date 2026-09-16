// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/error/error_catalog.h"
#include "loom/ir/context.h"
#include "loom/ir/module.h"
#include "loom/ops/index/ops.h"
#include "loom/ops/scf/ops.h"
#include "loom/testing/diagnostic_matchers.h"

namespace loom {
namespace {

class ScfVerifyTest : public ::testing::Test {
 protected:
  void SetUp() override {
    iree_arena_block_pool_initialize(4096, iree_allocator_system(),
                                     &block_pool_);
    loom_context_initialize(iree_allocator_system(), &context_);
    iree_host_size_t count = 0;
    const loom_op_vtable_t* const* tables = loom_scf_dialect_vtables(&count);
    IREE_ASSERT_OK(loom_context_register_dialect(&context_, LOOM_DIALECT_SCF,
                                                 tables, (uint16_t)count));
    tables = loom_index_dialect_vtables(&count);
    IREE_ASSERT_OK(loom_context_register_dialect(&context_, LOOM_DIALECT_INDEX,
                                                 tables, (uint16_t)count));
    IREE_ASSERT_OK(loom_context_finalize(&context_));
    IREE_ASSERT_OK(loom_module_allocate(&context_, IREE_SV("verify"),
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

  loom_value_id_t Constant(int64_t value) {
    loom_op_t* op = nullptr;
    IREE_CHECK_OK(loom_index_constant_build(
        &builder_, loom_attr_i64(value),
        loom_type_scalar(LOOM_SCALAR_TYPE_INDEX), LOOM_LOCATION_UNKNOWN, &op));
    return loom_index_constant_result(op);
  }

  // Pool backing the module and its builder.
  iree_arena_block_pool_t block_pool_;
  // Registered source dialects.
  loom_context_t context_;
  // Module owning the API-built operations.
  loom_module_t* module_ = nullptr;
  // Builder positioned at module scope.
  loom_builder_t builder_ = {};
};

TEST_F(ScfVerifyTest, PolicyOperandsCannotOwnResultStorage) {
  const auto lower = Constant(0);
  const auto upper = Constant(8);
  const auto step = Constant(1);
  const auto initial = Constant(9);
  const auto depth = Constant(3);
  const auto factor = Constant(2);
  const loom_scf_for_build_flags_t policies[] = {
      LOOM_SCF_FOR_BUILD_FLAG_HAS_PIPELINE_DEPTH,
      LOOM_SCF_FOR_BUILD_FLAG_HAS_UNROLL_FACTOR,
      LOOM_SCF_FOR_BUILD_FLAG_HAS_PIPELINE_DEPTH |
          LOOM_SCF_FOR_BUILD_FLAG_HAS_UNROLL_FACTOR,
  };
  // The text format parses result ties before policy clauses. The builder API
  // can express those ties and must reject them independently of policy order.
  for (auto flags : policies) {
    const bool has_depth =
        iree_any_bit_set(flags, LOOM_SCF_FOR_BUILD_FLAG_HAS_PIPELINE_DEPTH);
    const bool has_factor =
        iree_any_bit_set(flags, LOOM_SCF_FOR_BUILD_FLAG_HAS_UNROLL_FACTOR);
    for (uint16_t operand_index = 3; operand_index < 4 + has_depth + has_factor;
         ++operand_index) {
      loom_tied_result_t tie = {0, operand_index, false};
      loom_op_t* loop = nullptr;
      IREE_ASSERT_OK(
          loom_scf_for_build(&builder_, flags, lower, upper, step, &initial, 1,
                             &tie, 1, has_depth ? depth : LOOM_VALUE_ID_INVALID,
                             has_factor ? factor : LOOM_VALUE_ID_INVALID, 0, 0,
                             LOOM_LOCATION_UNKNOWN, &loop));
      EXPECT_EQ(loom_scf_for_pipeline_depth(loop),
                has_depth ? depth : LOOM_VALUE_ID_INVALID);
      EXPECT_EQ(loom_scf_for_unroll_factor(loop),
                has_factor ? factor : LOOM_VALUE_ID_INVALID);
      testing::DiagnosticEmissionCapture capture;
      IREE_ASSERT_OK(loom_scf_for_verify(module_, loop, capture.emitter()));
      if (operand_index == 3) {
        EXPECT_TRUE(capture.emissions.empty());
      } else {
        ASSERT_EQ(capture.emissions.size(), 1u);
        const auto& diagnostic = capture.emissions.front();
        EXPECT_EQ(diagnostic.error, LOOM_ERR_STRUCTURE_014);
        EXPECT_EQ(diagnostic.string_params[0], has_depth && operand_index == 4
                                                   ? "pipeline_depth"
                                                   : "unroll_factor");
        EXPECT_EQ(diagnostic.string_params[1], "not tied to a result");
      }
    }
  }
}

}  // namespace
}  // namespace loom
