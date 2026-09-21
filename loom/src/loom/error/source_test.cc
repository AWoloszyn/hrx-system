// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/error/source.h"

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/ir/context.h"
#include "loom/ir/module.h"

namespace {

TEST(SourceTest, HighlightOffsetsClampWhileCountingCodePoints) {
  const auto source = IREE_SV("\tαb\nlast\n");
  EXPECT_EQ(loom_source_byte_offset(source, 1, 1), 0u);
  EXPECT_EQ(loom_source_byte_offset(source, 1, 2), 1u);
  EXPECT_EQ(loom_source_byte_offset(source, 1, 3), 3u);
  EXPECT_EQ(loom_source_byte_offset(source, 1, 4), 4u);
  EXPECT_EQ(loom_source_byte_offset(source, 1, 99), 4u);
  EXPECT_EQ(loom_source_byte_offset(source, 2, 0), 5u);
  EXPECT_EQ(loom_source_byte_offset(source, 3, 1), source.size);
  EXPECT_EQ(loom_source_byte_offset(source, 99, 1), source.size);
  EXPECT_EQ(loom_source_byte_offset(source, 0, 99), 0u);
}

TEST(SourceTest, ExactResolutionRejectsUnavailableAndReversedCoordinates) {
  iree_arena_block_pool_t pool;
  iree_arena_block_pool_initialize(4096, iree_allocator_system(), &pool);
  loom_context_t context;
  loom_context_initialize(iree_allocator_system(), &context);
  IREE_ASSERT_OK(loom_context_finalize(&context));
  loom_module_t* module = nullptr;
  IREE_ASSERT_OK(loom_module_allocate(&context, IREE_SV("source"), &pool,
                                      nullptr, iree_allocator_system(),
                                      &module));
  loom_source_id_t source_id;
  IREE_ASSERT_OK(
      loom_module_register_source(module, IREE_SV("file"), &source_id));
  const loom_source_entry_t source = {source_id, IREE_SV("αb\nlast\n"),
                                      IREE_SV("file")};
  loom_source_table_resolver_t table = {&source, 1};
  const loom_source_resolver_t resolver = {loom_source_table_resolve, &table};
  auto resolve = [&](uint16_t first_line, uint16_t first_column,
                     uint16_t last_line, uint16_t last_column,
                     loom_source_range_t* out_range) {
    loom_location_id_t location;
    IREE_CHECK_OK(loom_module_add_location(
        module,
        loom_location_file_range(source_id, first_line, first_column, last_line,
                                 last_column),
        &location));
    return loom_source_resolve(resolver, module, location, out_range);
  };
  loom_source_range_t range;
  ASSERT_TRUE(resolve(1, 2, 2, 1, &range));
  EXPECT_EQ(range.start, 2u);
  EXPECT_EQ(range.end, 4u);
  EXPECT_EQ(range.source.data, source.source.data);
  EXPECT_EQ(range.provenance, LOOM_SOURCE_PROVENANCE_EXACT_SOURCE);
  EXPECT_TRUE(resolve(3, 1, 3, 1, &range));
  EXPECT_EQ(range.start, source.source.size);
  EXPECT_EQ(range.end, source.source.size);
  EXPECT_FALSE(resolve(0, 1, 1, 1, &range));
  EXPECT_FALSE(resolve(1, 0, 1, 1, &range));
  EXPECT_FALSE(resolve(4, 1, 4, 1, &range));
  EXPECT_FALSE(resolve(1, 4, 1, 4, &range));
  EXPECT_FALSE(resolve(1, 3, 1, 2, &range));
  EXPECT_FALSE(resolve(2, 1, 1, 1, &range));
  EXPECT_FALSE(
      loom_source_resolve(resolver, module, LOOM_LOCATION_UNKNOWN, &range));
  loom_module_free(module);
  loom_context_deinitialize(&context);
  iree_arena_block_pool_deinitialize(&pool);
}

}  // namespace
