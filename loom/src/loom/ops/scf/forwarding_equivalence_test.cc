// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/ops/scf/forwarding_equivalence.h"

#include <cstddef>
#include <cstdint>
#include <vector>

#include "iree/base/internal/arena.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace loom {
namespace {

class ForwardingEquivalenceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    iree_arena_block_pool_initialize(128 * 1024, iree_allocator_system(),
                                     &block_pool_);
    iree_arena_initialize(&block_pool_, &scratch_arena_);
  }

  void TearDown() override {
    iree_arena_deinitialize(&scratch_arena_);
    iree_arena_block_pool_deinitialize(&block_pool_);
  }

  void Solve(const std::vector<loom_value_id_t>& initial_values,
             const std::vector<uint16_t>& successors,
             const std::vector<loom_value_id_t>& yielded_values,
             std::vector<uint16_t>* out_representatives,
             uint16_t* out_class_count,
             iree_host_size_t* out_scratch_bytes = nullptr) {
    ASSERT_EQ(initial_values.size(), successors.size());
    ASSERT_EQ(initial_values.size(), yielded_values.size());
    ASSERT_LE(initial_values.size(), UINT16_MAX);
    iree_arena_reset(&scratch_arena_);

    std::vector<uint16_t> mutable_successors = successors;
    out_representatives->resize(initial_values.size());
    const loom_scf_forwarding_equivalence_problem_t problem = {
        /*fact_table=*/nullptr,
        /*initial_values=*/initial_values.data(),
        /*yielded_values=*/yielded_values.data(),
        /*successors=*/mutable_successors.data(),
        /*count=*/static_cast<uint16_t>(initial_values.size()),
    };
    IREE_ASSERT_OK(loom_scf_forwarding_equivalence_partition(
        problem, &scratch_arena_, out_representatives->data(),
        out_class_count));
    if (out_scratch_bytes) {
      *out_scratch_bytes = scratch_arena_.used_allocation_size;
    }
  }

  iree_arena_block_pool_t block_pool_ = {};
  iree_arena_allocator_t scratch_arena_ = {};
};

static std::vector<uint16_t> ReferencePartition(
    const std::vector<loom_value_id_t>& initial_values,
    const std::vector<uint16_t>& successors,
    const std::vector<loom_value_id_t>& yielded_values) {
  const uint16_t count = static_cast<uint16_t>(initial_values.size());
  std::vector<uint16_t> colors(count);
  uint16_t color_count = 0;
  for (uint16_t i = 0; i < count; ++i) {
    uint16_t matching_color = UINT16_MAX;
    for (uint16_t j = 0; j < i; ++j) {
      const bool i_is_terminal = successors[i] == LOOM_SCF_FORWARDING_TERMINAL;
      const bool j_is_terminal = successors[j] == LOOM_SCF_FORWARDING_TERMINAL;
      if (initial_values[i] == initial_values[j] &&
          i_is_terminal == j_is_terminal &&
          (!i_is_terminal || yielded_values[i] == yielded_values[j])) {
        matching_color = colors[j];
        break;
      }
    }
    colors[i] = matching_color == UINT16_MAX ? color_count++ : matching_color;
  }

  std::vector<uint16_t> next_colors(count);
  while (true) {
    uint16_t next_color_count = 0;
    for (uint16_t i = 0; i < count; ++i) {
      uint16_t matching_color = UINT16_MAX;
      for (uint16_t j = 0; j < i; ++j) {
        if (colors[i] != colors[j]) {
          continue;
        }
        const bool i_is_terminal =
            successors[i] == LOOM_SCF_FORWARDING_TERMINAL;
        const bool j_is_terminal =
            successors[j] == LOOM_SCF_FORWARDING_TERMINAL;
        const bool transitions_match =
            i_is_terminal || j_is_terminal
                ? i_is_terminal == j_is_terminal &&
                      yielded_values[i] == yielded_values[j]
                : colors[successors[i]] == colors[successors[j]];
        if (transitions_match) {
          matching_color = next_colors[j];
          break;
        }
      }
      next_colors[i] =
          matching_color == UINT16_MAX ? next_color_count++ : matching_color;
    }
    colors = next_colors;
    if (next_color_count == color_count) {
      break;
    }
    color_count = next_color_count;
  }

  std::vector<uint16_t> representatives(count, UINT16_MAX);
  for (uint16_t i = 0; i < count; ++i) {
    for (uint16_t j = 0; j < count; ++j) {
      if (colors[i] == colors[j]) {
        representatives[i] = j;
        break;
      }
    }
  }
  return representatives;
}

TEST_F(ForwardingEquivalenceTest, EquivalentRotationUsesFirstState) {
  const std::vector<loom_value_id_t> initial_values = {7, 7, 7, 7};
  const std::vector<uint16_t> successors = {1, 2, 3, 0};
  const std::vector<loom_value_id_t> yielded_values = {10, 11, 12, 13};

  std::vector<uint16_t> representatives;
  uint16_t class_count = 0;
  Solve(initial_values, successors, yielded_values, &representatives,
        &class_count);

  EXPECT_EQ(class_count, 1);
  EXPECT_EQ(representatives, (std::vector<uint16_t>{0, 0, 0, 0}));
}

TEST_F(ForwardingEquivalenceTest, InitialAndTerminalIdentitiesColorStates) {
  const std::vector<loom_value_id_t> initial_values = {7, 7, 8, 7, 7, 7};
  const std::vector<uint16_t> successors = {
      1,
      0,
      2,
      LOOM_SCF_FORWARDING_TERMINAL,
      LOOM_SCF_FORWARDING_TERMINAL,
      LOOM_SCF_FORWARDING_TERMINAL,
  };
  const std::vector<loom_value_id_t> yielded_values = {20, 21, 22, 30, 30, 31};

  std::vector<uint16_t> representatives;
  uint16_t class_count = 0;
  Solve(initial_values, successors, yielded_values, &representatives,
        &class_count);

  EXPECT_EQ(class_count, 4);
  EXPECT_EQ(representatives, (std::vector<uint16_t>{0, 0, 2, 3, 3, 5}));
}

TEST_F(ForwardingEquivalenceTest, ExhaustiveSmallGraphsMatchReference) {
  for (uint16_t count = 1; count <= 4; ++count) {
    uint32_t transition_configuration_count = 1;
    for (uint16_t i = 0; i < count; ++i) {
      transition_configuration_count *= count + 2;
    }

    for (uint32_t initial_bits = 0; initial_bits < (1u << count);
         ++initial_bits) {
      for (uint32_t configuration = 0;
           configuration < transition_configuration_count; ++configuration) {
        std::vector<loom_value_id_t> initial_values(count);
        std::vector<uint16_t> successors(count);
        std::vector<loom_value_id_t> yielded_values(count);
        uint32_t remaining = configuration;
        for (uint16_t i = 0; i < count; ++i) {
          initial_values[i] = (initial_bits >> i) & 1u;
          const uint16_t transition =
              static_cast<uint16_t>(remaining % (count + 2));
          remaining /= count + 2;
          if (transition < count) {
            successors[i] = transition;
            yielded_values[i] = 100 + i;
          } else {
            successors[i] = LOOM_SCF_FORWARDING_TERMINAL;
            yielded_values[i] = 100 + transition - count;
          }
        }

        const std::vector<uint16_t> expected =
            ReferencePartition(initial_values, successors, yielded_values);
        std::vector<uint16_t> actual;
        uint16_t class_count = 0;
        Solve(initial_values, successors, yielded_values, &actual,
              &class_count);
        EXPECT_EQ(actual, expected)
            << "count=" << count << " initial_bits=" << initial_bits
            << " transition_configuration=" << configuration;
      }
    }
  }
}

TEST_F(ForwardingEquivalenceTest, WideRotationAndChainBoundScratch) {
  constexpr uint16_t kStateCount = 4096;
  std::vector<loom_value_id_t> initial_values(kStateCount, 0);
  std::vector<uint16_t> successors(kStateCount);
  std::vector<loom_value_id_t> yielded_values(kStateCount, 0);
  for (uint16_t i = 0; i < kStateCount; ++i) {
    successors[i] = static_cast<uint16_t>((i + 1) % kStateCount);
  }

  std::vector<uint16_t> representatives;
  uint16_t class_count = 0;
  iree_host_size_t scratch_bytes = 0;
  Solve(initial_values, successors, yielded_values, &representatives,
        &class_count, &scratch_bytes);
  EXPECT_EQ(class_count, 1);
  EXPECT_LE(scratch_bytes, 18 * kStateCount);
  for (uint16_t representative : representatives) {
    EXPECT_EQ(representative, 0);
  }

  initial_values.back() = 1;
  for (uint16_t i = 0; i < kStateCount; ++i) {
    successors[i] = i + 1 < kStateCount ? (uint16_t)(i + 1) : i;
  }
  Solve(initial_values, successors, yielded_values, &representatives,
        &class_count, &scratch_bytes);
  EXPECT_EQ(class_count, kStateCount);
  EXPECT_LE(scratch_bytes, 18 * kStateCount);
  for (uint16_t i = 0; i < kStateCount; ++i) {
    EXPECT_EQ(representatives[i], i);
  }
}

}  // namespace
}  // namespace loom
