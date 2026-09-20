// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/util/segmented_storage.h"

#include <cstdint>

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace loom {
namespace {

class SegmentedStorageTest : public ::testing::Test {
 protected:
  static iree_status_t Allocate(void* self, iree_allocator_command_t command,
                                const void* parameters, void** pointer) {
    auto* test = static_cast<SegmentedStorageTest*>(self);
    if (command != IREE_ALLOCATOR_COMMAND_FREE) {
      const auto* allocation =
          static_cast<const iree_allocator_alloc_params_t*>(parameters);
      test->largest_allocation_ =
          iree_max(test->largest_allocation_, allocation->byte_length);
      if (test->allocation_count_++ == test->failure_index_) {
        return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                                "injected backing allocation failure");
      }
    }
    const auto allocator = iree_allocator_system();
    return allocator.ctl(allocator.self, command, parameters, pointer);
  }

  void InitializePool(iree_host_size_t block_size) {
    iree_arena_block_pool_initialize(block_size, {this, Allocate},
                                     &block_pool_);
    iree_arena_initialize(&block_pool_, &arena_);
  }

  void SetUp() override { InitializePool(128 * 1024); }

  void ResetPool(iree_host_size_t block_size) {
    iree_arena_deinitialize(&arena_);
    iree_arena_block_pool_deinitialize(&block_pool_);
    allocation_count_ = 0;
    largest_allocation_ = 0;
    failure_index_ = SIZE_MAX;
    InitializePool(block_size);
  }

  void TearDown() override {
    iree_arena_deinitialize(&arena_);
    iree_arena_block_pool_deinitialize(&block_pool_);
  }

  void AppendRows(loom_segmented_storage_t* storage, uint32_t count) {
    for (uint32_t i = storage->segment_count; i < count; ++i) {
      void* segment = nullptr;
      IREE_ASSERT_OK(loom_segmented_storage_append(storage, &arena_, &segment));
      *static_cast<uint32_t*>(segment) = i;
    }
  }

  void ExpectRows(const loom_segmented_storage_t& storage, uint32_t count) {
    ASSERT_EQ(storage.segment_count, count);
    for (uint32_t i = 0; i < count; ++i) {
      EXPECT_EQ(*static_cast<const uint32_t*>(
                    loom_segmented_storage_const_segment(&storage, i)),
                i);
    }
  }

  // Number of backing allocations attempted since reset.
  iree_host_size_t allocation_count_ = 0;
  // Largest requested backing allocation since reset, in bytes.
  iree_host_size_t largest_allocation_ = 0;
  // Allocation ordinal to fail, or SIZE_MAX to allow every request.
  iree_host_size_t failure_index_ = SIZE_MAX;
  // Shared backing pool retained across arena resets.
  iree_arena_block_pool_t block_pool_ = {};
  // Arena owning every payload and directory page in one test.
  iree_arena_allocator_t arena_ = {};
};

TEST_F(SegmentedStorageTest, InlineAndPrimaryPagePointersStayStable) {
  loom_segmented_storage_t storage;
  loom_segmented_storage_initialize(sizeof(uint64_t), alignof(uint64_t),
                                    &storage);

  constexpr uint32_t kSegmentCount =
      LOOM_SEGMENTED_STORAGE_INLINE_SEGMENT_COUNT + 1;
  uint64_t* pointers[kSegmentCount];
  for (uint32_t i = 0; i < kSegmentCount; ++i) {
    void* segment = nullptr;
    IREE_ASSERT_OK(loom_segmented_storage_append(&storage, &arena_, &segment));
    pointers[i] = static_cast<uint64_t*>(segment);
    *pointers[i] = 0xCAFE0000u + i;
  }

  EXPECT_EQ(storage.segment_count, kSegmentCount);
  for (uint32_t i = 0; i < kSegmentCount; ++i) {
    EXPECT_EQ(loom_segmented_storage_segment(&storage, i), pointers[i]);
    EXPECT_EQ(*static_cast<const uint64_t*>(
                  loom_segmented_storage_const_segment(&storage, i)),
              0xCAFE0000u + i);
  }
}

TEST_F(SegmentedStorageTest, SecondaryPointerPage) {
  loom_segmented_storage_t storage;
  loom_segmented_storage_initialize(sizeof(uint32_t), alignof(uint32_t),
                                    &storage);

  constexpr uint32_t kSegmentCount =
      LOOM_SEGMENTED_STORAGE_SEGMENTS_PER_PAGE + 1;
  for (uint32_t i = 0; i < kSegmentCount; ++i) {
    void* segment = nullptr;
    IREE_ASSERT_OK(loom_segmented_storage_append(&storage, &arena_, &segment));
    *static_cast<uint32_t*>(segment) = i;
  }

  EXPECT_EQ(storage.segment_count, kSegmentCount);
  for (uint32_t i = 0; i < kSegmentCount; ++i) {
    EXPECT_EQ(*static_cast<const uint32_t*>(
                  loom_segmented_storage_const_segment(&storage, i)),
              i);
  }
}

TEST_F(SegmentedStorageTest, MultiplePageGroupsPreserveThePrefix) {
  loom_segmented_storage_t storage;
  loom_segmented_storage_initialize(sizeof(uint32_t), alignof(uint32_t),
                                    &storage);
  constexpr uint32_t kSegmentCount =
      LOOM_SEGMENTED_STORAGE_SEGMENTS_PER_PAGE *
          LOOM_SEGMENTED_STORAGE_SEGMENTS_PER_PAGE +
      1;
  for (uint32_t i = 0; i < kSegmentCount; ++i) {
    void* segment = nullptr;
    IREE_ASSERT_OK(loom_segmented_storage_append(&storage, &arena_, &segment));
    *static_cast<uint32_t*>(segment) = i;
  }
  for (uint32_t i = 0; i < kSegmentCount; ++i) {
    EXPECT_EQ(*static_cast<const uint32_t*>(
                  loom_segmented_storage_const_segment(&storage, i)),
              i);
  }
}

TEST_F(SegmentedStorageTest, MoveInlineDirectory) {
  loom_segmented_storage_t source;
  loom_segmented_storage_initialize(sizeof(uint32_t), alignof(uint32_t),
                                    &source);
  for (uint32_t i = 0; i < 8; ++i) {
    void* segment = nullptr;
    IREE_ASSERT_OK(loom_segmented_storage_append(&source, &arena_, &segment));
    *static_cast<uint32_t*>(segment) = i;
  }

  loom_segmented_storage_t storage;
  loom_segmented_storage_move(&source, &storage);

  EXPECT_EQ(source.segment_count, 0u);
  EXPECT_EQ(source.primary_page, nullptr);
  EXPECT_EQ(storage.primary_page, nullptr);
  for (uint32_t i = 0; i < storage.segment_count; ++i) {
    EXPECT_EQ(*static_cast<const uint32_t*>(
                  loom_segmented_storage_const_segment(&storage, i)),
              i);
  }
}

TEST_F(SegmentedStorageTest, InlineDirectoryCopyIsSelfContained) {
  loom_segmented_storage_t source;
  loom_segmented_storage_initialize(sizeof(uint32_t), alignof(uint32_t),
                                    &source);
  for (uint32_t i = 0; i < 8; ++i) {
    void* segment = nullptr;
    IREE_ASSERT_OK(loom_segmented_storage_append(&source, &arena_, &segment));
    *static_cast<uint32_t*>(segment) = i;
  }

  loom_segmented_storage_t copy = source;
  source.inline_segments[0] = nullptr;

  EXPECT_NE(loom_segmented_storage_const_segment(&copy, 0), nullptr);
  for (uint32_t i = 0; i < copy.segment_count; ++i) {
    EXPECT_EQ(*static_cast<const uint32_t*>(
                  loom_segmented_storage_const_segment(&copy, i)),
              i);
  }
}

TEST_F(SegmentedStorageTest, MoveExpandedDirectory) {
  loom_segmented_storage_t source;
  loom_segmented_storage_initialize(sizeof(uint32_t), alignof(uint32_t),
                                    &source);
  constexpr uint32_t kSegmentCount =
      LOOM_SEGMENTED_STORAGE_SEGMENTS_PER_PAGE + 1;
  for (uint32_t i = 0; i < kSegmentCount; ++i) {
    void* segment = nullptr;
    IREE_ASSERT_OK(loom_segmented_storage_append(&source, &arena_, &segment));
    *static_cast<uint32_t*>(segment) = i;
  }
  auto* source_primary_page = source.primary_page;

  loom_segmented_storage_t storage;
  loom_segmented_storage_move(&source, &storage);

  EXPECT_EQ(source.segment_count, 0u);
  EXPECT_EQ(storage.primary_page, source_primary_page);
  for (uint32_t i = 0; i < storage.segment_count; ++i) {
    EXPECT_EQ(*static_cast<const uint32_t*>(
                  loom_segmented_storage_const_segment(&storage, i)),
              i);
  }
}

TEST_F(SegmentedStorageTest, ReadOnlyCopiesSurviveDirectoryPromotion) {
  loom_segmented_storage_t storage;
  loom_segmented_storage_initialize(sizeof(uint32_t), alignof(uint32_t),
                                    &storage);
  const uint32_t copy_counts[] = {
      LOOM_SEGMENTED_STORAGE_INLINE_SEGMENT_COUNT,
      LOOM_SEGMENTED_STORAGE_INLINE_SEGMENT_COUNT + 1,
      LOOM_SEGMENTED_STORAGE_SEGMENTS_PER_PAGE + 1,
  };
  loom_segmented_storage_t copies[IREE_ARRAYSIZE(copy_counts)];
  uint32_t copy_index = 0;
  for (uint32_t i = 0; i < 2 * LOOM_SEGMENTED_STORAGE_SEGMENTS_PER_PAGE + 1;
       ++i) {
    void* segment = nullptr;
    IREE_ASSERT_OK(loom_segmented_storage_append(&storage, &arena_, &segment));
    *static_cast<uint32_t*>(segment) = i;
    if (copy_index < IREE_ARRAYSIZE(copy_counts) &&
        storage.segment_count == copy_counts[copy_index]) {
      copies[copy_index++] = storage;
    }
  }
  ASSERT_EQ(copy_index, IREE_ARRAYSIZE(copy_counts));
  for (const auto& copy : copies) {
    for (uint32_t i = 0; i < copy.segment_count; ++i) {
      EXPECT_EQ(loom_segmented_storage_const_segment(&copy, i),
                loom_segmented_storage_const_segment(&storage, i));
      EXPECT_EQ(*static_cast<const uint32_t*>(
                    loom_segmented_storage_const_segment(&copy, i)),
                i);
    }
  }
}

TEST_F(SegmentedStorageTest, OveralignedPayloads) {
  loom_segmented_storage_t storage;
  loom_segmented_storage_initialize(/*segment_size=*/192,
                                    /*segment_alignment=*/256, &storage);

  for (uint32_t i = 0; i < 64; ++i) {
    void* segment = nullptr;
    IREE_ASSERT_OK(loom_segmented_storage_append(&storage, &arena_, &segment));
    EXPECT_EQ(reinterpret_cast<uintptr_t>(segment) % 256, 0u);
  }
}

TEST_F(SegmentedStorageTest, ArenaResetReusesPoolBlocks) {
  loom_segmented_storage_t storage;
  loom_segmented_storage_initialize(/*segment_size=*/4096,
                                    /*segment_alignment=*/64, &storage);
  for (uint32_t i = 0; i < 32; ++i) {
    void* segment = nullptr;
    IREE_ASSERT_OK(loom_segmented_storage_append(&storage, &arena_, &segment));
  }

  iree_arena_block_pool_statistics_t warm_statistics;
  iree_arena_block_pool_query_statistics(&block_pool_, &warm_statistics);
  const iree_host_size_t warm_allocation_count = allocation_count_;
  ASSERT_GT(warm_allocation_count, 0u);
  iree_arena_reset(&arena_);
  loom_segmented_storage_initialize(/*segment_size=*/4096,
                                    /*segment_alignment=*/64, &storage);
  for (uint32_t i = 0; i < 32; ++i) {
    void* segment = nullptr;
    IREE_ASSERT_OK(loom_segmented_storage_append(&storage, &arena_, &segment));
  }

  iree_arena_block_pool_statistics_t reused_statistics;
  iree_arena_block_pool_query_statistics(&block_pool_, &reused_statistics);
  EXPECT_EQ(reused_statistics.block_system_allocation_count,
            warm_statistics.block_system_allocation_count);
  EXPECT_EQ(reused_statistics.oversized_allocation_count,
            warm_statistics.oversized_allocation_count);
  EXPECT_EQ(allocation_count_, warm_allocation_count);
}

TEST_F(SegmentedStorageTest, NormalPoolSizesReuseDirectoryAndPayloadStorage) {
  for (const iree_host_size_t block_size : {32 * 1024u, 128 * 1024u}) {
    SCOPED_TRACE(block_size);
    ResetPool(block_size);
    iree_host_size_t warm_allocation_count = 0;
    for (uint32_t iteration = 0; iteration < 3; ++iteration) {
      loom_segmented_storage_t storage;
      loom_segmented_storage_initialize(sizeof(uint64_t), alignof(uint64_t),
                                        &storage);
      for (uint32_t i = 0; i < LOOM_SEGMENTED_STORAGE_SEGMENTS_PER_PAGE + 1;
           ++i) {
        void* segment = nullptr;
        IREE_ASSERT_OK(
            loom_segmented_storage_append(&storage, &arena_, &segment));
        *static_cast<uint64_t*>(segment) = i;
      }
      if (iteration == 0) {
        warm_allocation_count = allocation_count_;
        ASSERT_GT(warm_allocation_count, 0u);
      }
      EXPECT_EQ(allocation_count_, warm_allocation_count);
      EXPECT_LE(largest_allocation_, block_size);
      iree_arena_reset(&arena_);
    }
  }
}

TEST_F(SegmentedStorageTest, FailedAppendCanRewindAndRetryAtPageTransitions) {
  const uint32_t boundaries[] = {
      0,
      LOOM_SEGMENTED_STORAGE_INLINE_SEGMENT_COUNT,
      LOOM_SEGMENTED_STORAGE_SEGMENTS_PER_PAGE,
      LOOM_SEGMENTED_STORAGE_SEGMENTS_PER_PAGE + 1,
      2 * LOOM_SEGMENTED_STORAGE_SEGMENTS_PER_PAGE,
      LOOM_SEGMENTED_STORAGE_SEGMENTS_PER_PAGE *
          LOOM_SEGMENTED_STORAGE_SEGMENTS_PER_PAGE,
  };
  for (uint32_t boundary : boundaries) {
    SCOPED_TRACE(boundary);
    for (iree_host_size_t failure_index = 0;; ++failure_index) {
      SCOPED_TRACE(failure_index);
      // Valid small blocks expose each directory allocation to the backing
      // allocator. Each attempt starts without blocks from an earlier retry.
      ResetPool(128);
      loom_segmented_storage_t storage;
      loom_segmented_storage_initialize(sizeof(uint32_t), alignof(uint32_t),
                                        &storage);
      for (uint32_t i = 0; i < boundary; ++i) {
        void* segment = nullptr;
        IREE_ASSERT_OK(
            loom_segmented_storage_append(&storage, &arena_, &segment));
        *static_cast<uint32_t*>(segment) = i;
      }
      // Fill the current allocation block so the payload allocation itself,
      // as well as later directory allocations, has a fallible backing call.
      void* arena_tail = nullptr;
      IREE_ASSERT_OK(iree_arena_allocate(
          &arena_, iree_arena_block_pool_max_allocation_size(&block_pool_),
          &arena_tail));
      const loom_segmented_storage_t before = storage;
      const auto checkpoint = iree_arena_checkpoint_save(&arena_);
      allocation_count_ = 0;
      failure_index_ = failure_index;
      void* segment = nullptr;
      iree_status_t status =
          loom_segmented_storage_append(&storage, &arena_, &segment);
      failure_index_ = SIZE_MAX;
      if (iree_status_is_ok(status)) {
        EXPECT_LE(allocation_count_, failure_index);
        EXPECT_EQ(storage.segment_count, boundary + 1);
        break;
      }
      IREE_EXPECT_STATUS_IS(IREE_STATUS_RESOURCE_EXHAUSTED, status);
      EXPECT_EQ(allocation_count_, failure_index + 1);
      EXPECT_EQ(segment, nullptr);
      EXPECT_EQ(storage.segment_count, before.segment_count);
      EXPECT_EQ(storage.segment_size, before.segment_size);
      EXPECT_EQ(storage.segment_alignment, before.segment_alignment);
      EXPECT_EQ(storage.primary_page, before.primary_page);
      EXPECT_EQ(storage.page_directory, before.page_directory);
      for (uint32_t i = 0; i < LOOM_SEGMENTED_STORAGE_INLINE_SEGMENT_COUNT;
           ++i) {
        EXPECT_EQ(storage.inline_segments[i], before.inline_segments[i]);
      }
      iree_arena_checkpoint_restore(&checkpoint);
      IREE_ASSERT_OK(
          loom_segmented_storage_append(&storage, &arena_, &segment));
      *static_cast<uint32_t*>(segment) = boundary;
      for (uint32_t i = 0; i < storage.segment_count; ++i) {
        EXPECT_EQ(*static_cast<const uint32_t*>(
                      loom_segmented_storage_const_segment(&storage, i)),
                  i);
      }
    }
  }
}

TEST_F(SegmentedStorageTest, DiscardedExtensionsPreservePrefixAndRetry) {
  constexpr uint32_t kInline = LOOM_SEGMENTED_STORAGE_INLINE_SEGMENT_COUNT;
  constexpr uint32_t kPage = LOOM_SEGMENTED_STORAGE_SEGMENTS_PER_PAGE;
  struct AppendCase {
    // Published segment prefix preceding the candidate extension.
    uint32_t prefix_count;
    // Proposed segment count, crossing one or more directory boundaries.
    uint32_t extended_count;
  };
  const AppendCase cases[] = {
      {0, 4},
      {kInline - 1, kInline + 3},
      {kInline, kInline + 4},
      {kInline + 1, kInline + 5},
      {kPage - 1, kPage + 3},
      {kPage, kPage + 4},
      {kPage + 1, 2 * kPage + 3},
      {2 * kPage, 2 * kPage + 4},
      {kPage * kPage - 1, kPage * kPage + 3},
      {kPage * kPage, kPage * kPage + 4},
  };
  uint32_t partial_failures = 0;
  for (const auto& test : cases) {
    SCOPED_TRACE(test.prefix_count);
    ResetPool(128);
    loom_segmented_storage_t storage;
    loom_segmented_storage_initialize(sizeof(uint32_t), alignof(uint32_t),
                                      &storage);
    ASSERT_NO_FATAL_FAILURE(AppendRows(&storage, test.prefix_count));
    void* arena_tail = nullptr;
    IREE_ASSERT_OK(iree_arena_allocate(
        &arena_, iree_arena_block_pool_max_allocation_size(&block_pool_),
        &arena_tail));
    const auto checkpoint = iree_arena_checkpoint_save(&arena_);
    const auto used_bytes = arena_.used_allocation_size;
    const auto owned_bytes = arena_.total_allocation_size;
    const loom_segmented_storage_t prefix = storage;
    for (iree_host_size_t failure_index = 0;; ++failure_index) {
      SCOPED_TRACE(failure_index);
      loom_segmented_storage_t staged = storage;
      allocation_count_ = 0;
      failure_index_ = failure_index;
      iree_status_t status = iree_ok_status();
      for (uint32_t i = test.prefix_count;
           i < test.extended_count && iree_status_is_ok(status); ++i) {
        void* segment = nullptr;
        status = loom_segmented_storage_append(&staged, &arena_, &segment);
        if (iree_status_is_ok(status)) {
          *static_cast<uint32_t*>(segment) = i;
        }
      }
      failure_index_ = SIZE_MAX;
      const bool completed = iree_status_is_ok(status);
      if (completed) {
        EXPECT_LE(allocation_count_, failure_index);
      } else {
        IREE_EXPECT_STATUS_IS(IREE_STATUS_RESOURCE_EXHAUSTED, status);
        EXPECT_EQ(allocation_count_, failure_index + 1);
        partial_failures += staged.segment_count > test.prefix_count;
      }
      ASSERT_NO_FATAL_FAILURE(ExpectRows(storage, test.prefix_count));

      // Discard both partial and complete extensions. Trimming returned
      // blocks exercises stale unpublished slots before the original prefix
      // becomes the sole append owner again.
      iree_arena_checkpoint_restore(&checkpoint);
      EXPECT_EQ(arena_.used_allocation_size, used_bytes);
      EXPECT_EQ(arena_.total_allocation_size, owned_bytes);
      iree_arena_block_pool_trim(&block_pool_);
      ASSERT_NO_FATAL_FAILURE(ExpectRows(storage, test.prefix_count));
      ASSERT_NO_FATAL_FAILURE(AppendRows(&storage, test.extended_count));
      ASSERT_NO_FATAL_FAILURE(ExpectRows(storage, test.extended_count));
      if (completed) {
        break;
      }
      // Keep the same live prefix across attempts while returning every
      // extension block, so each failure ordinal has fresh backing calls.
      iree_arena_checkpoint_restore(&checkpoint);
      storage = prefix;
      iree_arena_block_pool_trim(&block_pool_);
    }
  }
  EXPECT_GT(partial_failures, 0u);
}

}  // namespace
}  // namespace loom
