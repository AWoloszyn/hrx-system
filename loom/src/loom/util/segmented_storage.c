// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/util/segmented_storage.h"

#include <string.h>

static iree_status_t loom_segmented_storage_ensure_page(
    loom_segmented_storage_t* storage, iree_arena_allocator_t* arena,
    uint32_t segment_index, loom_segmented_storage_page_t** out_page) {
  const uint32_t page_index =
      segment_index >> LOOM_SEGMENTED_STORAGE_PAGE_SHIFT;
  if (page_index == 0) {
    if (storage->primary_page == NULL) {
      loom_segmented_storage_page_t* page = NULL;
      IREE_RETURN_IF_ERROR(
          iree_arena_allocate(arena, sizeof(*page), (void**)&page));
      memcpy(page->segments, storage->inline_segments,
             sizeof(storage->inline_segments));
      storage->primary_page = page;
    }
    *out_page = storage->primary_page;
    return iree_ok_status();
  }

  const uint32_t group_index = page_index >> LOOM_SEGMENTED_STORAGE_PAGE_SHIFT;
  const uint32_t group_slot = page_index & LOOM_SEGMENTED_STORAGE_PAGE_MASK;
  loom_segmented_storage_directory_t* directory = storage->page_directory;
  if ((segment_index & LOOM_SEGMENTED_STORAGE_PAGE_MASK) != 0) {
    *out_page = directory->groups[group_index]->pages[group_slot];
    return iree_ok_status();
  }

  // Sequential append identifies new pages and groups without reading unused
  // slots. All fallible allocations precede publication into shared pages.
  loom_segmented_storage_page_group_t* group = NULL;
  if (directory == NULL) {
    IREE_RETURN_IF_ERROR(
        iree_arena_allocate(arena, sizeof(*directory), (void**)&directory));
    IREE_RETURN_IF_ERROR(
        iree_arena_allocate(arena, sizeof(*group), (void**)&group));
    group->pages[0] = storage->primary_page;
  } else if (group_slot == 0) {
    IREE_RETURN_IF_ERROR(
        iree_arena_allocate(arena, sizeof(*group), (void**)&group));
  } else {
    group = directory->groups[group_index];
  }
  loom_segmented_storage_page_t* page = NULL;
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate(arena, sizeof(*page), (void**)&page));
  group->pages[group_slot] = page;
  directory->groups[group_index] = group;
  storage->page_directory = directory;
  *out_page = page;
  return iree_ok_status();
}

void loom_segmented_storage_initialize(iree_host_size_t segment_size,
                                       iree_host_size_t segment_alignment,
                                       loom_segmented_storage_t* out_storage) {
  IREE_ASSERT_ARGUMENT(out_storage);
  IREE_ASSERT(segment_size > 0);
  IREE_ASSERT(iree_host_size_is_power_of_two(segment_alignment));
  memset(out_storage, 0, sizeof(*out_storage));
  out_storage->segment_size = segment_size;
  out_storage->segment_alignment = segment_alignment;
}

void loom_segmented_storage_move(loom_segmented_storage_t* source,
                                 loom_segmented_storage_t* out_storage) {
  IREE_ASSERT_ARGUMENT(source);
  IREE_ASSERT_ARGUMENT(out_storage);
  *out_storage = *source;
  *source = (loom_segmented_storage_t){0};
}

iree_status_t loom_segmented_storage_append(loom_segmented_storage_t* storage,
                                            iree_arena_allocator_t* arena,
                                            void** out_segment) {
  IREE_ASSERT_ARGUMENT(storage);
  IREE_ASSERT_ARGUMENT(arena);
  IREE_ASSERT_ARGUMENT(out_segment);
  *out_segment = NULL;

  if (IREE_UNLIKELY(storage->segment_count >=
                    LOOM_SEGMENTED_STORAGE_MAX_SEGMENT_COUNT)) {
    return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                            "segmented storage exceeds %u segments",
                            LOOM_SEGMENTED_STORAGE_MAX_SEGMENT_COUNT);
  }

  const uint32_t segment_index = storage->segment_count;
  void* segment = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_aligned(
      arena, storage->segment_size, storage->segment_alignment, &segment));
  void** segments = storage->inline_segments;
  if (segment_index >= LOOM_SEGMENTED_STORAGE_INLINE_SEGMENT_COUNT) {
    loom_segmented_storage_page_t* page = NULL;
    IREE_RETURN_IF_ERROR(loom_segmented_storage_ensure_page(
        storage, arena, segment_index, &page));
    segments = page->segments;
  }
  segments[segment_index & LOOM_SEGMENTED_STORAGE_PAGE_MASK] = segment;
  ++storage->segment_count;
  *out_segment = segment;
  return iree_ok_status();
}
