// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/arch/amdgpu/planning/occupancy.h"

#include "iree/base/internal/arena.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/target/arch/amdgpu/target_info.h"

namespace loom {
namespace {

class AmdgpuOccupancyTargetResourcesTest : public ::testing::Test {
 protected:
  void SetUp() override {
    iree_arena_block_pool_initialize(4096, iree_allocator_system(),
                                     &block_pool_);
    iree_arena_initialize(&block_pool_, &arena_);
  }

  void TearDown() override {
    iree_arena_deinitialize(&arena_);
    iree_arena_block_pool_deinitialize(&block_pool_);
  }

  loom_amdgpu_occupancy_target_resources_t Build(
      iree_string_view_t processor_name, uint32_t wave_size,
      uint32_t scalar_register_count, uint32_t vector_register_count,
      uint32_t flat_workgroup_size, uint32_t local_memory_bytes) {
    const loom_amdgpu_processor_info_t* processor =
        loom_amdgpu_target_info_find_processor(processor_name);
    EXPECT_NE(processor, nullptr);
    loom_amdgpu_occupancy_target_resources_t resources = {};
    IREE_EXPECT_OK(loom_amdgpu_occupancy_build_target_resources(
        processor, wave_size, scalar_register_count, vector_register_count,
        flat_workgroup_size, local_memory_bytes, &arena_, &resources));
    return resources;
  }

  iree_arena_block_pool_t block_pool_;
  iree_arena_allocator_t arena_;
};

TEST_F(AmdgpuOccupancyTargetResourcesTest,
       ReportsSelectedGenericTargetTransition) {
  const loom_amdgpu_occupancy_target_resources_t resources =
      Build(IREE_SV("gfx11-generic"), /*wave_size=*/64,
            /*scalar_register_count=*/32, /*vector_register_count=*/160,
            /*flat_workgroup_size=*/128, /*local_memory_bytes=*/0);
  const loom_target_residency_summary_t& summary = resources.residency_summary;

  EXPECT_TRUE(loom_target_residency_summary_is_valid(&summary));
  EXPECT_TRUE(iree_all_bits_set(
      summary.flags,
      LOOM_TARGET_RESIDENCY_SUMMARY_FLAG_HAS_NEXT_BETTER_TIER |
          LOOM_TARGET_RESIDENCY_SUMMARY_FLAG_HAS_UNIQUE_LIMITING_RESOURCE |
          LOOM_TARGET_RESIDENCY_SUMMARY_FLAG_HAS_LIMITING_RESOURCE_NEXT_WORSE_TIER));
  EXPECT_EQ(summary.best_tier, 16u);
  EXPECT_EQ(summary.tier, 3u);
  EXPECT_EQ(summary.next_better_tier, 4u);
  EXPECT_EQ(summary.limiting_resource_count, 1u);
  EXPECT_TRUE(iree_string_view_equal(summary.limiting_resource,
                                     IREE_SV("amdgpu.vgpr")));
  EXPECT_EQ(summary.limiting_resource_units, 160u);
  EXPECT_EQ(summary.limiting_resource_reduction_units_to_next_better_tier, 32u);
  EXPECT_GT(summary.limiting_resource_additional_units_to_next_worse_tier, 0u);
}

TEST_F(AmdgpuOccupancyTargetResourcesTest,
       DistinguishesConservativeGenericFromLargePoolExactProcessor) {
  const loom_target_residency_summary_t generic_summary =
      Build(IREE_SV("gfx11-generic"), /*wave_size=*/64,
            /*scalar_register_count=*/32, /*vector_register_count=*/160,
            /*flat_workgroup_size=*/128, /*local_memory_bytes=*/0)
          .residency_summary;
  const loom_target_residency_summary_t exact_summary =
      Build(IREE_SV("gfx1151"), /*wave_size=*/64,
            /*scalar_register_count=*/32, /*vector_register_count=*/160,
            /*flat_workgroup_size=*/128, /*local_memory_bytes=*/0)
          .residency_summary;

  EXPECT_TRUE(loom_target_residency_summary_is_valid(&generic_summary));
  EXPECT_TRUE(loom_target_residency_summary_is_valid(&exact_summary));
  EXPECT_EQ(generic_summary.tier, 3u);
  EXPECT_EQ(exact_summary.tier, 4u);
  EXPECT_EQ(generic_summary.next_better_tier, 4u);
  EXPECT_EQ(exact_summary.next_better_tier, 5u);
  EXPECT_GT(
      generic_summary.limiting_resource_reduction_units_to_next_better_tier,
      exact_summary.limiting_resource_reduction_units_to_next_better_tier);
  EXPECT_EQ(exact_summary.limiting_resource_reduction_units_to_next_better_tier,
            16u);
}

TEST_F(AmdgpuOccupancyTargetResourcesTest,
       OmitsTransitionWhenAgprCountIsUnavailable) {
  const loom_amdgpu_occupancy_target_resources_t resources =
      Build(IREE_SV("gfx942"), /*wave_size=*/64,
            /*scalar_register_count=*/32, /*vector_register_count=*/160,
            /*flat_workgroup_size=*/64, /*local_memory_bytes=*/0);

  EXPECT_FALSE(
      loom_target_residency_summary_is_valid(&resources.residency_summary));
}

TEST_F(AmdgpuOccupancyTargetResourcesTest,
       OmitsTransitionWithoutExactLaunchInformation) {
  const loom_amdgpu_occupancy_target_resources_t resources =
      Build(IREE_SV("gfx11-generic"), /*wave_size=*/64,
            /*scalar_register_count=*/32, /*vector_register_count=*/160,
            /*flat_workgroup_size=*/0, /*local_memory_bytes=*/0);

  EXPECT_FALSE(
      loom_target_residency_summary_is_valid(&resources.residency_summary));
}

TEST_F(AmdgpuOccupancyTargetResourcesTest,
       ReportsTransitionLimitedByLocalMemory) {
  const loom_amdgpu_occupancy_target_resources_t resources =
      Build(IREE_SV("gfx11-generic"), /*wave_size=*/64,
            /*scalar_register_count=*/32, /*vector_register_count=*/81,
            /*flat_workgroup_size=*/512, /*local_memory_bytes=*/65536);

  const loom_target_residency_summary_t& summary = resources.residency_summary;
  EXPECT_TRUE(loom_target_residency_summary_is_valid(&summary));
  EXPECT_TRUE(iree_string_view_equal(resources.limiting_resource,
                                     IREE_SV("amdgpu.lds")));
  EXPECT_EQ(summary.tier, 4u);
  // LDS permits six waves after the reduction, but registers cap the result
  // at five. The requirement is still the same first LDS allocation cliff.
  EXPECT_EQ(summary.next_better_tier, 5u);
  EXPECT_EQ(summary.limiting_resource_count, 1u);
  EXPECT_EQ(summary.limiting_resource_reduction_units_to_next_better_tier,
            22016u);
}

TEST_F(AmdgpuOccupancyTargetResourcesTest, ReportsCapturedSwiGluLdsTransition) {
  const auto resources = Build(IREE_SV("gfx1151"), /*wave_size=*/64,
                               /*scalar_register_count=*/36,
                               /*vector_register_count=*/136,
                               /*flat_workgroup_size=*/128,
                               /*local_memory_bytes=*/14848);
  const auto& summary = resources.residency_summary;
  EXPECT_TRUE(loom_target_residency_summary_is_valid(&summary));
  EXPECT_EQ(summary.tier, 4u);
  EXPECT_EQ(summary.next_better_tier, 5u);
  EXPECT_EQ(summary.limiting_resource_count, 1u);
  EXPECT_TRUE(
      iree_string_view_equal(summary.limiting_resource, IREE_SV("amdgpu.lds")));
  EXPECT_EQ(summary.limiting_resource_units, 14848u);
  EXPECT_EQ(summary.limiting_resource_reduction_units_to_next_better_tier,
            512u);
  EXPECT_EQ(summary.limiting_resource_next_worse_cliff_units, 18433u);
  EXPECT_EQ(summary.limiting_resource_additional_units_to_next_worse_tier,
            3585u);
  EXPECT_EQ(summary.limiting_resource_next_worse_tier, 3u);

  EXPECT_EQ(Build(IREE_SV("gfx1151"), 64, 36, 136, 128, 14336)
                .resident_waves_per_simd,
            5u);
  EXPECT_EQ(Build(IREE_SV("gfx1151"), 64, 36, 136, 128, 14337)
                .resident_waves_per_simd,
            4u);
  EXPECT_EQ(Build(IREE_SV("gfx1151"), 64, 36, 136, 128, 18432)
                .resident_waves_per_simd,
            4u);
  EXPECT_EQ(Build(IREE_SV("gfx1151"), 64, 36, 136, 128, 18433)
                .resident_waves_per_simd,
            3u);
}

TEST_F(AmdgpuOccupancyTargetResourcesTest, RequiresBothTiedResourcesToImprove) {
  const auto resources = Build(IREE_SV("gfx1151"), /*wave_size=*/64,
                               /*scalar_register_count=*/36,
                               /*vector_register_count=*/160,
                               /*flat_workgroup_size=*/128,
                               /*local_memory_bytes=*/14848);
  const auto& summary = resources.residency_summary;
  EXPECT_TRUE(loom_target_residency_summary_is_valid(&summary));
  EXPECT_EQ(summary.tier, 4u);
  EXPECT_EQ(summary.next_better_tier, 5u);
  EXPECT_EQ(summary.limiting_resource_count, 2u);
  EXPECT_FALSE(iree_any_bit_set(
      summary.flags,
      LOOM_TARGET_RESIDENCY_SUMMARY_FLAG_HAS_UNIQUE_LIMITING_RESOURCE));
  EXPECT_EQ(Build(IREE_SV("gfx1151"), 64, 36, 144, 128, 14848)
                .resident_waves_per_simd,
            4u);
  EXPECT_EQ(Build(IREE_SV("gfx1151"), 64, 36, 160, 128, 14336)
                .resident_waves_per_simd,
            4u);
  EXPECT_EQ(Build(IREE_SV("gfx1151"), 64, 36, 144, 128, 14336)
                .resident_waves_per_simd,
            5u);
}

TEST_F(AmdgpuOccupancyTargetResourcesTest, SkipsUnreachableLdsTiers) {
  const auto resources = Build(IREE_SV("gfx1100"), /*wave_size=*/32,
                               /*scalar_register_count=*/4,
                               /*vector_register_count=*/1,
                               /*flat_workgroup_size=*/32,
                               /*local_memory_bytes=*/2049);
  const auto& summary = resources.residency_summary;
  EXPECT_TRUE(loom_target_residency_summary_is_valid(&summary));
  EXPECT_EQ(summary.tier, 13u);
  EXPECT_EQ(summary.next_better_tier, 16u);
  EXPECT_EQ(summary.limiting_resource_reduction_units_to_next_better_tier, 1u);
  EXPECT_EQ(
      Build(IREE_SV("gfx1100"), 32, 4, 1, 32, 2048).resident_waves_per_simd,
      16u);
}

TEST_F(AmdgpuOccupancyTargetResourcesTest,
       ClipsRegisterTransitionToLdsCeiling) {
  const auto resources = Build(IREE_SV("gfx1151"), /*wave_size=*/64,
                               /*scalar_register_count=*/4,
                               /*vector_register_count=*/49,
                               /*flat_workgroup_size=*/128,
                               /*local_memory_bytes=*/5120);
  const auto& summary = resources.residency_summary;
  EXPECT_TRUE(loom_target_residency_summary_is_valid(&summary));
  EXPECT_EQ(summary.tier, 12u);
  EXPECT_EQ(summary.next_better_tier, 13u);
  EXPECT_EQ(summary.limiting_resource_count, 1u);
  EXPECT_TRUE(iree_string_view_equal(summary.limiting_resource,
                                     IREE_SV("amdgpu.vgpr")));
  EXPECT_EQ(summary.limiting_resource_reduction_units_to_next_better_tier, 1u);
  EXPECT_EQ(
      Build(IREE_SV("gfx1151"), 64, 4, 48, 128, 5120).resident_waves_per_simd,
      13u);
}

TEST_F(AmdgpuOccupancyTargetResourcesTest, RetainsFixedWorkgroupCeiling) {
  const auto resources = Build(IREE_SV("gfx1250"), /*wave_size=*/32,
                               /*scalar_register_count=*/4,
                               /*vector_register_count=*/1,
                               /*flat_workgroup_size=*/64,
                               /*local_memory_bytes=*/0);
  const auto& summary = resources.residency_summary;
  EXPECT_TRUE(loom_target_residency_summary_is_valid(&summary));
  EXPECT_EQ(summary.tier, 8u);
  EXPECT_EQ(summary.best_tier, 16u);
  EXPECT_EQ(summary.limiting_resource_count, 1u);
  EXPECT_FALSE(iree_any_bit_set(
      summary.flags, LOOM_TARGET_RESIDENCY_SUMMARY_FLAG_HAS_NEXT_BETTER_TIER));
  // A fixed ceiling is not a reducible resource footprint.
  EXPECT_FALSE(iree_any_bit_set(
      summary.flags,
      LOOM_TARGET_RESIDENCY_SUMMARY_FLAG_HAS_UNIQUE_LIMITING_RESOURCE));
  EXPECT_TRUE(iree_string_view_equal(resources.limiting_resource,
                                     IREE_SV("amdgpu.workgroup_slots")));

  const auto tied = Build(IREE_SV("gfx1250"), 32, 4, 128, 64, 20480);
  EXPECT_TRUE(loom_target_residency_summary_is_valid(&tied.residency_summary));
  EXPECT_EQ(tied.residency_summary.tier, 8u);
  EXPECT_EQ(tied.residency_summary.limiting_resource_count, 3u);
  EXPECT_FALSE(iree_any_bit_set(
      tied.residency_summary.flags,
      LOOM_TARGET_RESIDENCY_SUMMARY_FLAG_HAS_NEXT_BETTER_TIER));
}

TEST_F(AmdgpuOccupancyTargetResourcesTest, BracketsLocalMemoryTransitions) {
  struct TargetCase {
    // Exact processor whose final-metadata API is exercised.
    const char* processor;
    // Selected wave width in lanes.
    uint32_t wave_size;
  };
  const TargetCase targets[] = {
      {"gfx1100", 32}, {"gfx1151", 64}, {"gfx1250", 32}, {"gfx1201", 64}};
  const uint32_t workgroup_sizes[] = {1, 32, 33, 64, 65, 128, 256, 512, 1024};
  const uint32_t local_sizes[] = {0,    1,    511,   512,   513,  2048,
                                  2049, 8704, 14848, 32769, 65536};
  for (const auto& target : targets) {
    SCOPED_TRACE(target.processor);
    SCOPED_TRACE(target.wave_size);
    for (uint32_t workgroup_size : workgroup_sizes) {
      SCOPED_TRACE(workgroup_size);
      for (uint32_t local_size : local_sizes) {
        SCOPED_TRACE(local_size);
        const auto resources =
            Build(iree_make_cstring_view(target.processor), target.wave_size,
                  /*scalar_register_count=*/4, /*vector_register_count=*/1,
                  workgroup_size, local_size);
        const auto& summary = resources.residency_summary;
        ASSERT_TRUE(loom_target_residency_summary_is_valid(&summary));
        EXPECT_EQ(summary.tier, resources.resident_waves_per_simd);
        if (!iree_any_bit_set(
                summary.flags,
                LOOM_TARGET_RESIDENCY_SUMMARY_FLAG_HAS_NEXT_BETTER_TIER)) {
          continue;
        }
        ASSERT_TRUE(iree_string_view_equal(summary.limiting_resource,
                                           IREE_SV("amdgpu.lds")));
        ASSERT_GT(summary.limiting_resource_reduction_units_to_next_better_tier,
                  0u);
        const uint32_t boundary =
            local_size -
            summary.limiting_resource_reduction_units_to_next_better_tier;
        EXPECT_EQ(Build(iree_make_cstring_view(target.processor),
                        target.wave_size, 4, 1, workgroup_size, boundary)
                      .resident_waves_per_simd,
                  summary.next_better_tier);
        EXPECT_EQ(Build(iree_make_cstring_view(target.processor),
                        target.wave_size, 4, 1, workgroup_size, boundary + 1)
                      .resident_waves_per_simd,
                  summary.tier);
      }
    }
  }
}

TEST_F(AmdgpuOccupancyTargetResourcesTest,
       AppliesProcessorLaunchResourceLimits) {
  struct OccupancyCase {
    // Processor whose generated occupancy model is evaluated.
    const char* processor;
    // Wave width selected for the processor.
    uint32_t wave_size;
    // Fixed number of workitems in the workgroup.
    uint32_t flat_workgroup_size;
    // Fixed local-memory allocation in bytes.
    uint32_t local_memory_bytes;
    // Maximum resident waves admitted by the processor.
    uint32_t max_waves_per_simd;
    // Resident waves after applying launch resource limits.
    uint32_t resident_waves_per_simd;
    // Resident wave percentage after applying launch resource limits.
    uint32_t occupancy_percent;
    // Stable resource responsible for the resulting occupancy.
    const char* limiting_resource;
  };
  static constexpr OccupancyCase kCases[] = {
      {"gfx1100", 32, 64, 256, 16, 16, 100, "max_waves"},
      {"gfx1250", 32, 64, 256, 16, 8, 50, "amdgpu.workgroup_slots"},
      {"gfx1100", 32, 512, 32769, 16, 12, 75, "amdgpu.lds"},
      {"gfx942", 64, 512, 32769, 8, 2, 25, "amdgpu.lds"},
      {"gfx1250", 32, 512, 108545, 16, 8, 50, "amdgpu.lds"},
      {"gfx950", 64, 512, 108545, 8, 2, 25, "amdgpu.lds"},
  };

  for (const OccupancyCase& test_case : kCases) {
    SCOPED_TRACE(test_case.processor);
    const loom_amdgpu_occupancy_target_resources_t resources =
        Build(iree_make_cstring_view(test_case.processor), test_case.wave_size,
              /*scalar_register_count=*/4,
              /*vector_register_count=*/1, test_case.flat_workgroup_size,
              test_case.local_memory_bytes);

    EXPECT_TRUE(iree_string_view_equal(resources.scalar_register_class,
                                       IREE_SV("amdgpu.sgpr")));
    EXPECT_EQ(resources.scalar_register_count, 4u);
    EXPECT_TRUE(iree_string_view_equal(resources.vector_register_class,
                                       IREE_SV("amdgpu.vgpr")));
    EXPECT_EQ(resources.vector_register_count, 1u);
    EXPECT_EQ(resources.wave_size, test_case.wave_size);
    EXPECT_EQ(resources.max_waves_per_simd, test_case.max_waves_per_simd);
    EXPECT_EQ(resources.resident_waves_per_simd,
              test_case.resident_waves_per_simd);
    EXPECT_EQ(resources.occupancy_percent, test_case.occupancy_percent);
    EXPECT_TRUE(iree_string_view_equal(
        resources.limiting_resource,
        iree_make_cstring_view(test_case.limiting_resource)));
  }
}

}  // namespace
}  // namespace loom
