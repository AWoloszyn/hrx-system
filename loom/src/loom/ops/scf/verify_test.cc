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

TEST_F(ScfVerifyTest, PipelineOperandCannotOwnResultStorage) {
  const auto lower = Constant(0);
  const auto upper = Constant(8);
  const auto step = Constant(1);
  const auto initial = Constant(9);
  const auto depth = Constant(3);
  // The text format parses result ties before the pipeline clause. The
  // builder API can express a tie to that policy operand and must diagnose it.
  for (uint16_t operand_index : {uint16_t{3}, uint16_t{4}}) {
    loom_tied_result_t tie = {0, operand_index, false};
    loom_op_t* loop = nullptr;
    IREE_ASSERT_OK(loom_scf_for_build(
        &builder_, LOOM_SCF_FOR_BUILD_FLAG_HAS_PIPELINE_DEPTH, lower, upper,
        step, &initial, 1, &tie, 1, LOOM_VALUE_ID_INVALID, 0, 0, depth,
        LOOM_LOCATION_UNKNOWN, &loop));
    testing::DiagnosticEmissionCapture capture;
    IREE_ASSERT_OK(loom_scf_for_verify(module_, loop, capture.emitter()));
    if (operand_index == 3) {
      EXPECT_TRUE(capture.emissions.empty());
    } else {
      ASSERT_EQ(capture.emissions.size(), 1u);
      const auto& diagnostic = capture.emissions.front();
      EXPECT_EQ(diagnostic.error, LOOM_ERR_STRUCTURE_014);
      EXPECT_EQ(diagnostic.string_params[0], "pipeline_depth");
      EXPECT_EQ(diagnostic.string_params[1], "not tied to a result");
    }
  }
}

}  // namespace
}  // namespace loom
