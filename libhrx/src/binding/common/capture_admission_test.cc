// Copyright 2026 The HRX Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "common/capture_admission.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace iree::hal::streaming {
namespace {

TEST(CaptureAdmissionTest, TransitionWaitsForEveryAdmittedOperation) {
  iree_hal_streaming_capture_admission_t admission;
  iree_hal_streaming_capture_admission_initialize(&admission);
  IREE_ASSERT_OK(iree_hal_streaming_capture_admission_enter(&admission));
  IREE_ASSERT_OK(iree_hal_streaming_capture_admission_enter(&admission));

  std::mutex phase_mutex;
  std::condition_variable phase_notification;
  bool transition_started = false;
  bool transition_entered = false;
  bool finish_transition = false;
  std::thread transition_thread([&] {
    {
      std::lock_guard<std::mutex> lock(phase_mutex);
      transition_started = true;
    }
    phase_notification.notify_all();
    iree_hal_streaming_capture_admission_begin_transition(&admission);
    {
      std::unique_lock<std::mutex> lock(phase_mutex);
      transition_entered = true;
      phase_notification.notify_all();
      phase_notification.wait(lock, [&] { return finish_transition; });
    }
    iree_hal_streaming_capture_admission_end_transition(&admission);
  });

  {
    std::unique_lock<std::mutex> lock(phase_mutex);
    phase_notification.wait(lock, [&] { return transition_started; });
  }
  iree_hal_streaming_capture_admission_leave(&admission);
  {
    std::lock_guard<std::mutex> lock(phase_mutex);
    EXPECT_FALSE(transition_entered);
  }
  iree_hal_streaming_capture_admission_leave(&admission);
  {
    std::unique_lock<std::mutex> lock(phase_mutex);
    phase_notification.wait(lock, [&] { return transition_entered; });
    finish_transition = true;
  }
  phase_notification.notify_all();
  transition_thread.join();

  IREE_ASSERT_OK(iree_hal_streaming_capture_admission_enter(&admission));
  iree_hal_streaming_capture_admission_leave(&admission);
  iree_hal_streaming_capture_admission_deinitialize(&admission);
}

TEST(CaptureAdmissionTest, ConcurrentAdmissionsAndTransitionsDoNotOverlap) {
  iree_hal_streaming_capture_admission_t admission;
  iree_hal_streaming_capture_admission_initialize(&admission);

  std::mutex start_mutex;
  std::condition_variable start_notification;
  bool start = false;
  std::atomic<int> active_operations{0};
  std::atomic<int> active_transitions{0};
  std::atomic<bool> overlap_detected{false};
  std::vector<std::thread> threads;
  for (int thread_index = 0; thread_index < 4; ++thread_index) {
    threads.emplace_back([&] {
      {
        std::unique_lock<std::mutex> lock(start_mutex);
        start_notification.wait(lock, [&] { return start; });
      }
      for (int iteration = 0; iteration < 10000; ++iteration) {
        iree_status_t status =
            iree_hal_streaming_capture_admission_enter(&admission);
        if (!iree_status_is_ok(status)) {
          overlap_detected.store(true, std::memory_order_relaxed);
          iree_status_ignore(status);
          return;
        }
        active_operations.fetch_add(1, std::memory_order_seq_cst);
        if (active_transitions.load(std::memory_order_seq_cst) != 0) {
          overlap_detected.store(true, std::memory_order_relaxed);
        }
        active_operations.fetch_sub(1, std::memory_order_seq_cst);
        iree_hal_streaming_capture_admission_leave(&admission);
      }
    });
  }
  for (int thread_index = 0; thread_index < 2; ++thread_index) {
    threads.emplace_back([&] {
      {
        std::unique_lock<std::mutex> lock(start_mutex);
        start_notification.wait(lock, [&] { return start; });
      }
      for (int iteration = 0; iteration < 2000; ++iteration) {
        iree_hal_streaming_capture_admission_begin_transition(&admission);
        if (active_transitions.fetch_add(1, std::memory_order_seq_cst) != 0 ||
            active_operations.load(std::memory_order_seq_cst) != 0) {
          overlap_detected.store(true, std::memory_order_relaxed);
        }
        active_transitions.fetch_sub(1, std::memory_order_seq_cst);
        iree_hal_streaming_capture_admission_end_transition(&admission);
      }
    });
  }

  {
    std::lock_guard<std::mutex> lock(start_mutex);
    start = true;
  }
  start_notification.notify_all();
  for (std::thread& thread : threads) {
    thread.join();
  }

  EXPECT_FALSE(overlap_detected.load(std::memory_order_relaxed));
  iree_hal_streaming_capture_admission_deinitialize(&admission);
}

}  // namespace
}  // namespace iree::hal::streaming
