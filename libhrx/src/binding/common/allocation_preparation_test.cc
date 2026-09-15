// Copyright 2026 The HRX Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "common/allocation_preparation.h"

#include <condition_variable>
#include <mutex>
#include <thread>

#include "iree/testing/gtest.h"

namespace iree::hal::streaming {
namespace {

TEST(AllocationPreparationTest, ClosingRejectsNewLeasesUntilReopened) {
  iree_hal_streaming_allocation_preparation_t preparation;
  iree_hal_streaming_allocation_preparation_initialize(&preparation);

  const bool initially_acquired =
      iree_hal_streaming_allocation_preparation_try_acquire(&preparation);
  EXPECT_TRUE(initially_acquired);
  if (initially_acquired) {
    iree_hal_streaming_allocation_preparation_release(&preparation);
  }

  iree_hal_streaming_allocation_preparation_begin_close(&preparation);
  EXPECT_FALSE(
      iree_hal_streaming_allocation_preparation_try_acquire(&preparation));
  iree_hal_streaming_allocation_preparation_await_idle(&preparation);

  iree_hal_streaming_allocation_preparation_reopen(&preparation);
  const bool acquired_after_reopen =
      iree_hal_streaming_allocation_preparation_try_acquire(&preparation);
  EXPECT_TRUE(acquired_after_reopen);
  if (acquired_after_reopen) {
    iree_hal_streaming_allocation_preparation_release(&preparation);
  }

  iree_hal_streaming_allocation_preparation_deinitialize(&preparation);
}

TEST(AllocationPreparationTest, ClosingOneAllocationDoesNotBlockAnother) {
  iree_hal_streaming_allocation_preparation_t first_preparation;
  iree_hal_streaming_allocation_preparation_initialize(&first_preparation);
  iree_hal_streaming_allocation_preparation_t second_preparation;
  iree_hal_streaming_allocation_preparation_initialize(&second_preparation);

  const bool first_acquired =
      iree_hal_streaming_allocation_preparation_try_acquire(&first_preparation);
  EXPECT_TRUE(first_acquired);
  iree_hal_streaming_allocation_preparation_begin_close(&first_preparation);

  std::mutex phase_mutex;
  std::condition_variable phase_notification;
  bool waiter_started = false;
  std::thread close_thread([&] {
    {
      std::lock_guard<std::mutex> lock(phase_mutex);
      waiter_started = true;
    }
    phase_notification.notify_all();
    iree_hal_streaming_allocation_preparation_await_idle(&first_preparation);
  });

  {
    std::unique_lock<std::mutex> lock(phase_mutex);
    phase_notification.wait(lock, [&] { return waiter_started; });
  }
  EXPECT_FALSE(iree_hal_streaming_allocation_preparation_try_acquire(
      &first_preparation));
  const bool second_acquired =
      iree_hal_streaming_allocation_preparation_try_acquire(
          &second_preparation);
  EXPECT_TRUE(second_acquired);
  if (second_acquired) {
    iree_hal_streaming_allocation_preparation_release(&second_preparation);
  }

  if (first_acquired) {
    iree_hal_streaming_allocation_preparation_release(&first_preparation);
  }
  close_thread.join();

  iree_hal_streaming_allocation_preparation_deinitialize(&second_preparation);
  iree_hal_streaming_allocation_preparation_deinitialize(&first_preparation);
}

}  // namespace
}  // namespace iree::hal::streaming
