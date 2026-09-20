// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Managed shared-notification integration with one receiving domain. Publishers
// use the native resource owner without creating another polling consumer.

#include <future>
#include <thread>
#include <vector>

#include "iree/async/cts/util/registry.h"
#include "iree/async/cts/util/test_base.h"
#include "iree/async/notification.h"
#include "iree/async/operations/scheduling.h"
#include "iree/async/relay.h"

#if !defined(IREE_PLATFORM_WINDOWS)
#include <poll.h>
#endif

namespace iree::async::cts {

class SharedNotificationTest : public CtsTestBase<> {
 protected:
  void SetUp() override {
    CtsTestBase<>::SetUp();
    if (HasFatalFailure() || IsSkipped()) {
      return;
    }
    if (!iree_async_notification_native_is_supported()) {
      GTEST_SKIP() << "Native shared notification waits are unavailable";
    }
    iree_notification_state_initialize(&state_);
    IREE_ASSERT_OK(
        iree_async_notification_native_initialize(&state_, &native_));
    IREE_ASSERT_OK(iree_async_notification_create_shared(proactor_, &native_,
                                                         &notification_));
  }

  void TearDown() override {
    iree_async_notification_release(notification_);
    CtsTestBase<>::TearDown();
    iree_async_notification_native_deinitialize(&native_);
  }

  void ExpectNativeReady() {
#if defined(IREE_PLATFORM_WINDOWS)
    EXPECT_EQ(
        WaitForSingleObject(
            (HANDLE)native_.async_event.wait_primitive.value.win32_handle, 0),
        WAIT_OBJECT_0);
#else
    struct pollfd descriptor = {native_.async_event.wait_primitive.value.fd,
                                POLLIN, 0};
    EXPECT_EQ(poll(&descriptor, 1, 0), 1);
    EXPECT_NE(descriptor.revents & POLLIN, 0);
#endif
  }

  void WaitForEnrollment(uint32_t count) {
    while ((uint32_t)iree_atomic_load(&state_.value,
                                      iree_memory_order_acquire) != count) {
      std::this_thread::yield();
    }
  }

  void VerifyPeerRelay(iree_async_relay_flags_t flags, int signal_count) {
    for (int i = 0; i < 7; ++i) {
      iree_async_notification_native_signal(&native_, 1);
    }
    iree_async_notification_t* sink = nullptr;
    IREE_ASSERT_OK(iree_async_notification_create(
        proactor_, IREE_ASYNC_NOTIFICATION_FLAG_NONE, &sink));
    iree_async_relay_t* relay = nullptr;
    IREE_ASSERT_OK(iree_async_proactor_register_relay(
        proactor_, iree_async_relay_source_from_notification(notification_),
        iree_async_relay_sink_signal_notification(sink, 1), flags,
        iree_async_relay_error_callback_none(), &relay));
    for (int i = 0; i < signal_count; ++i) {
      SCOPED_TRACE(i);
      const uint32_t sink_epoch = iree_async_notification_query_epoch(sink);
      iree_async_notification_native_signal(&native_, 1);
      PollUntilCondition(
          [&] {
            return iree_async_notification_query_epoch(sink) != sink_epoch;
          },
          "shared notification relay");
      EXPECT_EQ(iree_async_notification_query_epoch(sink), sink_epoch + 1);
      EXPECT_EQ(iree_async_notification_query_epoch(notification_), 8u + i);
    }
    if (iree_any_bit_set(flags, IREE_ASYNC_RELAY_FLAG_PERSISTENT)) {
      WaitForRelayUnregistration(relay);
    }
    iree_async_notification_release(sink);
  }

  // Caller-owned state; process tests exercise the separately mapped form.
  iree_notification_state_t state_ = {};
  // Native ownership independent of the receiving proactor.
  iree_async_notification_native_t native_ = {};
  // Sole managed receiver borrowing native_.
  iree_async_notification_t* notification_ = nullptr;
};

TEST_P(SharedNotificationTest, SharedEpochSignalAndQuery) {
  EXPECT_EQ(iree_async_notification_query_epoch(notification_), 0u);
  iree_async_notification_signal(notification_, 1);
  EXPECT_EQ(iree_async_notification_query_epoch(notification_), 1u);
  iree_async_notification_native_signal(&native_, 1);
  EXPECT_EQ(iree_async_notification_query_epoch(notification_), 2u);
  EXPECT_EQ(iree_notification_state_query_epoch(&state_), 2u);
}

// The publisher waits for real blocking enrollment, not a pre-wait promise.
TEST_P(SharedNotificationTest, SharedEpochSyncWait) {
  std::thread waiter([&] {
    uint32_t token = iree_async_notification_begin_observe(notification_);
    EXPECT_TRUE(iree_async_notification_wait_for_token(
        notification_, token, iree_infinite_timeout()));
    iree_async_notification_end_observe(notification_);
  });
  WaitForEnrollment(1);
  iree_async_notification_native_signal(&native_, 1);
  waiter.join();
  EXPECT_EQ(
      (uint32_t)iree_atomic_load(&state_.value, iree_memory_order_acquire), 0u);
}

// Synchronous timeout has no ownership of the async readiness channel.
TEST_P(SharedNotificationTest, SyncTimeoutPreservesAsyncReadiness) {
  iree_async_notification_release(notification_);
  notification_ = nullptr;
  iree_async_notification_native_signal(&native_, 1);
  uint32_t token = iree_notification_state_query_epoch(&state_);
  EXPECT_FALSE(iree_async_notification_native_wait_for_token(
      &native_, token, iree_make_timeout_ms(1)));
  ExpectNativeReady();
  EXPECT_EQ(
      (uint32_t)iree_atomic_load(&state_.value, iree_memory_order_acquire), 0u);
}

TEST_P(SharedNotificationTest, SyncAndAsyncObserversShareOnePublication) {
  constexpr int kWaiterCount = 4;
  std::vector<std::thread> waiters;
  for (int i = 0; i < kWaiterCount; ++i) {
    waiters.emplace_back([&] {
      uint32_t token = iree_async_notification_begin_observe(notification_);
      EXPECT_TRUE(iree_async_notification_wait_for_token(
          notification_, token, iree_infinite_timeout()));
      iree_async_notification_end_observe(notification_);
    });
  }

  CompletionTracker tracker;
  iree_async_notification_wait_operation_t wait = {};
  iree_async_operation_initialize(
      &wait.base, IREE_ASYNC_OPERATION_TYPE_NOTIFICATION_WAIT,
      IREE_ASYNC_OPERATION_FLAG_NONE, CompletionTracker::Callback, &tracker);
  wait.notification = notification_;
  IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &wait.base));
  iree_async_proactor_wake(proactor_);
  PollOneProgressEvent();
  EXPECT_EQ(tracker.call_count, 0);
  WaitForEnrollment(kWaiterCount);

  iree_async_notification_native_signal(&native_, IREE_ALL_WAITERS);
  PollUntilCondition([&] { return tracker.call_count == 1; },
                     "shared async notification with sync observers");
  for (auto& waiter : waiters) {
    waiter.join();
  }
  IREE_EXPECT_OK(tracker.ConsumeStatus());
  EXPECT_EQ(
      (uint32_t)iree_atomic_load(&state_.value, iree_memory_order_acquire), 0u);
}

TEST_P(SharedNotificationTest, SharedEpochAsyncWait) {
  CompletionTracker tracker;
  iree_async_notification_wait_operation_t wait = {};
  iree_async_operation_initialize(
      &wait.base, IREE_ASYNC_OPERATION_TYPE_NOTIFICATION_WAIT,
      IREE_ASYNC_OPERATION_FLAG_NONE, CompletionTracker::Callback, &tracker);
  wait.notification = notification_;
  IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &wait.base));
  iree_async_proactor_wake(proactor_);
  PollOneProgressEvent();
  EXPECT_EQ(tracker.call_count, 0);

  std::thread signaler(
      [&] { iree_async_notification_native_signal(&native_, 1); });
  PollUntilCondition([&] { return tracker.call_count == 1; });
  signaler.join();
  IREE_EXPECT_OK(tracker.ConsumeStatus());
}

TEST_P(SharedNotificationTest, DestroyDoesNotClosePrimitives) {
  iree_async_notification_release(notification_);
  notification_ = nullptr;
  iree_async_notification_native_signal(&native_, 1);
  ExpectNativeReady();
}

// Each cycle captures its token before publishing readiness to the signaler.
TEST_P(SharedNotificationTest, MultipleCycles) {
  constexpr int kCycles = 64;
  std::promise<void> ready[kCycles];
  std::thread worker([&] {
    for (int i = 0; i < kCycles; ++i) {
      uint32_t token = iree_async_notification_begin_observe(notification_);
      ready[i].set_value();
      EXPECT_TRUE(iree_async_notification_wait_for_token(
          notification_, token, iree_infinite_timeout()));
      iree_async_notification_end_observe(notification_);
    }
  });
  for (int i = 0; i < kCycles; ++i) {
    ready[i].get_future().wait();
    iree_async_notification_native_signal(&native_, 1);
  }
  worker.join();
  EXPECT_EQ(iree_async_notification_query_epoch(notification_), kCycles);
}

// Shared advisory signaling cannot rely on the managed local observer count.
TEST_P(SharedNotificationTest, AdvisorySignalWakesSharedObserver) {
  CompletionTracker tracker;
  iree_async_notification_wait_operation_t wait = {};
  iree_async_operation_initialize(
      &wait.base, IREE_ASYNC_OPERATION_TYPE_NOTIFICATION_WAIT,
      IREE_ASYNC_OPERATION_FLAG_NONE, CompletionTracker::Callback, &tracker);
  wait.notification = notification_;
  wait.wait_flags = IREE_ASYNC_NOTIFICATION_WAIT_FLAG_USE_WAIT_TOKEN;
  wait.wait_token = iree_async_notification_begin_observe(notification_);
  IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &wait.base));
  iree_async_notification_end_observe(notification_);
  iree_async_proactor_wake(proactor_);
  PollOneProgressEvent();
  EXPECT_EQ(tracker.call_count, 0);
  EXPECT_EQ(iree_atomic_load(&notification_->observer_count,
                             iree_memory_order_acquire),
            0);

  EXPECT_TRUE(iree_async_notification_signal_if_observed(notification_, 1));
  PollUntilCondition([&] { return tracker.call_count == 1; });
  IREE_EXPECT_OK(tracker.ConsumeStatus());
  EXPECT_EQ(iree_async_notification_query_epoch(notification_), 1u);
}

TEST_P(SharedNotificationTest, OneShotPeerRelay) {
  VerifyPeerRelay(IREE_ASYNC_RELAY_FLAG_NONE, 1);
}

TEST_P(SharedNotificationTest, PersistentPeerRelay) {
  VerifyPeerRelay(IREE_ASYNC_RELAY_FLAG_PERSISTENT, 4);
}

TEST_P(SharedNotificationTest, CancelRegisteredWaitWithoutSignal) {
  CompletionTracker tracker;
  iree_async_notification_wait_operation_t wait = {};
  iree_async_operation_initialize(
      &wait.base, IREE_ASYNC_OPERATION_TYPE_NOTIFICATION_WAIT,
      IREE_ASYNC_OPERATION_FLAG_NONE, CompletionTracker::Callback, &tracker);
  wait.notification = notification_;
  IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &wait.base));
  iree_async_proactor_wake(proactor_);
  PollOneProgressEvent();
  IREE_ASSERT_OK(iree_async_proactor_cancel(proactor_, &wait.base));
  PollUntilCondition([&] { return tracker.call_count == 1; });
  IREE_EXPECT_STATUS_IS(IREE_STATUS_CANCELLED, tracker.ConsumeStatus());
  EXPECT_EQ(iree_async_notification_query_epoch(notification_), 0u);
}

TEST_P(SharedNotificationTest, SharedSyncWaitTimeout) {
  const iree_time_t deadline = iree_time_now() + iree_make_duration_ms(10);
  EXPECT_FALSE(iree_async_notification_wait(notification_,
                                            iree_make_deadline(deadline)));
  EXPECT_GE(iree_time_now(), deadline);
  EXPECT_EQ(
      (uint32_t)iree_atomic_load(&state_.value, iree_memory_order_acquire), 0u);
}

TEST_P(SharedNotificationTest, SignalAfterNotificationAndProactorTeardown) {
  CompletionTracker tracker;
  iree_async_notification_wait_operation_t wait = {};
  iree_async_operation_initialize(
      &wait.base, IREE_ASYNC_OPERATION_TYPE_NOTIFICATION_WAIT,
      IREE_ASYNC_OPERATION_FLAG_NONE, CompletionTracker::Callback, &tracker);
  wait.notification = notification_;
  IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &wait.base));
  iree_async_proactor_wake(proactor_);
  PollOneProgressEvent();
  iree_async_notification_native_signal(&native_, 1);
  PollUntilCondition([&] { return tracker.call_count == 1; });
  IREE_EXPECT_OK(tracker.ConsumeStatus());
  iree_async_notification_release(notification_);
  notification_ = nullptr;
  iree_async_proactor_release(proactor_);
  proactor_ = nullptr;

  // A late lease-return thread owns only the native bundle and shared state.
  std::thread signaler(
      [&] { iree_async_notification_native_signal(&native_, 1); });
  signaler.join();
  ExpectNativeReady();
  EXPECT_EQ(iree_notification_state_query_epoch(&state_), 2u);
}

CTS_REGISTER_TEST_SUITE_WITH_TAGS(SharedNotificationTest,
                                  /*required=*/{"shared_notification"},
                                  /*excluded=*/{});

}  // namespace iree::async::cts
