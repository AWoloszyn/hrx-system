// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/async/cts/util/registry.h"
#include "iree/async/cts/util/test_base.h"
#include "iree/async/operations/scheduling.h"

#if defined(IREE_PLATFORM_WINDOWS)
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif  // IREE_PLATFORM_WINDOWS

namespace iree::async::cts {
namespace {

static iree_async_handle_poll_operation_t MakePoll(
    iree_async_primitive_t primitive, iree_async_poll_events_t events,
    CompletionTracker* tracker) {
  iree_async_handle_poll_operation_t operation = {};
  iree_async_operation_initialize(&operation.base,
                                  IREE_ASYNC_OPERATION_TYPE_HANDLE_POLL, 0,
                                  CompletionTracker::Callback, tracker);
  operation.primitive = primitive;
  operation.events = events;
  return operation;
}

class HandlePollTest : public CtsTestBase<> {
 protected:
  void SetUp() override {
    CtsTestBase<>::SetUp();
    if (HasFatalFailure() || IsSkipped()) {
      return;
    }
#if defined(IREE_PLATFORM_WINDOWS)
    event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ASSERT_NE(event_, nullptr);
    primitive_ = iree_async_primitive_from_win32_handle((uintptr_t)event_);
#else
    ASSERT_EQ(pipe(descriptors_), 0);
    for (int descriptor : descriptors_) {
      int flags = fcntl(descriptor, F_GETFL);
      ASSERT_GE(flags, 0);
      ASSERT_EQ(fcntl(descriptor, F_SETFL, flags | O_NONBLOCK), 0);
      ASSERT_EQ(fcntl(descriptor, F_SETFD, FD_CLOEXEC), 0);
    }
    primitive_ = iree_async_primitive_from_fd(descriptors_[0]);
#endif  // IREE_PLATFORM_WINDOWS
  }

  void TearDown() override {
#if defined(IREE_PLATFORM_WINDOWS)
    if (event_) {
      EXPECT_TRUE(CloseHandle(event_));
    }
#else
    for (int descriptor : descriptors_) {
      if (descriptor >= 0) {
        EXPECT_EQ(close(descriptor), 0);
      }
    }
#endif  // IREE_PLATFORM_WINDOWS
    CtsTestBase<>::TearDown();
  }

  void Signal() {
#if defined(IREE_PLATFORM_WINDOWS)
    ASSERT_TRUE(SetEvent(event_));
#else
    const char value = 'x';
    ASSERT_EQ(write(descriptors_[1], &value, 1), 1);
#endif  // IREE_PLATFORM_WINDOWS
  }

  void Reset() {
#if defined(IREE_PLATFORM_WINDOWS)
    ASSERT_TRUE(ResetEvent(event_));
#else
    char value = 0;
    ASSERT_EQ(read(descriptors_[0], &value, 1), 1);
    ASSERT_EQ(value, 'x');
#endif  // IREE_PLATFORM_WINDOWS
  }

  void PollAvailable() {
    iree_host_size_t completed = 0;
    iree_status_t status = iree_async_proactor_poll(
        proactor_, iree_immediate_timeout(), &completed);
    if (iree_status_is_deadline_exceeded(status)) {
      iree_status_free(status);
    } else {
      IREE_ASSERT_OK(status);
    }
  }

  // Fixture-owned primitive, borrowed by each submitted poll.
  iree_async_primitive_t primitive_ = iree_async_primitive_none();
#if defined(IREE_PLATFORM_WINDOWS)
  // Manual-reset event so signaling persists across independent waits.
  HANDLE event_ = nullptr;
#else
  // Nonblocking read and write ends, each owned until fixture teardown.
  int descriptors_[2] = {-1, -1};
#endif  // IREE_PLATFORM_WINDOWS
};

TEST_P(HandlePollTest, SignalBeforeSubmission) {
  Signal();
  CompletionTracker tracker;
  auto operation = MakePoll(primitive_, IREE_ASYNC_POLL_EVENT_IN, &tracker);
  IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &operation.base));
  PollUntilCondition([&] { return tracker.call_count == 1; });
  IREE_EXPECT_OK(tracker.ConsumeStatus());
  EXPECT_EQ(operation.result_events, IREE_ASYNC_POLL_EVENT_IN);
}

TEST_P(HandlePollTest, WaitsWithoutConsumingOrClosingPrimitive) {
  CompletionTracker tracker;
  auto operation = MakePoll(primitive_, IREE_ASYNC_POLL_EVENT_IN, &tracker);
  IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &operation.base));
  PollAvailable();
  EXPECT_EQ(tracker.call_count, 0);
  Signal();
  PollUntilCondition([&] { return tracker.call_count == 1; });
  IREE_EXPECT_OK(tracker.ConsumeStatus());
  EXPECT_EQ(operation.result_events, IREE_ASYNC_POLL_EVENT_IN);

  // No reset or read between waits: readiness and caller ownership survive.
  tracker.Reset();
  IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &operation.base));
  PollUntilCondition([&] { return tracker.call_count == 1; });
  IREE_EXPECT_OK(tracker.ConsumeStatus());
  EXPECT_EQ(operation.result_events, IREE_ASYNC_POLL_EVENT_IN);
  Reset();
}

TEST_P(HandlePollTest, CancellationBeforePollingClearsResults) {
  CompletionTracker tracker;
  auto operation = MakePoll(primitive_, IREE_ASYNC_POLL_EVENT_IN, &tracker);
  operation.result_events = IREE_ASYNC_POLL_EVENT_OUT;
  IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &operation.base));
  IREE_ASSERT_OK(iree_async_proactor_cancel(proactor_, &operation.base));
  PollUntilCondition([&] { return tracker.call_count == 1; });
  IREE_EXPECT_STATUS_IS(IREE_STATUS_CANCELLED, tracker.ConsumeStatus());
  EXPECT_EQ(operation.result_events, IREE_ASYNC_POLL_EVENT_NONE);
  Signal();
  Reset();
}

TEST_P(HandlePollTest, ReusedPollClearsResultsOnCancellation) {
  Signal();
  CompletionTracker tracker;
  auto operation = MakePoll(primitive_, IREE_ASYNC_POLL_EVENT_IN, &tracker);
  IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &operation.base));
  PollUntilCondition([&] { return tracker.call_count == 1; });
  IREE_EXPECT_OK(tracker.ConsumeStatus());
  ASSERT_EQ(operation.result_events, IREE_ASYNC_POLL_EVENT_IN);
  Reset();

  tracker.Reset();
  IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &operation.base));
  PollAvailable();
  EXPECT_EQ(tracker.call_count, 0);
  IREE_ASSERT_OK(iree_async_proactor_cancel(proactor_, &operation.base));
  PollUntilCondition([&] { return tracker.call_count == 1; });
  IREE_EXPECT_STATUS_IS(IREE_STATUS_CANCELLED, tracker.ConsumeStatus());
  EXPECT_EQ(operation.result_events, IREE_ASYNC_POLL_EVENT_NONE);
}

TEST_P(HandlePollTest, CancelledLinkedPredecessorClearsPollResults) {
  CompletionTracker predecessor_tracker;
  CompletionTracker poll_tracker;
  auto predecessor =
      MakePoll(primitive_, IREE_ASYNC_POLL_EVENT_IN, &predecessor_tracker);
  auto successor =
      MakePoll(primitive_, IREE_ASYNC_POLL_EVENT_IN, &poll_tracker);
  predecessor.base.flags = IREE_ASYNC_OPERATION_FLAG_LINKED;
  successor.result_events = IREE_ASYNC_POLL_EVENT_OUT;
  iree_async_operation_t* operations[] = {&predecessor.base, &successor.base};
  IREE_ASSERT_OK(iree_async_proactor_submit(
      proactor_, iree_async_operation_list_make(operations, 2)));
  IREE_ASSERT_OK(iree_async_proactor_cancel(proactor_, &predecessor.base));
  PollUntilCondition([&] {
    return predecessor_tracker.call_count == 1 && poll_tracker.call_count == 1;
  });
  IREE_EXPECT_STATUS_IS(IREE_STATUS_CANCELLED,
                        predecessor_tracker.ConsumeStatus());
  IREE_EXPECT_STATUS_IS(IREE_STATUS_CANCELLED, poll_tracker.ConsumeStatus());
  EXPECT_EQ(successor.result_events, IREE_ASYNC_POLL_EVENT_NONE);
}

TEST_P(HandlePollTest, RejectsInvalidInterestsWithoutCallbacks) {
  const iree_async_poll_events_t invalid_events[] = {
      IREE_ASYNC_POLL_EVENT_NONE,
      IREE_ASYNC_POLL_EVENT_ERR,
      IREE_ASYNC_POLL_EVENT_HUP,
      IREE_ASYNC_POLL_EVENT_IN | IREE_ASYNC_POLL_EVENT_ERR,
      1u << 31,
  };
  for (iree_async_poll_events_t events : invalid_events) {
    CompletionTracker tracker;
    auto operation = MakePoll(primitive_, events, &tracker);
    operation.result_events = IREE_ASYNC_POLL_EVENT_OUT;
    IREE_EXPECT_STATUS_IS(
        IREE_STATUS_INVALID_ARGUMENT,
        iree_async_proactor_submit_one(proactor_, &operation.base));
    EXPECT_EQ(operation.result_events, IREE_ASYNC_POLL_EVENT_OUT);
    EXPECT_EQ(tracker.call_count, 0);
  }
}

TEST_P(HandlePollTest, InvalidBatchLeavesValidPollCallerOwned) {
  CompletionTracker valid_tracker;
  CompletionTracker invalid_tracker;
  auto valid = MakePoll(primitive_, IREE_ASYNC_POLL_EVENT_IN, &valid_tracker);
  auto invalid =
      MakePoll(primitive_, IREE_ASYNC_POLL_EVENT_NONE, &invalid_tracker);
  valid.result_events = IREE_ASYNC_POLL_EVENT_OUT;
  iree_async_operation_t* operations[] = {&valid.base, &invalid.base};
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      iree_async_proactor_submit(
          proactor_, iree_async_operation_list_make(operations, 2)));
  EXPECT_EQ(valid.result_events, IREE_ASYNC_POLL_EVENT_OUT);
  EXPECT_EQ(valid_tracker.call_count, 0);
  EXPECT_EQ(invalid_tracker.call_count, 0);
  Signal();
  IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &valid.base));
  PollUntilCondition([&] { return valid_tracker.call_count == 1; });
  IREE_EXPECT_OK(valid_tracker.ConsumeStatus());
  EXPECT_EQ(valid.result_events, IREE_ASYNC_POLL_EVENT_IN);
}

TEST_P(HandlePollTest, FinalCallbackMayDeleteOperation) {
  CompletionTracker tracker;
  auto* operation = new iree_async_handle_poll_operation_t(
      MakePoll(primitive_, IREE_ASYNC_POLL_EVENT_IN, &tracker));
  operation->base.completion_fn =
      +[](void* user_data, iree_async_operation_t* base, iree_status_t status,
          iree_async_completion_flags_t flags) {
        auto* poll =
            reinterpret_cast<iree_async_handle_poll_operation_t*>(base);
        EXPECT_EQ(poll->result_events, IREE_ASYNC_POLL_EVENT_IN);
        CompletionTracker::Callback(user_data, base, status, flags);
        delete poll;
      };
  IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &operation->base));
  Signal();
  PollUntilCondition([&] { return tracker.call_count == 1; });
  IREE_EXPECT_OK(tracker.ConsumeStatus());
  Reset();
}

#if defined(IREE_PLATFORM_WINDOWS)

TEST_P(HandlePollTest, RejectsWritableWaitOnWindowsHandle) {
  const iree_async_poll_events_t interests[] = {
      IREE_ASYNC_POLL_EVENT_OUT,
      IREE_ASYNC_POLL_EVENT_IN | IREE_ASYNC_POLL_EVENT_OUT,
  };
  for (iree_async_poll_events_t events : interests) {
    CompletionTracker tracker;
    auto operation = MakePoll(primitive_, events, &tracker);
    IREE_EXPECT_STATUS_IS(
        IREE_STATUS_UNAVAILABLE,
        iree_async_proactor_submit_one(proactor_, &operation.base));
    EXPECT_EQ(tracker.call_count, 0);
  }
}

#else

TEST_P(HandlePollTest, ReportsHangupWithoutAnExplicitHangupInterest) {
  CompletionTracker tracker;
  auto operation = MakePoll(primitive_, IREE_ASYNC_POLL_EVENT_IN, &tracker);
  IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &operation.base));
  ASSERT_EQ(close(descriptors_[1]), 0);
  descriptors_[1] = -1;
  PollUntilCondition([&] { return tracker.call_count == 1; });
  IREE_EXPECT_OK(tracker.ConsumeStatus());
  EXPECT_TRUE(
      iree_any_bit_set(operation.result_events, IREE_ASYNC_POLL_EVENT_HUP));
}

TEST_P(HandlePollTest, ReportsPipeWriteErrorAsReadiness) {
  CompletionTracker tracker;
  auto operation = MakePoll(iree_async_primitive_from_fd(descriptors_[1]),
                            IREE_ASYNC_POLL_EVENT_OUT, &tracker);
  ASSERT_EQ(close(descriptors_[0]), 0);
  descriptors_[0] = -1;
  IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &operation.base));
  PollUntilCondition([&] { return tracker.call_count == 1; });
  IREE_EXPECT_OK(tracker.ConsumeStatus());
  EXPECT_TRUE(
      iree_any_bit_set(operation.result_events,
                       IREE_ASYNC_POLL_EVENT_ERR | IREE_ASYNC_POLL_EVENT_HUP));
}

class HandlePollSocketTest : public HandlePollTest {
 protected:
  void SetUp() override {
    HandlePollTest::SetUp();
    if (HasFatalFailure() || IsSkipped()) {
      return;
    }
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets_), 0);
    for (int descriptor : sockets_) {
      int flags = fcntl(descriptor, F_GETFL);
      ASSERT_GE(flags, 0);
      ASSERT_EQ(fcntl(descriptor, F_SETFL, flags | O_NONBLOCK), 0);
      ASSERT_EQ(fcntl(descriptor, F_SETFD, FD_CLOEXEC), 0);
    }
    primitive_ = iree_async_primitive_from_fd(sockets_[0]);
  }

  void TearDown() override {
    for (int descriptor : sockets_) {
      if (descriptor >= 0) {
        EXPECT_EQ(close(descriptor), 0);
      }
    }
    HandlePollTest::TearDown();
  }

  void FillSendBuffer() {
    int size = 4096;
    ASSERT_EQ(
        setsockopt(sockets_[0], SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)), 0);
    char bytes[4096] = {};
    for (;;) {
      ssize_t written = send(sockets_[0], bytes, sizeof(bytes), 0);
      if (written > 0) {
        continue;
      }
      ASSERT_EQ(written, -1);
      ASSERT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);
      break;
    }
  }

  void DrainSendBuffer() {
    char bytes[4096];
    for (;;) {
      ssize_t received = recv(sockets_[1], bytes, sizeof(bytes), 0);
      if (received > 0) {
        continue;
      }
      ASSERT_EQ(received, -1);
      ASSERT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);
      break;
    }
  }

  // Both caller-owned ends of a nonblocking local stream.
  int sockets_[2] = {-1, -1};
};

TEST_P(HandlePollSocketTest, WritableInterestResumesAfterBackpressure) {
  FillSendBuffer();
  CompletionTracker tracker;
  auto operation = MakePoll(primitive_, IREE_ASYNC_POLL_EVENT_OUT, &tracker);
  IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &operation.base));
  PollAvailable();
  EXPECT_EQ(tracker.call_count, 0);
  DrainSendBuffer();
  PollUntilCondition([&] { return tracker.call_count == 1; });
  IREE_EXPECT_OK(tracker.ConsumeStatus());
  EXPECT_EQ(operation.result_events, IREE_ASYNC_POLL_EVENT_OUT);
  const char byte = 'x';
  EXPECT_EQ(send(sockets_[0], &byte, 1, 0), 1);
}

TEST_P(HandlePollSocketTest, ReadAndWriteWaitsOnSameDescriptorStayIndependent) {
  CompletionTracker reader;
  CompletionTracker writer;
  auto read_operation = MakePoll(primitive_, IREE_ASYNC_POLL_EVENT_IN, &reader);
  auto write_operation =
      MakePoll(primitive_, IREE_ASYNC_POLL_EVENT_OUT, &writer);
  iree_async_operation_t* operations[] = {&read_operation.base,
                                          &write_operation.base};
  IREE_ASSERT_OK(iree_async_proactor_submit(
      proactor_, iree_async_operation_list_make(operations, 2)));
  PollUntilCondition([&] { return writer.call_count == 1; });
  IREE_EXPECT_OK(writer.ConsumeStatus());
  EXPECT_EQ(write_operation.result_events, IREE_ASYNC_POLL_EVENT_OUT);
  EXPECT_EQ(reader.call_count, 0);
  const char byte = 'x';
  ASSERT_EQ(send(sockets_[1], &byte, 1, 0), 1);
  PollUntilCondition([&] { return reader.call_count == 1; });
  IREE_EXPECT_OK(reader.ConsumeStatus());
  EXPECT_EQ(read_operation.result_events, IREE_ASYNC_POLL_EVENT_IN);
}

TEST_P(HandlePollSocketTest, CombinedInterestCompletesForEitherDirection) {
  CompletionTracker tracker;
  auto operation =
      MakePoll(primitive_, IREE_ASYNC_POLL_EVENT_IN | IREE_ASYNC_POLL_EVENT_OUT,
               &tracker);
  IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &operation.base));
  PollUntilCondition([&] { return tracker.call_count == 1; });
  IREE_EXPECT_OK(tracker.ConsumeStatus());
  EXPECT_EQ(operation.result_events, IREE_ASYNC_POLL_EVENT_OUT);

  FillSendBuffer();
  const char byte = 'x';
  ASSERT_EQ(send(sockets_[1], &byte, 1, 0), 1);
  tracker.Reset();
  IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &operation.base));
  PollUntilCondition([&] { return tracker.call_count == 1; });
  IREE_EXPECT_OK(tracker.ConsumeStatus());
  EXPECT_EQ(operation.result_events, IREE_ASYNC_POLL_EVENT_IN);
}

TEST_P(HandlePollSocketTest, CancelsBackpressuredWritableWait) {
  FillSendBuffer();
  CompletionTracker tracker;
  auto operation = MakePoll(primitive_, IREE_ASYNC_POLL_EVENT_OUT, &tracker);
  operation.result_events = IREE_ASYNC_POLL_EVENT_IN;
  IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &operation.base));
  PollAvailable();
  EXPECT_EQ(tracker.call_count, 0);
  IREE_ASSERT_OK(iree_async_proactor_cancel(proactor_, &operation.base));
  PollUntilCondition([&] { return tracker.call_count == 1; });
  IREE_EXPECT_STATUS_IS(IREE_STATUS_CANCELLED, tracker.ConsumeStatus());
  EXPECT_EQ(operation.result_events, IREE_ASYNC_POLL_EVENT_NONE);
}

CTS_REGISTER_TEST_SUITE(HandlePollSocketTest);

#endif  // IREE_PLATFORM_WINDOWS

CTS_REGISTER_TEST_SUITE(HandlePollTest);

}  // namespace
}  // namespace iree::async::cts
