// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <sys/eventfd.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <vector>

#include "iree/async/operations/message.h"
#include "iree/async/operations/scheduling.h"
#include "iree/async/platform/io_uring/api.h"
#include "iree/async/relay.h"
#include "iree/async/util/proactor_thread.h"
#include "iree/base/threading/notification.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace {

struct EventCallbackState {
  // Descriptor consumed by the event source callback.
  int event_fd = -1;

  // Event bits delivered by the proactor.
  std::atomic<iree_async_poll_events_t> events{IREE_ASYNC_POLL_EVENT_NONE};

  // Set after the callback consumes the eventfd value.
  std::atomic<bool> read_succeeded{false};

  // Set after all callback results have been published.
  std::atomic<bool> invoked{false};

  // Wakes the test thread after the callback publishes its results.
  iree_notification_t notification;
};

static bool EventCallbackInvoked(void* user_data) {
  auto* state = static_cast<EventCallbackState*>(user_data);
  return state->invoked.load(std::memory_order_acquire);
}

struct MessageCompletionState {
  // Set when the source proactor completes the message operation.
  bool completed = false;

  // Terminal status code reported by the source proactor.
  iree_status_code_t status_code = IREE_STATUS_OK;
};

struct MessageReceiverState {
  // Payload delivered to the target.
  uint64_t value = 0;

  // Set after the callback publishes |value|.
  std::atomic<bool> received{false};

  // Wakes the test thread after the target dispatches the callback.
  iree_notification_t notification;
};

static bool MessageReceived(void* user_data) {
  auto* state = static_cast<MessageReceiverState*>(user_data);
  return state->received.load(std::memory_order_acquire);
}

TEST(IoUringCrossThreadTest,
     EventSourcesExceedSubmissionQueueBeforePollOwnerStarts) {
  constexpr iree_host_size_t kEventSourceCount = 32;
  std::vector<int> event_fds;
  event_fds.reserve(kEventSourceCount);
  for (iree_host_size_t i = 0; i < kEventSourceCount; ++i) {
    int event_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (event_fd < 0) {
      for (int open_fd : event_fds) close(open_fd);
      FAIL() << "eventfd creation failed: " << errno;
    }
    event_fds.push_back(event_fd);
  }

  iree_async_proactor_options_t options = iree_async_proactor_options_default();
  options.max_concurrent_operations = 8;
  options.threading_mode = IREE_ASYNC_PROACTOR_THREADING_CROSS_THREAD;
  iree_async_proactor_t* proactor = nullptr;
  iree_status_t status = iree_async_proactor_create_io_uring(
      options, iree_allocator_system(), &proactor);
  if (iree_status_is_unavailable(status)) {
    iree_status_free(status);
    for (int event_fd : event_fds) close(event_fd);
    GTEST_SKIP() << "io_uring is unavailable";
  }
  if (!iree_status_is_ok(status)) {
    for (int event_fd : event_fds) close(event_fd);
    IREE_ASSERT_OK(status);
  }

  EventCallbackState callback_state;
  callback_state.event_fd = event_fds.back();
  iree_notification_initialize(&callback_state.notification);
  iree_async_event_source_callback_t active_callback = {
      +[](void* user_data, iree_async_event_source_t* source,
          iree_async_poll_events_t events) {
        auto* state = static_cast<EventCallbackState*>(user_data);
        (void)source;
        uint64_t value = 0;
        ssize_t read_length = 0;
        do {
          read_length = read(state->event_fd, &value, sizeof(value));
        } while (read_length < 0 && errno == EINTR);
        state->events.store(events, std::memory_order_relaxed);
        state->read_succeeded.store(
            read_length == static_cast<ssize_t>(sizeof(value)),
            std::memory_order_relaxed);
        state->invoked.store(true, std::memory_order_release);
        iree_notification_post(&state->notification, IREE_ALL_WAITERS);
      },
      &callback_state,
  };
  iree_async_event_source_callback_t idle_callback = {
      +[](void* user_data, iree_async_event_source_t* source,
          iree_async_poll_events_t events) {
        (void)user_data;
        (void)source;
        (void)events;
      },
      nullptr,
  };
  for (iree_host_size_t i = 0; i < event_fds.size(); ++i) {
    iree_async_event_source_t* event_source = nullptr;
    status = iree_async_proactor_register_event_source(
        proactor, iree_async_primitive_from_fd(event_fds[i]),
        i + 1 == event_fds.size() ? active_callback : idle_callback,
        &event_source);
    if (!iree_status_is_ok(status)) {
      iree_notification_deinitialize(&callback_state.notification);
      iree_async_proactor_release(proactor);
      for (int event_fd : event_fds) close(event_fd);
      IREE_ASSERT_OK(status);
    }
    ASSERT_NE(event_source, nullptr);
  }

  uint64_t signal_value = 1;
  ASSERT_EQ(write(event_fds.back(), &signal_value, sizeof(signal_value)),
            sizeof(signal_value));

  iree_async_proactor_thread_t* proactor_thread = nullptr;
  status = iree_async_proactor_thread_create(
      proactor, iree_async_proactor_thread_options_default(),
      iree_allocator_system(), &proactor_thread);
  if (!iree_status_is_ok(status)) {
    iree_notification_deinitialize(&callback_state.notification);
    iree_async_proactor_release(proactor);
    for (int event_fd : event_fds) close(event_fd);
    IREE_ASSERT_OK(status);
  }

  EXPECT_TRUE(iree_notification_await(&callback_state.notification,
                                      EventCallbackInvoked, &callback_state,
                                      iree_infinite_timeout()));
  EXPECT_TRUE(callback_state.read_succeeded.load(std::memory_order_relaxed));
  EXPECT_TRUE(callback_state.events.load(std::memory_order_relaxed) &
              IREE_ASYNC_POLL_EVENT_IN);

  iree_async_proactor_thread_request_stop(proactor_thread);
  IREE_ASSERT_OK(
      iree_async_proactor_thread_join(proactor_thread, IREE_DURATION_INFINITE));
  IREE_EXPECT_OK(iree_async_proactor_thread_consume_status(proactor_thread));
  iree_async_proactor_thread_release(proactor_thread);
  iree_async_proactor_release(proactor);
  iree_notification_deinitialize(&callback_state.notification);
  for (int event_fd : event_fds) close(event_fd);
}

TEST(IoUringCrossThreadTest, MessageQueuedBeforePollOwnerStarts) {
  iree_async_proactor_t* source = nullptr;
  iree_status_t status = iree_async_proactor_create_io_uring(
      iree_async_proactor_options_default(), iree_allocator_system(), &source);
  if (iree_status_is_unavailable(status)) {
    iree_status_free(status);
    GTEST_SKIP() << "io_uring is unavailable";
  }
  IREE_ASSERT_OK(status);

  iree_async_proactor_options_t target_options =
      iree_async_proactor_options_default();
  target_options.threading_mode = IREE_ASYNC_PROACTOR_THREADING_CROSS_THREAD;
  iree_async_proactor_t* target = nullptr;
  status = iree_async_proactor_create_io_uring(
      target_options, iree_allocator_system(), &target);
  if (iree_status_is_unavailable(status)) {
    iree_status_free(status);
    iree_async_proactor_release(source);
    GTEST_SKIP() << "cross-thread io_uring is unavailable";
  }
  if (!iree_status_is_ok(status)) {
    iree_async_proactor_release(source);
    IREE_ASSERT_OK(status);
  }

  if (!iree_any_bit_set(iree_async_proactor_query_capabilities(source),
                        IREE_ASYNC_PROACTOR_CAPABILITY_PROACTOR_MESSAGING)) {
    iree_async_proactor_release(target);
    iree_async_proactor_release(source);
    GTEST_SKIP() << "MSG_RING is unavailable";
  }

  MessageReceiverState receiver;
  iree_notification_initialize(&receiver.notification);
  iree_async_proactor_set_message_callback(
      target,
      iree_async_proactor_message_callback_t{
          +[](iree_async_proactor_t* proactor, uint64_t message_data,
              void* user_data) {
            auto* receiver = static_cast<MessageReceiverState*>(user_data);
            (void)proactor;
            receiver->value = message_data;
            receiver->received.store(true, std::memory_order_release);
            iree_notification_post(&receiver->notification, IREE_ALL_WAITERS);
          },
          &receiver,
      });

  constexpr uint64_t kMessageValue = 0x123456789ABCDEF0ull;
  MessageCompletionState completion;
  iree_async_message_operation_t message = {};
  message.base.type = IREE_ASYNC_OPERATION_TYPE_MESSAGE;
  message.base.completion_fn =
      +[](void* user_data, iree_async_operation_t* operation,
          iree_status_t status, iree_async_completion_flags_t flags) {
        auto* completion = static_cast<MessageCompletionState*>(user_data);
        (void)operation;
        (void)flags;
        completion->status_code = iree_status_code(status);
        completion->completed = true;
        iree_status_free(status);
      };
  message.base.user_data = &completion;
  message.target = target;
  message.message_data = kMessageValue;

  IREE_ASSERT_OK(iree_async_proactor_submit_one(source, &message.base));
  while (!completion.completed) {
    IREE_ASSERT_OK(iree_async_proactor_poll(source, iree_infinite_timeout(),
                                            /*out_completed_count=*/nullptr));
  }
  if (completion.status_code != IREE_STATUS_OK) {
    iree_notification_deinitialize(&receiver.notification);
    iree_async_proactor_release(target);
    iree_async_proactor_release(source);
    EXPECT_EQ(completion.status_code, IREE_STATUS_OK);
    return;
  }
  EXPECT_FALSE(receiver.received.load(std::memory_order_acquire));

  iree_async_proactor_thread_t* target_thread = nullptr;
  status = iree_async_proactor_thread_create(
      target, iree_async_proactor_thread_options_default(),
      iree_allocator_system(), &target_thread);
  if (!iree_status_is_ok(status)) {
    iree_notification_deinitialize(&receiver.notification);
    iree_async_proactor_release(target);
    iree_async_proactor_release(source);
    IREE_ASSERT_OK(status);
  }
  EXPECT_TRUE(iree_notification_await(&receiver.notification, MessageReceived,
                                      &receiver, iree_infinite_timeout()));
  EXPECT_EQ(receiver.value, kMessageValue);

  iree_async_proactor_thread_request_stop(target_thread);
  IREE_ASSERT_OK(
      iree_async_proactor_thread_join(target_thread, IREE_DURATION_INFINITE));
  IREE_EXPECT_OK(iree_async_proactor_thread_consume_status(target_thread));
  iree_async_proactor_thread_release(target_thread);
  iree_notification_deinitialize(&receiver.notification);
  iree_async_proactor_release(target);
  iree_async_proactor_release(source);
}

class ProactorLifetimeTest : public ::testing::Test {
 protected:
  struct CountingAllocatorState {
    // Allocator receiving forwarded commands.
    iree_allocator_t delegate = iree_allocator_system();

    // Most recent allocation returned by the delegate.
    void* last_allocation = nullptr;

    // Allocation whose release the current test observes.
    void* watched_allocation = nullptr;

    // Set when the watched allocation is passed to FREE.
    bool watched_allocation_freed = false;
  };

  struct NopCompletionState {
    // Number of NOP callbacks dispatched by the poll loop.
    iree_host_size_t count = 0;
  };

  static iree_status_t CountingAllocatorCtl(void* self,
                                            iree_allocator_command_t command,
                                            const void* params,
                                            void** inout_ptr) {
    auto* state = static_cast<CountingAllocatorState*>(self);
    void* old_ptr = *inout_ptr;
    if (command == IREE_ALLOCATOR_COMMAND_FREE &&
        old_ptr == state->watched_allocation) {
      state->watched_allocation_freed = true;
    }
    iree_status_t status =
        state->delegate.ctl(state->delegate.self, command, params, inout_ptr);
    if (iree_status_is_ok(status) &&
        (command == IREE_ALLOCATOR_COMMAND_MALLOC ||
         command == IREE_ALLOCATOR_COMMAND_CALLOC ||
         (command == IREE_ALLOCATOR_COMMAND_REALLOC && !old_ptr))) {
      state->last_allocation = *inout_ptr;
    }
    return status;
  }

  void SetUp() override {
    iree_async_proactor_options_t options =
        iree_async_proactor_options_default();
    options.max_concurrent_operations = 8;
    iree_allocator_t allocator = {
        &allocator_state_,
        CountingAllocatorCtl,
    };
    iree_status_t create_status =
        iree_async_proactor_create_io_uring(options, allocator, &proactor_);
    if (iree_status_is_unavailable(create_status)) {
      IREE_EXPECT_STATUS_IS(IREE_STATUS_UNAVAILABLE, create_status);
      GTEST_SKIP() << "io_uring is unavailable";
    }
    IREE_ASSERT_OK(create_status);

    // Bind the poll owner and arm the persistent wake operation before tests
    // fill the submission queue.
    IREE_EXPECT_STATUS_IS(
        IREE_STATUS_DEADLINE_EXCEEDED,
        iree_async_proactor_poll(proactor_, iree_immediate_timeout(), nullptr));
  }

  void TearDown() override {
    iree_async_proactor_release(proactor_);
    proactor_ = nullptr;
  }

  void FillSubmissionQueue() {
    // Submit without polling until get_sqe() reports the queue full. These
    // SQEs remain unpublished, guaranteeing cancellation cannot allocate an
    // SQE on its first attempt.
    nop_operations_.resize(1024);
    for (auto& nop : nop_operations_) {
      memset(&nop, 0, sizeof(nop));
      nop.base.type = IREE_ASYNC_OPERATION_TYPE_NOP;
      nop.base.completion_fn =
          +[](void* user_data, iree_async_operation_t* operation,
              iree_status_t status, iree_async_completion_flags_t flags) {
            auto* state = static_cast<NopCompletionState*>(user_data);
            (void)operation;
            (void)flags;
            IREE_EXPECT_OK(status);
            ++state->count;
          };
      nop.base.user_data = &nop_state_;
      iree_status_t submit_status =
          iree_async_proactor_submit_one(proactor_, &nop.base);
      if (iree_status_is_resource_exhausted(submit_status)) {
        IREE_EXPECT_STATUS_IS(IREE_STATUS_RESOURCE_EXHAUSTED, submit_status);
        break;
      }
      IREE_ASSERT_OK(submit_status);
      ++submitted_count_;
    }
    ASSERT_GT(submitted_count_, 0u);
    ASSERT_LT(submitted_count_, nop_operations_.size());
  }

  void PollOnce() {
    IREE_ASSERT_OK(iree_async_proactor_poll(proactor_, iree_infinite_timeout(),
                                            /*out_completed_count=*/nullptr));
  }

  iree_async_proactor_t* proactor_ = nullptr;
  CountingAllocatorState allocator_state_;
  std::vector<iree_async_nop_operation_t> nop_operations_;
  NopCompletionState nop_state_;
  iree_host_size_t submitted_count_ = 0;
};

TEST_F(ProactorLifetimeTest,
       EventSourceUnregistrationRetriesAfterSubmissionQueuePressure) {
  int event_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  ASSERT_GE(event_fd, 0);

  struct EventState {
    // Number of event source callbacks dispatched by the poll loop.
    iree_host_size_t count = 0;
  } event_state;
  iree_async_event_source_callback_t event_callback = {
      +[](void* user_data, iree_async_event_source_t* source,
          iree_async_poll_events_t events) {
        auto* state = static_cast<EventState*>(user_data);
        (void)source;
        (void)events;
        ++state->count;
      },
      &event_state,
  };
  iree_async_event_source_t* event_source = nullptr;
  IREE_ASSERT_OK(iree_async_proactor_register_event_source(
      proactor_, iree_async_primitive_from_fd(event_fd), event_callback,
      &event_source));
  ASSERT_EQ(static_cast<void*>(event_source), allocator_state_.last_allocation);
  allocator_state_.watched_allocation = event_source;

  // Ensure cancellation targets a live multishot poll.
  PollOnce();

  FillSubmissionQueue();
  iree_async_proactor_unregister_event_source(proactor_, event_source);

  while (!allocator_state_.watched_allocation_freed ||
         nop_state_.count < submitted_count_) {
    PollOnce();
  }

  EXPECT_EQ(event_state.count, 0u);
  EXPECT_EQ(nop_state_.count, submitted_count_);
  close(event_fd);
}

TEST_F(ProactorLifetimeTest,
       RelayUnregistrationRetriesAfterSubmissionQueuePressure) {
  iree_async_notification_t* source_notification = nullptr;
  IREE_ASSERT_OK(iree_async_notification_create(
      proactor_, IREE_ASYNC_NOTIFICATION_FLAG_NONE, &source_notification));
  iree_async_notification_t* sink_notification = nullptr;
  IREE_ASSERT_OK(iree_async_notification_create(
      proactor_, IREE_ASYNC_NOTIFICATION_FLAG_NONE, &sink_notification));

  iree_async_relay_t* relay = nullptr;
  IREE_ASSERT_OK(iree_async_proactor_register_relay(
      proactor_, iree_async_relay_source_from_notification(source_notification),
      iree_async_relay_sink_signal_notification(sink_notification, 1),
      IREE_ASYNC_RELAY_FLAG_PERSISTENT, iree_async_relay_error_callback_none(),
      &relay));

  // Ensure cancellation targets a live source wait.
  PollOnce();

  FillSubmissionQueue();

  struct UnregistrationState {
    // Set after the relay has no remaining backend references.
    bool completed = false;
  } unregistration_state;
  iree_async_relay_unregistered_callback_t unregistered_callback = {
      +[](void* user_data) {
        static_cast<UnregistrationState*>(user_data)->completed = true;
      },
      &unregistration_state,
  };
  iree_async_proactor_unregister_relay(proactor_, relay, unregistered_callback);
  EXPECT_FALSE(unregistration_state.completed);

  while (!unregistration_state.completed ||
         nop_state_.count < submitted_count_) {
    PollOnce();
  }

  EXPECT_EQ(nop_state_.count, submitted_count_);
  iree_async_notification_release(source_notification);
  iree_async_notification_release(sink_notification);
}

TEST_F(ProactorLifetimeTest,
       PersistentRelayFaultCancelsSourceBeforeUnregistration) {
  int source_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  ASSERT_GE(source_fd, 0);
  int sink_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  ASSERT_GE(sink_fd, 0);

  struct FaultState {
    // Set when the relay reports its terminal sink failure.
    bool faulted = false;
    // Status code reported for the failed sink write.
    iree_status_code_t status_code = IREE_STATUS_OK;
  } fault_state;
  iree_async_relay_error_callback_t error_callback = {
      +[](void* user_data, iree_async_relay_t* relay, iree_status_t status) {
        auto* state = static_cast<FaultState*>(user_data);
        (void)relay;
        state->status_code = iree_status_code(status);
        state->faulted = true;
        iree_status_free(status);
      },
      &fault_state,
  };

  iree_async_relay_t* relay = nullptr;
  IREE_ASSERT_OK(iree_async_proactor_register_relay(
      proactor_,
      iree_async_relay_source_from_primitive(
          iree_async_primitive_from_fd(source_fd)),
      iree_async_relay_sink_signal_primitive(
          iree_async_primitive_from_fd(sink_fd), 1),
      IREE_ASYNC_RELAY_FLAG_PERSISTENT, error_callback, &relay));

  // Closing the sink makes the next source transfer fault. The source remains
  // a live multishot poll until the backend receives its terminal cancel CQE.
  close(sink_fd);
  uint64_t signal_value = 1;
  ASSERT_EQ(write(source_fd, &signal_value, sizeof(signal_value)),
            sizeof(signal_value));
  while (!fault_state.faulted) PollOnce();
  EXPECT_NE(fault_state.status_code, IREE_STATUS_OK);

  struct UnregistrationState {
    // Set after the relay has no remaining backend references.
    bool completed = false;
  } unregistration_state;
  iree_async_relay_unregistered_callback_t unregistered_callback = {
      +[](void* user_data) {
        static_cast<UnregistrationState*>(user_data)->completed = true;
      },
      &unregistration_state,
  };
  iree_async_proactor_unregister_relay(proactor_, relay, unregistered_callback);
  while (!unregistration_state.completed) PollOnce();

  close(source_fd);
}

TEST_F(ProactorLifetimeTest,
       ProactorDestructionJoinsPendingRelayUnregistration) {
  int source_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  ASSERT_GE(source_fd, 0);
  int sink_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  ASSERT_GE(sink_fd, 0);

  iree_async_relay_t* relay = nullptr;
  IREE_ASSERT_OK(iree_async_proactor_register_relay(
      proactor_,
      iree_async_relay_source_from_primitive(
          iree_async_primitive_from_fd(source_fd)),
      iree_async_relay_sink_signal_primitive(
          iree_async_primitive_from_fd(sink_fd), 1),
      IREE_ASYNC_RELAY_FLAG_PERSISTENT, iree_async_relay_error_callback_none(),
      &relay));
  ASSERT_EQ(static_cast<void*>(relay), allocator_state_.last_allocation);
  allocator_state_.watched_allocation = relay;

  // Submit source monitoring, then force cancellation to remain pending behind
  // a full SQ. Destruction must terminate the live kernel operation and run the
  // caller's terminal callback before returning.
  PollOnce();
  FillSubmissionQueue();

  struct UnregistrationState {
    // Set after destruction has retired all backend references.
    bool completed = false;
  } unregistration_state;
  iree_async_relay_unregistered_callback_t unregistered_callback = {
      +[](void* user_data) {
        static_cast<UnregistrationState*>(user_data)->completed = true;
      },
      &unregistration_state,
  };
  iree_async_proactor_unregister_relay(proactor_, relay, unregistered_callback);
  EXPECT_FALSE(unregistration_state.completed);

  iree_async_proactor_release(proactor_);
  proactor_ = nullptr;

  EXPECT_TRUE(allocator_state_.watched_allocation_freed);
  EXPECT_TRUE(unregistration_state.completed);
  close(source_fd);
  close(sink_fd);
}

}  // namespace
