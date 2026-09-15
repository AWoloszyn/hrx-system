// Copyright 2026 The HRX Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "binding/hip/array_lifetime.h"

#include <atomic>
#include <thread>

#include "iree/testing/gtest.h"

namespace {

TEST(ArrayLifetimeTest, ClosingDrainsAcceptedUsesAndRejectsNewUses) {
  iree_hip_array_lifetime_t lifetime;
  iree_hip_array_lifetime_initialize(&lifetime);
  ASSERT_TRUE(iree_hip_array_lifetime_try_acquire(&lifetime));
  iree_hip_array_lifetime_begin_close(&lifetime);
  EXPECT_FALSE(iree_hip_array_lifetime_try_acquire(&lifetime));

  std::atomic<bool> waiter_entered = false;
  std::atomic<bool> waiter_completed = false;
  std::thread waiter([&] {
    waiter_entered.store(true, std::memory_order_release);
    iree_hip_array_lifetime_await_idle(&lifetime);
    waiter_completed.store(true, std::memory_order_release);
  });
  while (!waiter_entered.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  EXPECT_FALSE(waiter_completed.load(std::memory_order_acquire));

  iree_hip_array_lifetime_release(&lifetime);
  waiter.join();
  EXPECT_TRUE(waiter_completed.load(std::memory_order_acquire));
  iree_hip_array_lifetime_deinitialize(&lifetime);
}

}  // namespace
