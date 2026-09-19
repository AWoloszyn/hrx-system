// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/format/text/printer/block_order.h"

#include <vector>

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/ir/context.h"
#include "loom/ir/module.h"
#include "loom/ops/cfg/ops.h"
#include "loom/ops/test/ops.h"
#include "loom/ops/test/registry.h"

namespace loom {
namespace {

class BlockOrderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    iree_arena_block_pool_initialize(4096, iree_allocator_system(), &pool_);
    loom_context_initialize(iree_allocator_system(), &context_);
    IREE_ASSERT_OK(loom_test_dialect_register(&context_));
    iree_host_size_t count = 0;
    const loom_op_vtable_t* const* vtables = loom_cfg_dialect_vtables(&count);
    IREE_ASSERT_OK(loom_context_register_dialect(&context_, LOOM_DIALECT_CFG,
                                                 vtables, count));
    IREE_ASSERT_OK(loom_context_finalize(&context_));
    IREE_ASSERT_OK(loom_module_allocate(&context_, IREE_SV("order"), &pool_,
                                        nullptr, iree_allocator_system(),
                                        &module_));
  }

  void TearDown() override {
    loom_module_free(module_);
    loom_context_deinitialize(&context_);
    iree_arena_block_pool_deinitialize(&pool_);
  }

  std::vector<uint16_t> Order(
      const std::vector<std::vector<uint16_t>>& successors) {
    loom_region_t* region = nullptr;
    IREE_CHECK_OK(
        loom_module_allocate_region(module_, successors.size(), &region));
    loom_builder_t builder;
    loom_builder_initialize(module_, &module_->arena,
                            loom_region_entry_block(region), &builder);
    loom_op_t* constant = nullptr;
    IREE_CHECK_OK(loom_test_constant_build(
        &builder, loom_attr_i64(0), loom_type_scalar(LOOM_SCALAR_TYPE_I1),
        LOOM_LOCATION_UNKNOWN, &constant));
    std::vector<loom_block_t*> original;
    for (uint16_t index = 0; index < successors.size(); ++index) {
      loom_block_t* block = loom_region_block(region, index);
      original.push_back(block);
      loom_builder_set_block(&builder, block);
      loom_op_t* terminator = nullptr;
      if (successors[index].empty()) {
        IREE_CHECK_OK(loom_test_yield_build(
            &builder, nullptr, 0, LOOM_LOCATION_UNKNOWN, &terminator));
      } else if (successors[index].size() == 1) {
        IREE_CHECK_OK(loom_cfg_br_build(
            &builder, loom_region_block(region, successors[index][0]), nullptr,
            0, LOOM_LOCATION_UNKNOWN, &terminator));
      } else {
        IREE_CHECK_OK(loom_cfg_cond_br_build(
            &builder, loom_test_constant_result(constant),
            loom_region_block(region, successors[index][0]),
            loom_region_block(region, successors[index][1]),
            LOOM_LOCATION_UNKNOWN, &terminator));
      }
    }
    loom_print_block_order_t order = {};
    IREE_CHECK_OK(loom_print_block_order_initialize(module_, region, &order));
    std::vector<uint16_t> result;
    for (uint16_t position = 0; position < region->block_count; ++position) {
      result.push_back(loom_print_block_order_index(&order, position));
      EXPECT_EQ(loom_region_block(region, position), original[position]);
      EXPECT_EQ(loom_block_region_index(original[position]), position);
    }
    if (region->block_count == 1) {
      EXPECT_EQ(order.arena.block_pool, nullptr);
    }
    loom_print_block_order_deinitialize(&order);
    return result;
  }

  // Storage pool shared by the module and transient printer plans.
  iree_arena_block_pool_t pool_;
  // Minimal dialect registry for the constructed CFG terminators.
  loom_context_t context_;
  // Owns the region and its operation/value identities.
  loom_module_t* module_ = nullptr;
};

TEST_F(BlockOrderTest, SingleBlockNeedsNoAnalysis) {
  EXPECT_EQ(Order({{}}), (std::vector<uint16_t>{0}));
}

TEST_F(BlockOrderTest, PreservesOrderedSiblingsAndNoncontiguousSubtrees) {
  EXPECT_EQ(Order({{1, 2}, {3}, {4}, {4}, {}}),
            (std::vector<uint16_t>{0, 1, 2, 3, 4}));
}

TEST_F(BlockOrderTest, PullsLaterDominatorsAheadOfTheirUses) {
  EXPECT_EQ(Order({{3}, {}, {1}, {2}}), (std::vector<uint16_t>{0, 3, 2, 1}));
}

TEST_F(BlockOrderTest, LoopBackedgesDoNotRequireTopologicalControlFlow) {
  EXPECT_EQ(Order({{3}, {3}, {}, {1, 2}}), (std::vector<uint16_t>{0, 3, 1, 2}));
}

TEST_F(BlockOrderTest, PreservesIrreducibleSiblingOrder) {
  EXPECT_EQ(Order({{1, 2}, {2, 3}, {1, 3}, {}}),
            (std::vector<uint16_t>{0, 1, 2, 3}));
}

TEST_F(BlockOrderTest, KeepsUnreachableBlocksInOriginalOrderAfterLiveBlocks) {
  EXPECT_EQ(Order({{3}, {2}, {1}, {}}), (std::vector<uint16_t>{0, 3, 1, 2}));
}

TEST_F(BlockOrderTest, DeepDominanceChainUsesAnExplicitStack) {
  constexpr uint16_t kBlockCount = 4096;
  std::vector<std::vector<uint16_t>> successors(kBlockCount);
  successors[0] = {kBlockCount - 1};
  for (uint16_t i = 2; i < kBlockCount; ++i) {
    successors[i] = {uint16_t(i - 1)};
  }
  std::vector<uint16_t> expected = {0};
  for (uint16_t i = kBlockCount - 1; i > 0; --i) {
    expected.push_back(i);
  }
  EXPECT_EQ(Order(successors), expected);
}

}  // namespace
}  // namespace loom
