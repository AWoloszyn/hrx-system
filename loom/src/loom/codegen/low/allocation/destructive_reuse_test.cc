// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/codegen/low/allocation/destructive_reuse.h"

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace loom {
namespace {

class DestructiveReuseTest : public ::testing::Test {
 protected:
  void SetUp() override {
    iree_arena_block_pool_initialize(4096, iree_allocator_system(), &pool_);
    iree_arena_initialize(&pool_, &arena_);
  }

  void TearDown() override {
    iree_arena_deinitialize(&arena_);
    iree_arena_block_pool_deinitialize(&pool_);
  }

  // Two source words form a tuple, a fact identity forwards the tuple, and a
  // destructive result consumes that identity at point 4. Ordinals deliberately
  // put the identity and result before their sources, as CFG layout can do.
  void Refine(uint32_t low_end, uint32_t high_end, uint32_t write_point) {
    uint32_t unit_starts[] = {0, 2, 4, 6, 7};
    uint32_t unit_ends[] = {write_point, write_point,     6,
                            6,           write_point - 1, write_point - 1,
                            low_end,     high_end};
    loom_low_allocation_unit_liveness_t units = {};
    units.point_starts_by_value_ordinal = unit_starts;
    units.end_points = unit_ends;
    units.point_count = IREE_ARRAYSIZE(unit_ends);

    auto relation = [](loom_value_ordinal_t result, loom_value_ordinal_t source,
                       uint32_t result_offset, uint32_t count,
                       loom_low_placement_cause_t cause) {
      loom_low_placement_relation_t row = {};
      row.result_ordinal = result;
      row.source_ordinal = source;
      row.result_unit_offset = result_offset;
      row.unit_count = count;
      row.kind = cause == LOOM_LOW_PLACEMENT_CAUSE_LOW_CONCAT
                     ? LOOM_LOW_PLACEMENT_RELATION_CONTIGUOUS_PART
                     : LOOM_LOW_PLACEMENT_RELATION_SAME_STORAGE;
      row.cause = cause;
      row.flags = LOOM_LOW_PLACEMENT_RELATION_FLAG_CAN_ALIAS_STORAGE |
                  (cause == LOOM_LOW_PLACEMENT_CAUSE_TIED_RESULT
                       ? LOOM_LOW_PLACEMENT_RELATION_FLAG_HARD
                       : LOOM_LOW_PLACEMENT_RELATION_FLAG_PREFERRED);
      return row;
    };
    relations_[0] = relation(0, 2, 0, 2, LOOM_LOW_PLACEMENT_CAUSE_TIED_RESULT);
    relations_[1] = relation(1, 0, 0, 2, LOOM_LOW_PLACEMENT_CAUSE_TIED_RESULT);
    relations_[1].flags |= LOOM_LOW_PLACEMENT_RELATION_FLAG_WRITES_STORAGE;
    relations_[1].write_point = write_point;
    relations_[2] = relation(2, 3, 0, 1, LOOM_LOW_PLACEMENT_CAUSE_LOW_CONCAT);
    relations_[3] = relation(2, 4, 1, 1, LOOM_LOW_PLACEMENT_CAUSE_LOW_CONCAT);
    const loom_low_placement_relation_range_t ranges[] = {
        {0, 1}, {1, 1}, {2, 2}, {4, 0}, {4, 0}};
    loom_low_placement_table_t placement = {};
    placement.relations = relations_;
    placement.relation_count = IREE_ARRAYSIZE(relations_);
    placement.value_count = IREE_ARRAYSIZE(ranges);
    placement.ranges_by_result_ordinal = ranges;
    IREE_ASSERT_OK(loom_low_allocation_refine_destructive_reuse(
        &units, &placement, &arena_));
  }

  // Owns scratch blocks reused by each refinement.
  iree_arena_block_pool_t pool_;
  // Supplies temporary analysis storage.
  iree_arena_allocator_t arena_;
  // Retains permissions for the identity, write, and two borrowed words.
  loom_low_placement_relation_t relations_[4];
};

TEST_F(DestructiveReuseTest, PreservesOnlyTheLiveComponent) {
  Refine(/*low_end=*/7, /*high_end=*/2, /*write_point=*/4);
  EXPECT_TRUE(loom_low_placement_relation_can_alias(&relations_[0]));
  EXPECT_TRUE(loom_low_placement_relation_can_alias(&relations_[1]));
  EXPECT_FALSE(loom_low_placement_relation_can_alias(&relations_[2]));
  EXPECT_TRUE(loom_low_placement_relation_can_alias(&relations_[3]));
}

TEST_F(DestructiveReuseTest, KeepsIdentityUsesBeforeTheWriteCoalescible) {
  Refine(/*low_end=*/4, /*high_end=*/4, /*write_point=*/4);
  EXPECT_TRUE(loom_low_placement_relation_can_alias(&relations_[2]));
  EXPECT_TRUE(loom_low_placement_relation_can_alias(&relations_[3]));
}

TEST_F(DestructiveReuseTest, UsesTheAcceptedScheduleWritePoint) {
  Refine(/*low_end=*/4, /*high_end=*/4, /*write_point=*/3);
  EXPECT_FALSE(loom_low_placement_relation_can_alias(&relations_[2]));
  EXPECT_FALSE(loom_low_placement_relation_can_alias(&relations_[3]));
}

}  // namespace
}  // namespace loom
