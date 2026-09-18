// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// CTS tests for NOP operations.
//
// NOP is the simplest operation type - it completes immediately on the next
// poll with an OK status. These tests validate basic submit/poll/callback
// mechanics without involving timers, I/O, or complex state.

#include "iree/async/cts/util/registry.h"
#include "iree/async/cts/util/test_base.h"
#include "iree/async/operations/scheduling.h"

namespace iree::async::cts {

class NopTest : public CtsTestBase<> {};

struct NopOrderState {
  struct CallbackState {
    NopOrderState* owner;
    int index;
  };

  std::vector<int> completion_order;

  static void Callback(void* user_data, iree_async_operation_t* operation,
                       iree_status_t status,
                       iree_async_completion_flags_t flags) {
    (void)operation;
    (void)flags;
    auto* callback_state = static_cast<CallbackState*>(user_data);
    IREE_EXPECT_OK(status);
    callback_state->owner->completion_order.push_back(callback_state->index);
  }
};

// Single NOP: submit, poll, verify callback fires with OK status.
TEST_P(NopTest, SingleNop) {
  iree_async_nop_operation_t nop;
  memset(&nop, 0, sizeof(nop));
  nop.base.type = IREE_ASYNC_OPERATION_TYPE_NOP;

  CompletionTracker tracker;
  nop.base.completion_fn = CompletionTracker::Callback;
  nop.base.user_data = &tracker;

  IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &nop.base));

  // NOP should complete on the first poll.
  PollUntil(/*min_completions=*/1);

  EXPECT_EQ(tracker.call_count, 1);
  IREE_EXPECT_OK(tracker.ConsumeStatus());
  EXPECT_EQ(tracker.last_operation, &nop.base);
}

// Multiple NOPs: submit batch, verify all complete in submission order.
TEST_P(NopTest, MultipleNops) {
  constexpr int kNopCount = 5;
  iree_async_nop_operation_t nops[kNopCount];
  memset(nops, 0, sizeof(nops));

  NopOrderState order_state;
  NopOrderState::CallbackState callback_states[kNopCount];
  iree_async_operation_t* ops[kNopCount];
  for (int i = 0; i < kNopCount; ++i) {
    callback_states[i] = {&order_state, i};
    nops[i].base.type = IREE_ASYNC_OPERATION_TYPE_NOP;
    nops[i].base.completion_fn = NopOrderState::Callback;
    nops[i].base.user_data = &callback_states[i];
    ops[i] = &nops[i].base;
  }

  iree_async_operation_list_t list = {ops, kNopCount};
  IREE_ASSERT_OK(iree_async_proactor_submit(proactor_, list));

  // All NOPs should complete quickly.
  PollUntil(/*min_completions=*/kNopCount);

  EXPECT_EQ(order_state.completion_order, (std::vector<int>{0, 1, 2, 3, 4}));
}

TEST_P(NopTest, LinkedNops) {
  constexpr int kNopCount = 5;
  iree_async_nop_operation_t nops[kNopCount];
  memset(nops, 0, sizeof(nops));

  NopOrderState order_state;
  NopOrderState::CallbackState callback_states[kNopCount];
  iree_async_operation_t* ops[kNopCount];
  for (int i = 0; i < kNopCount; ++i) {
    callback_states[i] = {&order_state, i};
    nops[i].base.type = IREE_ASYNC_OPERATION_TYPE_NOP;
    nops[i].base.flags = i + 1 < kNopCount ? IREE_ASYNC_OPERATION_FLAG_LINKED
                                           : IREE_ASYNC_OPERATION_FLAG_NONE;
    nops[i].base.completion_fn = NopOrderState::Callback;
    nops[i].base.user_data = &callback_states[i];
    ops[i] = &nops[i].base;
  }

  IREE_ASSERT_OK(iree_async_proactor_submit(
      proactor_, iree_async_operation_list_make(ops, kNopCount)));
  PollUntil(/*min_completions=*/kNopCount);

  EXPECT_EQ(order_state.completion_order, (std::vector<int>{0, 1, 2, 3, 4}));
}

// NOP callback receives correct operation pointer.
TEST_P(NopTest, CallbackReceivesOperationPointer) {
  iree_async_nop_operation_t nop;
  memset(&nop, 0, sizeof(nop));
  nop.base.type = IREE_ASYNC_OPERATION_TYPE_NOP;

  iree_async_operation_t* received_op = nullptr;
  auto capture_callback = [](void* user_data, iree_async_operation_t* op,
                             iree_status_t status,
                             iree_async_completion_flags_t flags) {
    *static_cast<iree_async_operation_t**>(user_data) = op;
    iree_status_ignore(status);
  };

  nop.base.completion_fn = capture_callback;
  nop.base.user_data = &received_op;

  IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &nop.base));
  PollUntil(/*min_completions=*/1);

  EXPECT_EQ(received_op, &nop.base);
}

// Empty submit list: should succeed with no completions.
TEST_P(NopTest, EmptySubmit) {
  iree_async_operation_list_t empty_list = iree_async_operation_list_empty();
  IREE_ASSERT_OK(iree_async_proactor_submit(proactor_, empty_list));

  // Poll should return immediately with no completions (deadline exceeded).
  iree_host_size_t completed = 0;
  IREE_EXPECT_STATUS_IS(IREE_STATUS_DEADLINE_EXCEEDED,
                        iree_async_proactor_poll(
                            proactor_, iree_immediate_timeout(), &completed));
  EXPECT_EQ(completed, 0u);
}

//===----------------------------------------------------------------------===//
// Registration
//===----------------------------------------------------------------------===//

CTS_REGISTER_TEST_SUITE(NopTest);

}  // namespace iree::async::cts
