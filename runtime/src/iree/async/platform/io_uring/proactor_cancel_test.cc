// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <sys/eventfd.h>
#include <unistd.h>

#include <atomic>
#include <memory>
#include <thread>

#include "iree/async/event.h"
#include "iree/async/operations/net.h"
#include "iree/async/operations/scheduling.h"
#include "iree/async/platform/io_uring/api.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace {

struct Completion {
  // Number of terminal callbacks delivered.
  int count = 0;
  // Result of the last terminal callback.
  iree_status_code_t code = IREE_STATUS_UNKNOWN;

  static void Record(void* user_data, iree_async_operation_t*,
                     iree_status_t status,
                     iree_async_completion_flags_t flags) {
    auto* self = static_cast<Completion*>(user_data);
    EXPECT_EQ(flags, IREE_ASYNC_COMPLETION_FLAG_NONE);
    ++self->count;
    self->code = iree_status_code(status);
    iree_status_free(status);
  }
};

struct OwnedCompletion {
  // Owner of the request and target-retired notification.
  iree_async_proactor_t* proactor;
  // Caller storage held through the native key-retirement receipt.
  iree_async_cancel_request_t request = {};
  // Terminal target result, independent of request retirement.
  Completion target;
  // Number of receipts observed.
  int receipts = 0;
  // Request phase observed before reporting target retirement.
  iree_async_cancel_request_phase_t phase_at_target =
      IREE_ASYNC_CANCEL_REQUEST_PHASE_IDLE;

  explicit OwnedCompletion(iree_async_proactor_t* proactor)
      : proactor(proactor) {
    iree_async_cancel_request_initialize(
        {[](void* user_data) {
           ++static_cast<OwnedCompletion*>(user_data)->receipts;
         },
         this},
        &request);
  }

  static void Record(void* user_data, iree_async_operation_t* operation,
                     iree_status_t status,
                     iree_async_completion_flags_t flags) {
    auto* self = static_cast<OwnedCompletion*>(user_data);
    self->phase_at_target = self->request.phase;
    Completion::Record(&self->target, operation, status, flags);
    if (!self->receipts) {
      iree_async_proactor_cancel_request_target_retired(self->proactor,
                                                        &self->request);
    }
  }

  bool done() const { return receipts == 1 && target.count == 1; }
};

enum class DispatchPhase { kFirstDrain, kLastDrain };

class IoUringCancelTest : public ::testing::TestWithParam<DispatchPhase> {
 protected:
  void SetUp() override {
    auto options = iree_async_proactor_options_default();
    options.max_concurrent_operations = IREE_ARRAYSIZE(fillers_);
    options.allowed_capabilities &=
        ~IREE_ASYNC_PROACTOR_CAPABILITY_PROACTOR_MESSAGING;
    iree_status_t status = iree_async_proactor_create_io_uring(
        options, iree_allocator_t{this, ControlAllocation}, &proactor_);
    if (iree_status_is_unavailable(status)) {
      iree_status_free(status);
      GTEST_SKIP() << "io_uring is unavailable";
    }
    IREE_ASSERT_OK(status);
    event_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    ASSERT_GE(event_fd_, 0);
  }

  void TearDown() override {
    if (fillers_submitted_) {
      PollUntil([&] { return filler_completion_.count == 8; });
      EXPECT_EQ(filler_completion_.code, IREE_STATUS_OK);
    }
    EXPECT_EQ(rejected_allocations_.load(), 0u);
    if (event_fd_ >= 0) {
      EXPECT_EQ(close(event_fd_), 0);
    }
    iree_async_proactor_release(proactor_);
  }

  static iree_status_t ControlAllocation(void* user_data,
                                         iree_allocator_command_t command,
                                         const void* params, void** inout_ptr) {
    auto* self = static_cast<IoUringCancelTest*>(user_data);
    if (!self->allocations_enabled_ && command != IREE_ALLOCATOR_COMMAND_FREE) {
      ++self->rejected_allocations_;
      return iree_status_from_code(IREE_STATUS_RESOURCE_EXHAUSTED);
    }
    auto allocator = iree_allocator_system();
    return allocator.ctl(allocator.self, command, params, inout_ptr);
  }

  template <typename Predicate>
  void PollUntil(Predicate complete) {
    while (!complete()) {
      IREE_ASSERT_OK(iree_async_proactor_poll(
          proactor_, iree_infinite_timeout(), nullptr));
    }
  }

  template <typename Callback>
  void Dispatch(Callback callback) {
    struct State {
      // Owner-thread action to perform.
      Callback callback;
      // Set only after the action returns.
      bool completed = false;
    } state{callback};
    iree_async_operation_t operation = {};
    iree_async_operation_initialize(
        &operation, IREE_ASYNC_OPERATION_TYPE_NOP,
        IREE_ASYNC_OPERATION_FLAG_NONE,
        [](void* user_data, iree_async_operation_t*, iree_status_t status,
           iree_async_completion_flags_t flags) {
          IREE_EXPECT_OK(status);
          EXPECT_EQ(flags, IREE_ASYNC_COMPLETION_FLAG_NONE);
          auto* state = static_cast<State*>(user_data);
          state->callback();
          state->completed = true;
        },
        &state);
    if (GetParam() == DispatchPhase::kFirstDrain) {
      IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &operation));
    } else {
      // Fallback messages dispatch after the CQE drain. Their NOP is consumed
      // by the final software drain in the same poll turn.
      iree_async_proactor_set_message_callback(
          proactor_,
          iree_async_proactor_message_callback_t{
              [](iree_async_proactor_t* proactor, uint64_t, void* user_data) {
                IREE_ASSERT_OK(iree_async_proactor_submit_one(
                    proactor, static_cast<iree_async_operation_t*>(user_data)));
              },
              &operation});
      IREE_ASSERT_OK(iree_async_proactor_send_message(proactor_, 0));
    }
    PollUntil([&] { return state.completed; });
    iree_async_proactor_set_message_callback(proactor_, {nullptr, nullptr});
  }

  void InitializeWait(iree_async_handle_poll_operation_t* operation,
                      Completion* completion) {
    iree_async_operation_initialize(
        &operation->base, IREE_ASYNC_OPERATION_TYPE_HANDLE_POLL,
        IREE_ASYNC_OPERATION_FLAG_NONE, Completion::Record, completion);
    operation->primitive = iree_async_primitive_from_fd(event_fd_);
    operation->events = IREE_ASYNC_POLL_EVENT_IN;
  }

  void FillSubmissionQueue() {
    // A producer cannot enter the kernel. Joining it inside the owner callback
    // leaves the full prepared batch in the SQ until cancellation makes room.
    std::thread producer([&] {
      iree_async_operation_t* operations[IREE_ARRAYSIZE(fillers_)];
      for (iree_host_size_t i = 0; i < IREE_ARRAYSIZE(fillers_); ++i) {
        iree_async_operation_initialize(
            &fillers_[i].base, IREE_ASYNC_OPERATION_TYPE_TIMER,
            IREE_ASYNC_OPERATION_FLAG_NONE, Completion::Record,
            &filler_completion_);
        fillers_[i].deadline_ns = iree_time_now();
        operations[i] = &fillers_[i].base;
      }
      IREE_ASSERT_OK(iree_async_proactor_submit(
          proactor_, iree_async_operation_list_make(
                         operations, IREE_ARRAYSIZE(operations))));
      fillers_submitted_ = true;
    });
    producer.join();
  }

  // Proactor with an eight-entry SQ.
  iree_async_proactor_t* proactor_ = nullptr;
  // Kept unsignaled except in the explicit natural-completion test.
  int event_fd_ = -1;
  // Submission pressure owned until the native timer completions retire.
  iree_async_timer_operation_t fillers_[8] = {};
  // Completion join for the filler operations.
  Completion filler_completion_;
  // Whether teardown must join filler completions.
  bool fillers_submitted_ = false;
  // Disabled after setup to require allocation-free dispatch and cancellation.
  bool allocations_enabled_ = true;
  // Counts attempted host allocations after setup.
  std::atomic<size_t> rejected_allocations_{0};
};

TEST_P(IoUringCancelTest, OwnerCancelsPendingWaitWithFullSq) {
  Completion completion;
  iree_async_handle_poll_operation_t operation = {};
  InitializeWait(&operation, &completion);
  IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &operation.base));
  allocations_enabled_ = false;
  Dispatch([&] {
    FillSubmissionQueue();
    IREE_ASSERT_OK(iree_async_proactor_cancel(proactor_, &operation.base));
    EXPECT_EQ(completion.count, 0);
  });
  PollUntil([&] { return completion.count == 1; });
  EXPECT_EQ(completion.code, IREE_STATUS_CANCELLED);
}

TEST_P(IoUringCancelTest, OwnedCancellationJoinsWithFullSqWithoutAllocation) {
  OwnedCompletion completion(proactor_);
  iree_async_handle_poll_operation_t operation = {};
  InitializeWait(&operation, nullptr);
  operation.base.completion_fn = OwnedCompletion::Record;
  operation.base.user_data = &completion;
  IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &operation.base));
  allocations_enabled_ = false;
  Dispatch([&] {
    FillSubmissionQueue();
    IREE_ASSERT_OK(iree_async_proactor_request_cancel(
        proactor_, &operation.base, &completion.request));
    EXPECT_EQ(completion.target.count, 0);
    EXPECT_EQ(completion.receipts, 0);
  });
  PollUntil([&] { return completion.done(); });
  EXPECT_EQ(completion.target.code, IREE_STATUS_CANCELLED);
}

TEST_P(IoUringCancelTest, TargetWithdrawsQueuedKeyBeforeReusingAddress) {
  OwnedCompletion completion(proactor_);
  iree_async_handle_poll_operation_t operation = {};
  InitializeWait(&operation, nullptr);
  operation.base.completion_fn = OwnedCompletion::Record;
  operation.base.user_data = &completion;
  IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &operation.base));
  uint64_t value = 1;
  ASSERT_EQ(write(event_fd_, &value, sizeof(value)), sizeof(value));
  struct State {
    // Cancellation owner with a target CQE ready before control dispatch.
    OwnedCompletion* completion;
    // Existing target whose identity must be withdrawn without native issue.
    iree_async_operation_t* operation;
    // One-shot poll-owner hook preceding CQE dispatch.
    iree_async_progress_entry_t progress = {};
  } state{&completion, &operation.base};
  state.progress.user_data = &state;
  state.progress.fn =
      [](void* user_data,
         iree_host_size_t* out_completed_count) -> iree_status_t {
    auto* state = static_cast<State*>(user_data);
    *out_completed_count = 0;
    state->progress.remove_requested = true;
    return iree_async_proactor_request_cancel(state->completion->proactor,
                                              state->operation,
                                              &state->completion->request);
  };
  iree_async_proactor_register_progress(proactor_, &state.progress);
  allocations_enabled_ = false;
  PollUntil([&] { return completion.done(); });
  EXPECT_EQ(completion.target.code, IREE_STATUS_OK);
  EXPECT_EQ(completion.phase_at_target, IREE_ASYNC_CANCEL_REQUEST_PHASE_QUEUED);
  ASSERT_EQ(read(event_fd_, &value, sizeof(value)), sizeof(value));

  Completion replacement;
  InitializeWait(&operation, &replacement);
  IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &operation.base));
  Dispatch([] {});
  EXPECT_EQ(replacement.count, 0);
  value = 1;
  ASSERT_EQ(write(event_fd_, &value, sizeof(value)), sizeof(value));
  PollUntil([&] { return replacement.count == 1; });
  EXPECT_EQ(replacement.code, IREE_STATUS_OK);
}

TEST_P(IoUringCancelTest, RepeatedOwnerCancellationExceedsSqCapacity) {
  Completion completion;
  iree_async_handle_poll_operation_t operation = {};
  InitializeWait(&operation, &completion);
  IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &operation.base));
  allocations_enabled_ = false;
  Dispatch([&] {
    FillSubmissionQueue();
    for (int i = 0; i < 24; ++i) {
      IREE_ASSERT_OK(iree_async_proactor_cancel(proactor_, &operation.base));
    }
    EXPECT_EQ(completion.count, 0);
  });
  PollUntil([&] { return completion.count == 1; });
  EXPECT_EQ(completion.code, IREE_STATUS_CANCELLED);
  Dispatch([] {});
  EXPECT_EQ(completion.count, 1);
}

TEST_P(IoUringCancelTest, NonOwnerDoesNotEnterFullRing) {
  Completion completion;
  iree_async_handle_poll_operation_t operation = {};
  InitializeWait(&operation, &completion);
  IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &operation.base));
  Dispatch([] {});
  FillSubmissionQueue();
  std::thread caller([&] {
    IREE_EXPECT_STATUS_IS(
        IREE_STATUS_RESOURCE_EXHAUSTED,
        iree_async_proactor_cancel(proactor_, &operation.base));
  });
  caller.join();
  EXPECT_EQ(completion.count, 0);
  Dispatch([&] {
    IREE_ASSERT_OK(iree_async_proactor_cancel(proactor_, &operation.base));
  });
  PollUntil([&] { return completion.count == 1; });
  EXPECT_EQ(completion.code, IREE_STATUS_CANCELLED);
}

TEST_P(IoUringCancelTest, OwnerCancelsLinkedEventWaitWithFullSq) {
  iree_async_event_t* event = nullptr;
  IREE_ASSERT_OK(iree_async_event_create(proactor_, &event));
  Completion wait_completion;
  iree_async_event_wait_operation_t wait = {};
  iree_async_operation_initialize(
      &wait.base, IREE_ASYNC_OPERATION_TYPE_EVENT_WAIT,
      IREE_ASYNC_OPERATION_FLAG_LINKED, Completion::Record, &wait_completion);
  wait.event = event;
  Completion tail_completion;
  iree_async_operation_t tail = {};
  iree_async_operation_initialize(&tail, IREE_ASYNC_OPERATION_TYPE_NOP,
                                  IREE_ASYNC_OPERATION_FLAG_NONE,
                                  Completion::Record, &tail_completion);
  iree_async_operation_t* operations[] = {&wait.base, &tail};
  IREE_ASSERT_OK(iree_async_proactor_submit(
      proactor_, iree_async_operation_list_make(operations, 2)));
  allocations_enabled_ = false;
  Dispatch([&] {
    FillSubmissionQueue();
    IREE_ASSERT_OK(iree_async_proactor_cancel(proactor_, &wait.base));
  });
  PollUntil(
      [&] { return wait_completion.count == 1 && tail_completion.count == 1; });
  EXPECT_EQ(wait_completion.code, IREE_STATUS_CANCELLED);
  EXPECT_EQ(tail_completion.code, IREE_STATUS_CANCELLED);
  iree_async_event_release(event);
}

TEST_P(IoUringCancelTest, OwnerCancelsSilentListenerWithFullSq) {
  iree_async_socket_t* listener = nullptr;
  IREE_ASSERT_OK(iree_async_socket_create(proactor_, IREE_ASYNC_SOCKET_TYPE_TCP,
                                          IREE_ASYNC_SOCKET_OPTION_NONE,
                                          &listener));
  iree_async_address_t address;
  IREE_ASSERT_OK(
      iree_async_address_from_ipv4(IREE_SV("127.0.0.1"), 0, &address));
  IREE_ASSERT_OK(iree_async_socket_bind(listener, &address));
  IREE_ASSERT_OK(iree_async_socket_listen(listener, 1));
  Completion completion;
  iree_async_socket_accept_operation_t operation = {};
  iree_async_operation_initialize(
      &operation.base, IREE_ASYNC_OPERATION_TYPE_SOCKET_ACCEPT,
      IREE_ASYNC_OPERATION_FLAG_NONE, Completion::Record, &completion);
  operation.listen_socket = listener;
  IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &operation.base));
  allocations_enabled_ = false;
  Dispatch([&] {
    FillSubmissionQueue();
    IREE_ASSERT_OK(iree_async_proactor_cancel(proactor_, &operation.base));
  });
  PollUntil([&] { return completion.count == 1; });
  EXPECT_EQ(completion.code, IREE_STATUS_CANCELLED);
  EXPECT_EQ(operation.accepted_socket, nullptr);
  iree_async_socket_release(listener);
}

TEST_P(IoUringCancelTest, TerminalCallbackCanDestroyCancelledOperation) {
  struct State {
    // Caller storage destroyed directly from the terminal callback.
    std::unique_ptr<iree_async_handle_poll_operation_t> operation =
        std::make_unique<iree_async_handle_poll_operation_t>();
    // Terminal result stored independently of the destroyed operation.
    Completion completion;
  } state;
  InitializeWait(state.operation.get(), &state.completion);
  state.operation->base.user_data = &state;
  state.operation->base.completion_fn =
      [](void* user_data, iree_async_operation_t* operation,
         iree_status_t status, iree_async_completion_flags_t flags) {
        auto* state = static_cast<State*>(user_data);
        Completion::Record(&state->completion, operation, status, flags);
        state->operation.reset();
      };
  IREE_ASSERT_OK(
      iree_async_proactor_submit_one(proactor_, &state.operation->base));
  allocations_enabled_ = false;
  Dispatch([&] {
    FillSubmissionQueue();
    IREE_ASSERT_OK(
        iree_async_proactor_cancel(proactor_, &state.operation->base));
  });
  PollUntil([&] { return state.completion.count == 1; });
  EXPECT_EQ(state.completion.code, IREE_STATUS_CANCELLED);
  EXPECT_EQ(state.operation, nullptr);
  Dispatch([] {});
}

TEST_P(IoUringCancelTest, OwnedConnectDrainsWithoutListenerProgress) {
  iree_async_socket_t* listener = nullptr;
  IREE_ASSERT_OK(iree_async_socket_create(proactor_, IREE_ASYNC_SOCKET_TYPE_TCP,
                                          IREE_ASYNC_SOCKET_OPTION_NONE,
                                          &listener));
  iree_async_address_t address;
  IREE_ASSERT_OK(
      iree_async_address_from_ipv4(IREE_SV("127.0.0.1"), 0, &address));
  IREE_ASSERT_OK(iree_async_socket_bind(listener, &address));
  IREE_ASSERT_OK(iree_async_socket_listen(listener, 1));
  IREE_ASSERT_OK(iree_async_socket_query_local_address(listener, &address));

  // Linux admits backlog+1 completed handshakes. Leave both unaccepted so the
  // third connect stays pending against a real, live, non-progressing peer.
  iree_async_socket_t* clients[3] = {};
  iree_async_socket_connect_operation_t operations[3] = {};
  Completion completions[2];
  OwnedCompletion pending(proactor_);
  for (int i = 0; i < 3; ++i) {
    IREE_ASSERT_OK(
        iree_async_socket_create(proactor_, IREE_ASYNC_SOCKET_TYPE_TCP,
                                 IREE_ASYNC_SOCKET_OPTION_NONE, &clients[i]));
    iree_async_operation_initialize(
        &operations[i].base, IREE_ASYNC_OPERATION_TYPE_SOCKET_CONNECT,
        IREE_ASYNC_OPERATION_FLAG_NONE,
        i < 2 ? Completion::Record : OwnedCompletion::Record,
        i < 2 ? static_cast<void*>(&completions[i])
              : static_cast<void*>(&pending));
    operations[i].socket = clients[i];
    operations[i].address = address;
    IREE_ASSERT_OK(
        iree_async_proactor_submit_one(proactor_, &operations[i].base));
    if (i < 2) {
      PollUntil([&] { return completions[i].count == 1; });
      ASSERT_EQ(completions[i].code, IREE_STATUS_OK);
    }
  }
  allocations_enabled_ = false;
  Dispatch([&] {
    ASSERT_EQ(pending.target.count, 0);
    FillSubmissionQueue();
    IREE_ASSERT_OK(iree_async_proactor_request_cancel(
        proactor_, &operations[2].base, &pending.request));
  });
  PollUntil([&] { return pending.done(); });
  EXPECT_EQ(pending.target.code, IREE_STATUS_CANCELLED);
  for (auto* client : clients) {
    iree_async_socket_release(client);
  }
  iree_async_socket_release(listener);
}

TEST_P(IoUringCancelTest, CompletionReuseIsNotCancelledByEarlierRequests) {
  struct State {
    // Owner used to resubmit from the first callback.
    iree_async_proactor_t* proactor;
    // Event consumed before the operation is reused.
    int event_fd;
    // Number of terminal callbacks across both uses.
    int count = 0;
    // Result from the second use, which has no cancellation request.
    iree_status_code_t second_code = IREE_STATUS_UNKNOWN;
  } state{proactor_, event_fd_};
  iree_async_handle_poll_operation_t operation = {};
  InitializeWait(&operation, nullptr);
  operation.base.user_data = &state;
  operation.base.completion_fn = [](void* user_data,
                                    iree_async_operation_t* base,
                                    iree_status_t status,
                                    iree_async_completion_flags_t flags) {
    auto* state = static_cast<State*>(user_data);
    EXPECT_EQ(flags, IREE_ASYNC_COMPLETION_FLAG_NONE);
    iree_status_code_t code = iree_status_code(status);
    iree_status_free(status);
    if (++state->count == 1) {
      EXPECT_TRUE(code == IREE_STATUS_OK || code == IREE_STATUS_CANCELLED);
      uint64_t value = 0;
      ASSERT_EQ(read(state->event_fd, &value, sizeof(value)), sizeof(value));
      auto callback = base->completion_fn;
      iree_async_operation_initialize(
          base, IREE_ASYNC_OPERATION_TYPE_HANDLE_POLL,
          IREE_ASYNC_OPERATION_FLAG_NONE, callback, state);
      auto* operation =
          reinterpret_cast<iree_async_handle_poll_operation_t*>(base);
      operation->result_events = IREE_ASYNC_POLL_EVENT_NONE;
      IREE_ASSERT_OK(iree_async_proactor_submit_one(state->proactor, base));
    } else {
      state->second_code = code;
    }
  };
  IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &operation.base));
  allocations_enabled_ = false;
  Dispatch([&] {
    FillSubmissionQueue();
    uint64_t value = 1;
    ASSERT_EQ(write(event_fd_, &value, sizeof(value)), sizeof(value));
    for (int i = 0; i < 24; ++i) {
      IREE_ASSERT_OK(iree_async_proactor_cancel(proactor_, &operation.base));
    }
  });
  PollUntil([&] { return state.count >= 1; });
  Dispatch([] {});
  EXPECT_EQ(state.count, 1);
  uint64_t value = 1;
  ASSERT_EQ(write(event_fd_, &value, sizeof(value)), sizeof(value));
  PollUntil([&] { return state.count == 2; });
  EXPECT_EQ(state.second_code, IREE_STATUS_OK);
}

TEST_P(IoUringCancelTest, SoftwareDispatchAdmitsWithoutSqSpaceOrAllocation) {
  FillSubmissionQueue();
  allocations_enabled_ = false;
  Dispatch([] {});
}

TEST_P(IoUringCancelTest, LastDispatchWakesSoftwareWorkForNextPoll) {
  Completion completion;
  iree_async_operation_t operation = {};
  iree_async_operation_initialize(&operation, IREE_ASYNC_OPERATION_TYPE_NOP,
                                  IREE_ASYNC_OPERATION_FLAG_NONE,
                                  Completion::Record, &completion);
  allocations_enabled_ = false;
  Dispatch([&] {
    IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &operation));
  });
  PollUntil([&] { return completion.count == 1; });
  EXPECT_EQ(completion.code, IREE_STATUS_OK);
}

INSTANTIATE_TEST_SUITE_P(
    OwnerCompletion, IoUringCancelTest,
    ::testing::Values(DispatchPhase::kFirstDrain, DispatchPhase::kLastDrain),
    [](const ::testing::TestParamInfo<DispatchPhase>& info) {
      return info.param == DispatchPhase::kFirstDrain ? "FirstDrain"
                                                      : "LastDrain";
    });

}  // namespace
