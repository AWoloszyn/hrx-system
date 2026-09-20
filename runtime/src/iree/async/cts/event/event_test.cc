// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// CTS tests for event operations.
//
// Events are the building block for cross-thread signaling. These tests verify
// that event creation, signaling, waiting, and reset work correctly across
// single-threaded and multi-threaded scenarios.

#include "iree/async/event.h"

#include <thread>

#include "iree/async/cts/util/registry.h"
#include "iree/async/cts/util/test_base.h"
#include "iree/async/operations/scheduling.h"

#if defined(IREE_PLATFORM_WINDOWS)
#include <windows.h>
#elif defined(IREE_ASYNC_HAVE_FD)
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif  // native handle type

namespace iree::async::cts {

class EventTest : public CtsTestBase<> {};

// Create event, signal from same thread, poll - callback fires.
TEST_P(EventTest, SameThreadSignal) {
  iree_async_event_t* event = nullptr;
  IREE_ASSERT_OK(iree_async_event_create(proactor_, &event));

  iree_async_event_wait_operation_t wait_op;
  memset(&wait_op, 0, sizeof(wait_op));
  wait_op.base.type = IREE_ASYNC_OPERATION_TYPE_EVENT_WAIT;
  wait_op.event = event;

  CompletionTracker tracker;
  wait_op.base.completion_fn = CompletionTracker::Callback;
  wait_op.base.user_data = &tracker;

  IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &wait_op.base));

  // Signal the event from the same thread.
  iree_async_event_set(event);

  // Poll should pick up the signaled event.
  PollUntil(/*min_completions=*/1);

  EXPECT_EQ(tracker.call_count, 1);
  IREE_EXPECT_OK(tracker.ConsumeStatus());
  EXPECT_EQ(tracker.last_operation, &wait_op.base);

  iree_async_event_release(event);
}

// Create event, signal from another thread, poll - callback fires.
TEST_P(EventTest, CrossThreadSignal) {
  iree_async_event_t* event = nullptr;
  IREE_ASSERT_OK(iree_async_event_create(proactor_, &event));

  iree_async_event_wait_operation_t wait_op;
  memset(&wait_op, 0, sizeof(wait_op));
  wait_op.base.type = IREE_ASYNC_OPERATION_TYPE_EVENT_WAIT;
  wait_op.event = event;

  CompletionTracker tracker;
  wait_op.base.completion_fn = CompletionTracker::Callback;
  wait_op.base.user_data = &tracker;

  IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &wait_op.base));

  // Publication may precede native wait registration after admission; the
  // native event retains readiness until the wait consumes it.
  std::thread signaler([event]() { iree_async_event_set(event); });

  // Poll should pick up the signaled event.
  PollUntil(/*min_completions=*/1);

  signaler.join();

  EXPECT_EQ(tracker.call_count, 1);
  IREE_EXPECT_OK(tracker.ConsumeStatus());

  iree_async_event_release(event);
}

// Multiple events, signal subset, verify correct callbacks.
TEST_P(EventTest, MultipleEventsPartialSignal) {
  constexpr int kEventCount = 3;
  iree_async_event_t* events[kEventCount] = {};
  iree_async_event_wait_operation_t wait_ops[kEventCount];
  CompletionTracker trackers[kEventCount];

  // Create events and wait operations.
  for (int i = 0; i < kEventCount; ++i) {
    IREE_ASSERT_OK(iree_async_event_create(proactor_, &events[i]));
    memset(&wait_ops[i], 0, sizeof(wait_ops[i]));
    wait_ops[i].base.type = IREE_ASYNC_OPERATION_TYPE_EVENT_WAIT;
    wait_ops[i].event = events[i];
    wait_ops[i].base.completion_fn = CompletionTracker::Callback;
    wait_ops[i].base.user_data = &trackers[i];
  }

  // Submit all wait operations.
  iree_async_operation_t* ops[] = {&wait_ops[0].base, &wait_ops[1].base,
                                   &wait_ops[2].base};
  iree_async_operation_list_t list = {ops, kEventCount};
  IREE_ASSERT_OK(iree_async_proactor_submit(proactor_, list));

  // Signal only events 0 and 2, leaving event 1 unsignaled.
  iree_async_event_set(events[0]);
  iree_async_event_set(events[2]);

  // Poll should pick up exactly 2 completions.
  PollUntil(/*min_completions=*/2);

  EXPECT_EQ(trackers[0].call_count, 1);
  IREE_EXPECT_OK(trackers[0].ConsumeStatus());
  EXPECT_EQ(trackers[1].call_count, 0);  // Not signaled.
  EXPECT_EQ(trackers[2].call_count, 1);
  IREE_EXPECT_OK(trackers[2].ConsumeStatus());

  // Now signal event 1.
  iree_async_event_set(events[1]);
  PollUntil(/*min_completions=*/1);

  EXPECT_EQ(trackers[1].call_count, 1);
  IREE_EXPECT_OK(trackers[1].ConsumeStatus());

  for (int i = 0; i < kEventCount; ++i) {
    iree_async_event_release(events[i]);
  }
}

// Event reset after signal, re-wait works.
TEST_P(EventTest, ResetAndReWait) {
  iree_async_event_t* event = nullptr;
  IREE_ASSERT_OK(iree_async_event_create(proactor_, &event));

  // First wait/signal cycle.
  {
    iree_async_event_wait_operation_t wait_op;
    memset(&wait_op, 0, sizeof(wait_op));
    wait_op.base.type = IREE_ASYNC_OPERATION_TYPE_EVENT_WAIT;
    wait_op.event = event;

    CompletionTracker tracker;
    wait_op.base.completion_fn = CompletionTracker::Callback;
    wait_op.base.user_data = &tracker;

    IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &wait_op.base));
    iree_async_event_set(event);
    PollUntil(/*min_completions=*/1);

    EXPECT_EQ(tracker.call_count, 1);
    IREE_EXPECT_OK(tracker.ConsumeStatus());

    // Event is automatically drained by the proactor when the wait completes
    // (e.g., via linked POLL_ADD+READ on io_uring).
  }

  // Second wait/signal cycle should work identically.
  {
    iree_async_event_wait_operation_t wait_op;
    memset(&wait_op, 0, sizeof(wait_op));
    wait_op.base.type = IREE_ASYNC_OPERATION_TYPE_EVENT_WAIT;
    wait_op.event = event;

    CompletionTracker tracker;
    wait_op.base.completion_fn = CompletionTracker::Callback;
    wait_op.base.user_data = &tracker;

    IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &wait_op.base));
    iree_async_event_set(event);
    PollUntil(/*min_completions=*/1);

    EXPECT_EQ(tracker.call_count, 1);
    IREE_EXPECT_OK(tracker.ConsumeStatus());
  }

  iree_async_event_release(event);
}

// Pre-signaled event: signal before wait submission still completes.
TEST_P(EventTest, PreSignaledEvent) {
  iree_async_event_t* event = nullptr;
  IREE_ASSERT_OK(iree_async_event_create(proactor_, &event));

  // Signal BEFORE submitting the wait.
  iree_async_event_set(event);

  iree_async_event_wait_operation_t wait_op;
  memset(&wait_op, 0, sizeof(wait_op));
  wait_op.base.type = IREE_ASYNC_OPERATION_TYPE_EVENT_WAIT;
  wait_op.event = event;

  CompletionTracker tracker;
  wait_op.base.completion_fn = CompletionTracker::Callback;
  wait_op.base.user_data = &tracker;

  IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &wait_op.base));

  // Should complete immediately since the event was already signaled.
  PollUntil(/*min_completions=*/1);

  EXPECT_EQ(tracker.call_count, 1);
  IREE_EXPECT_OK(tracker.ConsumeStatus());

  iree_async_event_release(event);
}

// Retain/release pair on event doesn't crash.
TEST_P(EventTest, RetainRelease) {
  iree_async_event_t* event = nullptr;
  IREE_ASSERT_OK(iree_async_event_create(proactor_, &event));

  // Retain bumps refcount.
  iree_async_event_retain(event);

  // Release decrements but shouldn't destroy (create holds a ref).
  iree_async_event_release(event);

  // Event should still be usable.
  iree_async_event_set(event);

  // Final release destroys.
  iree_async_event_release(event);
}

CTS_REGISTER_TEST_SUITE(EventTest);

#if defined(IREE_ASYNC_HAVE_FD) || defined(IREE_ASYNC_HAVE_WIN32_HANDLE)

class NativeEventTest : public CtsTestBase<> {
 protected:
  void TearDown() override {
    CtsTestBase<>::TearDown();
    iree_async_event_native_deinitialize(&native_);
  }

  void ExpectReady() {
#if defined(IREE_PLATFORM_WINDOWS)
    EXPECT_EQ(WaitForSingleObject(
                  (HANDLE)native_.wait_primitive.value.win32_handle, 0),
              WAIT_OBJECT_0);
#else
    pollfd descriptor = {native_.wait_primitive.value.fd, POLLIN, 0};
    EXPECT_EQ(poll(&descriptor, 1, 0), 1);
    EXPECT_NE(descriptor.revents & POLLIN, 0);
#endif  // IREE_PLATFORM_WINDOWS
  }

  // Owned independently of the fixture's proactor and any managed borrower.
  iree_async_event_native_t native_ = {};
};

TEST_P(NativeEventTest, NonblockingNoninheritableHandles) {
  IREE_ASSERT_OK(iree_async_event_native_initialize(&native_));
  for (auto primitive : {native_.wait_primitive, native_.signal_primitive}) {
#if defined(IREE_PLATFORM_WINDOWS)
    DWORD flags = 0;
    ASSERT_TRUE(
        GetHandleInformation((HANDLE)primitive.value.win32_handle, &flags));
    EXPECT_EQ(flags & HANDLE_FLAG_INHERIT, 0u);
#else
    int flags = fcntl(primitive.value.fd, F_GETFL);
    ASSERT_GE(flags, 0);
    EXPECT_NE(flags & O_NONBLOCK, 0);
    flags = fcntl(primitive.value.fd, F_GETFD);
    ASSERT_GE(flags, 0);
    EXPECT_NE(flags & FD_CLOEXEC, 0);
#endif  // IREE_PLATFORM_WINDOWS
  }
  iree_async_event_native_set(&native_);
  ExpectReady();
}

TEST_P(NativeEventTest, SaturationIsAlreadySignaled) {
  IREE_ASSERT_OK(iree_async_event_native_initialize(&native_));
#if defined(IREE_PLATFORM_WINDOWS)
  iree_async_event_native_set(&native_);
#elif defined(IREE_ASYNC_HAVE_EVENTFD)
  const uint64_t value = UINT64_MAX - 1;
  ASSERT_EQ(write(native_.signal_primitive.value.fd, &value, sizeof(value)),
            sizeof(value));
#else
  // With no consumer, a nonblocking pipe reaches its native capacity. The
  // additional public set below must succeed without blocking or draining it.
  const uint8_t value = 1;
  ssize_t result;
  do {
    result = write(native_.signal_primitive.value.fd, &value, sizeof(value));
  } while (result == sizeof(value) || (result < 0 && errno == EINTR));
  ASSERT_EQ(result, -1);
  ASSERT_EQ(errno, EAGAIN);
#endif  // native event type
  iree_async_event_native_set(&native_);
  ExpectReady();
#if defined(IREE_PLATFORM_WINDOWS)
  // Auto-reset consumes the coalesced signal once, not one credit per set.
  EXPECT_EQ(
      WaitForSingleObject((HANDLE)native_.wait_primitive.value.win32_handle, 0),
      WAIT_TIMEOUT);
#endif  // IREE_PLATFORM_WINDOWS
}

TEST_P(NativeEventTest, SignalAfterNotificationAndProactorTeardown) {
  IREE_ASSERT_OK(iree_async_event_native_initialize(&native_));
  iree_atomic_int32_t epoch = IREE_ATOMIC_VAR_INIT(0);
  iree_async_notification_shared_options_t options = {};
  options.epoch_address = &epoch;
  options.wake_primitive = native_.wait_primitive;
  options.signal_primitive = native_.signal_primitive;
  iree_async_notification_t* notification = nullptr;
  IREE_ASSERT_OK(iree_async_notification_create_shared(proactor_, &options,
                                                       &notification));

  CompletionTracker tracker;
  iree_async_notification_wait_operation_t wait = {};
  iree_async_operation_initialize(
      &wait.base, IREE_ASYNC_OPERATION_TYPE_NOTIFICATION_WAIT,
      IREE_ASYNC_OPERATION_FLAG_NONE, CompletionTracker::Callback, &tracker);
  wait.notification = notification;
  wait.wait_flags = IREE_ASYNC_NOTIFICATION_WAIT_FLAG_USE_WAIT_TOKEN;
  wait.wait_token = iree_async_notification_begin_observe(notification);
  IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &wait.base));
  iree_async_notification_end_observe(notification);
  iree_async_proactor_wake(proactor_);
  PollOneProgressEvent();
  EXPECT_EQ(tracker.call_count, 0);

  iree_atomic_fetch_add(&epoch, 1, iree_memory_order_release);
  iree_async_event_native_set(&native_);
  PollUntilCondition([&] { return tracker.call_count == 1; });
  IREE_EXPECT_OK(tracker.ConsumeStatus());
  iree_async_notification_release(notification);
  iree_async_proactor_release(proactor_);
  proactor_ = nullptr;

  // A late lease-return thread owns only the native event and shared epoch.
  // Neither signaling nor eventual destruction can reach a former proactor.
  std::thread signaler([&] {
    iree_atomic_fetch_add(&epoch, 1, iree_memory_order_release);
    iree_async_event_native_set(&native_);
  });
  signaler.join();
  ExpectReady();
}

CTS_REGISTER_TEST_SUITE(NativeEventTest);

#endif  // native handles available

}  // namespace iree::async::cts
