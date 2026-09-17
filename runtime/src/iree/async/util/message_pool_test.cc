// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/async/util/message_pool.h"

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace {

TEST(MessagePoolTest, ReservationIsInvisibleAndReleasable) {
  iree_async_message_pool_entry_t storage[1];
  iree_async_message_pool_t pool;
  iree_async_message_pool_initialize(IREE_ARRAYSIZE(storage), storage, &pool);

  iree_async_message_pool_entry_t* entry = nullptr;
  IREE_ASSERT_OK(iree_async_message_pool_acquire(&pool, &entry));
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(iree_async_message_pool_flush(&pool), nullptr);

  iree_async_message_pool_entry_t* exhausted_entry = nullptr;
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_RESOURCE_EXHAUSTED,
      iree_async_message_pool_acquire(&pool, &exhausted_entry));
  EXPECT_EQ(exhausted_entry, nullptr);

  iree_async_message_pool_release(&pool, entry);
  entry = nullptr;
  IREE_ASSERT_OK(iree_async_message_pool_acquire(&pool, &entry));

  constexpr uint64_t kMessageData = 0xA11CE;
  iree_async_message_pool_publish(&pool, entry, kMessageData);
  iree_async_message_pool_entry_t* flushed_entry =
      iree_async_message_pool_flush(&pool);
  ASSERT_EQ(flushed_entry, entry);
  EXPECT_EQ(flushed_entry->message_data, kMessageData);
  EXPECT_EQ(iree_async_message_pool_entry_next(flushed_entry), nullptr);
  iree_async_message_pool_release(&pool, flushed_entry);

  iree_async_message_pool_deinitialize(&pool);
}

}  // namespace
