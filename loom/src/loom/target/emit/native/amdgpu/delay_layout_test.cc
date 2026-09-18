// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/emit/native/amdgpu/delay_layout.h"

#include <array>

#include "iree/base/internal/arena.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/target/emit/native/amdgpu/branch_layout.h"

namespace loom {
namespace {

class AmdgpuDelayLayoutTest : public ::testing::Test {
 protected:
  void SetUp() override {
    iree_arena_block_pool_initialize(4096, iree_allocator_system(), &pool_);
    iree_arena_initialize(&pool_, &arena_);
  }

  void TearDown() override {
    iree_arena_deinitialize(&arena_);
    iree_arena_block_pool_deinitialize(&pool_);
  }

  // Owns reusable storage for the native layout's lifetime.
  iree_arena_block_pool_t pool_;
  // Owns the layouts built by each test.
  iree_arena_allocator_t arena_;
};

TEST_F(AmdgpuDelayLayoutTest, NativeSkipRange) {
  for (uint64_t distance : {1u, 2u, 3u, 4u, 5u, 6u}) {
    loom_amdgpu_delay_layout_builder_t builder;
    IREE_ASSERT_OK(
        loom_amdgpu_delay_layout_builder_initialize(2, &arena_, &builder));
    EXPECT_EQ(loom_amdgpu_delay_layout_record(&builder, 0, 10, 100, 5), 5);
    const uint16_t second =
        loom_amdgpu_delay_layout_record(&builder, 1, 12, 101 + distance, 3);
    if (distance <= 5) {
      EXPECT_EQ(second, 0);
      EXPECT_EQ(builder.immediates[0], 5u | (distance << 4) | (3u << 7));
      EXPECT_EQ(builder.immediates[1], 0);
      ASSERT_EQ(builder.span_count, 1u);
      EXPECT_EQ(builder.spans[0].first_packet_index, 10u);
      EXPECT_EQ(builder.spans[0].last_packet_index, 12u);
    } else {
      EXPECT_EQ(second, 3);
      EXPECT_EQ(builder.immediates[0], 5);
      EXPECT_EQ(builder.span_count, 0u);
    }
  }
}

TEST_F(AmdgpuDelayLayoutTest, BoundariesAndDualSelectorsStayLocal) {
  loom_amdgpu_delay_layout_builder_t builder;
  IREE_ASSERT_OK(
      loom_amdgpu_delay_layout_builder_initialize(4, &arena_, &builder));
  EXPECT_EQ(loom_amdgpu_delay_layout_record(&builder, 0, 0, 0, 1), 1);
  loom_amdgpu_delay_layout_end_span(&builder);
  EXPECT_EQ(loom_amdgpu_delay_layout_record(&builder, 1, 2, 2, 5), 5);
  constexpr uint16_t kDualSelector = 1u | (5u << 7);
  EXPECT_EQ(loom_amdgpu_delay_layout_record(&builder, 2, 3, 4, kDualSelector),
            kDualSelector);
  EXPECT_EQ(loom_amdgpu_delay_layout_record(&builder, 3, 4, 6, 2), 2);
  EXPECT_EQ(builder.span_count, 0u);
  EXPECT_EQ(builder.immediates[0], 1);
  EXPECT_EQ(builder.immediates[1], 5);
  EXPECT_EQ(builder.immediates[2], kDualSelector);
}

TEST_F(AmdgpuDelayLayoutTest, BranchIslandMovesOutsidePackedConsumers) {
  loom_amdgpu_delay_layout_builder_t builder;
  IREE_ASSERT_OK(
      loom_amdgpu_delay_layout_builder_initialize(2, &arena_, &builder));
  loom_amdgpu_delay_layout_record(&builder, 0, 10, 19998, 1);
  loom_amdgpu_delay_layout_record(&builder, 1, 12, 20001, 1);
  const loom_amdgpu_delay_layout_t delays = {
      /*immediates=*/builder.immediates,
      /*spans=*/builder.spans,
      /*span_count=*/builder.span_count,
  };
  const std::array blocks = {
      loom_amdgpu_branch_layout_block_t{0},
      loom_amdgpu_branch_layout_block_t{160008},
  };
  const std::array edges = {
      loom_amdgpu_branch_layout_input_edge_t{0, 1},
  };
  const std::array anchors = {
      loom_amdgpu_branch_layout_anchor_t{79992, 10},
      loom_amdgpu_branch_layout_anchor_t{80000, 11},
      loom_amdgpu_branch_layout_anchor_t{80004, 12},
      loom_amdgpu_branch_layout_anchor_t{80008, 13},
  };
  loom_amdgpu_branch_layout_input_t input = {
      /*byte_length=*/160012,
      /*blocks=*/blocks.data(),
      /*block_count=*/blocks.size(),
      /*edges=*/edges.data(),
      /*edge_count=*/edges.size(),
      /*anchors=*/anchors.data(),
      /*anchor_count=*/anchors.size(),
  };
  loom_amdgpu_branch_layout_t unprotected_layout;
  IREE_ASSERT_OK(
      loom_amdgpu_branch_layout_build(&input, &arena_, &unprotected_layout));
  ASSERT_EQ(unprotected_layout.group_count, 1u);
  EXPECT_EQ(unprotected_layout.groups[0].packet_index, 12u);

  std::array<loom_amdgpu_branch_layout_anchor_t, 4> legal_anchors;
  iree_host_size_t legal_count = 0;
  iree_host_size_t span_cursor = 0;
  for (const auto& anchor : anchors) {
    if (loom_amdgpu_delay_layout_allows_island(&delays, anchor.packet_index,
                                               &span_cursor)) {
      legal_anchors[legal_count++] = anchor;
    }
  }
  ASSERT_EQ(legal_count, 2u);
  input.anchors = legal_anchors.data();
  input.anchor_count = legal_count;
  loom_amdgpu_branch_layout_t protected_layout;
  IREE_ASSERT_OK(
      loom_amdgpu_branch_layout_build(&input, &arena_, &protected_layout));
  ASSERT_EQ(protected_layout.group_count, 1u);
  EXPECT_EQ(protected_layout.groups[0].packet_index, 13u);
  EXPECT_EQ(protected_layout.byte_length, input.byte_length + 8u);
}

}  // namespace
}  // namespace loom
