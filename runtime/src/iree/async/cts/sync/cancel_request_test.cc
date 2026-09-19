// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <array>
#include <memory>
#include <vector>

#include "iree/async/cts/util/registry.h"
#include "iree/async/cts/util/socket_test_base.h"
#include "iree/async/event.h"
#include "iree/async/operations/net.h"
#include "iree/async/operations/scheduling.h"

namespace iree::async::cts {
namespace {

// Models a private factory operation: the target and cancellation receipt own
// independent obligations, and the last callback may destroy the entire owner.
struct CancelJoin {
  // Poll owner used for request admission and target-retired notification.
  iree_async_proactor_t* proactor;
  // Embedded storage borrowed until the cancellation receipt.
  iree_async_cancel_request_t request = {};
  // Whether the receipt still owns the request storage.
  bool receipt_pending = false;
  // Whether the original operation still owns its storage.
  bool target_pending = true;
  // Counts each independently delivered receipt.
  int receipts = 0;
  // Counts final target callbacks, excluding multishot results.
  int terminals = 0;
  // Number of intermediate accepts preceding retirement.
  int accepts = 0;
  // Final operation result, captured before callback-owned destruction.
  iree_status_code_t code = IREE_STATUS_UNKNOWN;
  // Optional terminal owner action.
  void (*joined)(void*) = nullptr;
  // Borrowed context for the terminal owner action.
  void* user_data = nullptr;

  explicit CancelJoin(iree_async_proactor_t* proactor) : proactor(proactor) {
    iree_async_cancel_request_initialize({Receipt, this}, &request);
  }

  CancelJoin(const CancelJoin&) = delete;
  CancelJoin& operator=(const CancelJoin&) = delete;

  bool done() const { return !receipt_pending && !target_pending; }

  void Cancel(iree_async_operation_t* target) {
    receipt_pending = true;
    IREE_CHECK_OK(
        iree_async_proactor_request_cancel(proactor, target, &request));
    EXPECT_EQ(receipts, 0);
  }

  void Finish() {
    if (done() && joined) {
      joined(user_data);
    }
  }

  static void Receipt(void* user_data) {
    auto* self = static_cast<CancelJoin*>(user_data);
    EXPECT_TRUE(self->receipt_pending);
    self->receipt_pending = false;
    ++self->receipts;
    self->Finish();
  }

  static void Complete(void* user_data, iree_async_operation_t* operation,
                       iree_status_t status,
                       iree_async_completion_flags_t flags) {
    auto* self = static_cast<CancelJoin*>(user_data);
    if (operation->type == IREE_ASYNC_OPERATION_TYPE_SOCKET_ACCEPT) {
      auto* accept =
          reinterpret_cast<iree_async_socket_accept_operation_t*>(operation);
      if (accept->accepted_socket) {
        ++self->accepts;
        iree_async_socket_release(accept->accepted_socket);
        accept->accepted_socket = nullptr;
      }
    }
    if (iree_any_bit_set(flags, IREE_ASYNC_COMPLETION_FLAG_MORE)) {
      IREE_EXPECT_OK(status);
      return;
    }
    self->code = iree_status_code(status);
    iree_status_free(status);
    ++self->terminals;
    self->target_pending = false;
    if (self->receipt_pending) {
      // Withdrawal can run Receipt inline and delete self. Issued requests
      // instead finish later; neither case permits another access here.
      iree_async_proactor_cancel_request_target_retired(self->proactor,
                                                        &self->request);
      return;
    }
    self->Finish();
  }
};

class CancelRequestTest : public SocketTestBase<> {
 protected:
  void SetUp() override {
    SocketTestBase<>::SetUp();
    if (HasFatalFailure() || IsSkipped()) {
      return;
    }
    IREE_ASSERT_OK(iree_async_event_create(proactor_, &event_));
  }

  void TearDown() override {
    iree_async_event_release(event_);
    SocketTestBase<>::TearDown();
  }

  iree_async_handle_poll_operation_t MakeWait(CancelJoin* join) {
    iree_async_handle_poll_operation_t operation = {};
    iree_async_operation_initialize(
        &operation.base, IREE_ASYNC_OPERATION_TYPE_HANDLE_POLL,
        IREE_ASYNC_OPERATION_FLAG_NONE, CancelJoin::Complete, join);
    operation.primitive = event_->primitive;
    operation.events = IREE_ASYNC_POLL_EVENT_IN;
    return operation;
  }

  void PollAvailable() {
    iree_status_t status =
        iree_async_proactor_poll(proactor_, iree_immediate_timeout(), nullptr);
    if (iree_status_is_deadline_exceeded(status)) {
      iree_status_free(status);
    } else {
      IREE_ASSERT_OK(status);
    }
  }

  template <typename Callback>
  void Dispatch(Callback callback) {
    struct DispatchState {
      // Owner-thread action.
      Callback callback;
      // Set only once the action has returned.
      bool completed = false;
    } state{callback};
    iree_async_operation_t operation = {};
    iree_async_operation_initialize(
        &operation, IREE_ASYNC_OPERATION_TYPE_NOP,
        IREE_ASYNC_OPERATION_FLAG_NONE,
        [](void* user_data, iree_async_operation_t*, iree_status_t status,
           iree_async_completion_flags_t) {
          IREE_EXPECT_OK(status);
          auto* state = static_cast<DispatchState*>(user_data);
          state->callback();
          state->completed = true;
        },
        &state);
    IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &operation));
    PollUntilCondition([&] { return state.completed; });
  }

  // Primitive remains unsignaled except in explicit natural-completion races.
  iree_async_event_t* event_ = nullptr;
};

TEST_P(CancelRequestTest, CancelBeforeFirstPollWithoutPeerProgress) {
  CancelJoin join(proactor_);
  auto operation = MakeWait(&join);
  IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &operation.base));
  join.Cancel(&operation.base);
  PollUntilCondition([&] { return join.done(); });
  EXPECT_EQ(join.code, IREE_STATUS_CANCELLED);
  EXPECT_EQ(join.terminals, 1);
  EXPECT_EQ(join.receipts, 1);
}

TEST_P(CancelRequestTest, CancelRegisteredWaitWithoutPeerProgress) {
  CancelJoin join(proactor_);
  auto operation = MakeWait(&join);
  IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &operation.base));
  PollAvailable();
  EXPECT_EQ(join.terminals, 0);
  Dispatch([&] { join.Cancel(&operation.base); });
  PollUntilCondition([&] { return join.done(); });
  EXPECT_EQ(join.code, IREE_STATUS_CANCELLED);
  EXPECT_EQ(join.receipts, 1);
}

TEST_P(CancelRequestTest, CallbackCancelsWaitBeforeRegistration) {
  CancelJoin join(proactor_);
  auto operation = MakeWait(&join);
  CompletionTracker marker;
  iree_async_operation_t trailing = {};
  iree_async_operation_initialize(&trailing, IREE_ASYNC_OPERATION_TYPE_NOP, 0,
                                  CompletionTracker::Callback, &marker);
  Dispatch([&] {
    IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &operation.base));
    IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &trailing));
    join.Cancel(&operation.base);
  });
  PollUntilCondition([&] { return join.done() && marker.call_count == 1; });
  EXPECT_EQ(join.code, IREE_STATUS_CANCELLED);
  IREE_EXPECT_OK(marker.ConsumeStatus());
}

TEST_P(CancelRequestTest, NaturalCompletionRacesCancellation) {
  CancelJoin join(proactor_);
  auto operation = MakeWait(&join);
  IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &operation.base));
  IREE_ASSERT_OK(iree_async_event_set(event_));
  join.Cancel(&operation.base);
  PollUntilCondition([&] { return join.done(); });
  EXPECT_TRUE(join.code == IREE_STATUS_OK ||
              join.code == IREE_STATUS_CANCELLED);
  EXPECT_EQ(join.terminals, 1);
  EXPECT_EQ(join.receipts, 1);
}

TEST_P(CancelRequestTest, JoinCallbackDestroysItsOwner) {
  struct Owner {
    // The two-obligation join is embedded in the owning object.
    CancelJoin join;
    // The target is destroyed together with the request at the join.
    iree_async_handle_poll_operation_t operation = {};
    // Completion witness outside the destroyed storage.
    bool* destroyed;
  };
  bool destroyed = false;
  auto* owner = new Owner{CancelJoin(proactor_), {}, &destroyed};
  owner->operation = MakeWait(&owner->join);
  owner->join.user_data = owner;
  owner->join.joined = [](void* user_data) {
    auto* owner = static_cast<Owner*>(user_data);
    EXPECT_EQ(owner->join.code, IREE_STATUS_CANCELLED);
    EXPECT_EQ(owner->join.receipts, 1);
    EXPECT_EQ(owner->join.terminals, 1);
    *owner->destroyed = true;
    delete owner;
  };
  IREE_ASSERT_OK(
      iree_async_proactor_submit_one(proactor_, &owner->operation.base));
  owner->join.Cancel(&owner->operation.base);
  PollUntilCondition([&] { return destroyed; });
  Dispatch([] {});
}

TEST_P(CancelRequestTest, JoinReusesTargetAddressForAnUncancelledWait) {
  CancelJoin join(proactor_);
  auto operation = MakeWait(&join);
  CompletionTracker replacement;
  struct State {
    // Same proactor for both executions of the same target address.
    iree_async_proactor_t* proactor;
    // Target storage reused after both obligations join.
    iree_async_handle_poll_operation_t* operation;
    // Replacement execution has independent completion ownership.
    CompletionTracker* replacement;
  } state{proactor_, &operation, &replacement};
  join.user_data = &state;
  join.joined = [](void* user_data) {
    auto* state = static_cast<State*>(user_data);
    iree_async_operation_initialize(
        &state->operation->base, IREE_ASYNC_OPERATION_TYPE_HANDLE_POLL, 0,
        CompletionTracker::Callback, state->replacement);
    IREE_ASSERT_OK(iree_async_proactor_submit_one(state->proactor,
                                                  &state->operation->base));
  };
  IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &operation.base));
  join.Cancel(&operation.base);
  PollUntilCondition([&] { return join.done(); });
  Dispatch([] {});
  EXPECT_EQ(replacement.call_count, 0);
  IREE_ASSERT_OK(iree_async_event_set(event_));
  PollUntilCondition([&] { return replacement.call_count == 1; });
  IREE_EXPECT_OK(replacement.ConsumeStatus());
}

TEST_P(CancelRequestTest, CancellingOneWaitPreservesItsDescriptorNeighbor) {
  CancelJoin cancelled(proactor_);
  auto first = MakeWait(&cancelled);
  CompletionTracker survivor;
  auto second = MakeWait(nullptr);
  second.base.completion_fn = CompletionTracker::Callback;
  second.base.user_data = &survivor;
  iree_async_operation_t* operations[] = {&first.base, &second.base};
  IREE_ASSERT_OK(iree_async_proactor_submit(
      proactor_, iree_async_operation_list_make(operations, 2)));
  PollAvailable();
  cancelled.Cancel(&first.base);
  PollUntilCondition([&] { return cancelled.done(); });
  EXPECT_EQ(cancelled.code, IREE_STATUS_CANCELLED);
  EXPECT_EQ(survivor.call_count, 0);
  IREE_ASSERT_OK(iree_async_event_set(event_));
  PollUntilCondition([&] { return survivor.call_count == 1; });
  IREE_EXPECT_OK(survivor.ConsumeStatus());
}

TEST_P(CancelRequestTest, ReceiptCanEnqueueTheNextCancellation) {
  CancelJoin first(proactor_);
  CancelJoin second(proactor_);
  auto first_operation = MakeWait(&first);
  auto second_operation = MakeWait(&second);
  struct State {
    // Receipt being delivered before admitting the next request.
    CancelJoin* first;
    // New cancellation is admitted from another request's receipt.
    CancelJoin* next;
    // Target remains live while the first cancellation retires.
    iree_async_operation_t* target;
  } state{&first, &second, &second_operation.base};
  iree_async_cancel_request_initialize({[](void* user_data) {
                                          auto* state =
                                              static_cast<State*>(user_data);
                                          CancelJoin::Receipt(state->first);
                                          state->next->Cancel(state->target);
                                        },
                                        &state},
                                       &first.request);
  iree_async_operation_t* operations[] = {&first_operation.base,
                                          &second_operation.base};
  IREE_ASSERT_OK(iree_async_proactor_submit(
      proactor_, iree_async_operation_list_make(operations, 2)));
  first.Cancel(&first_operation.base);
  PollUntilCondition([&] { return first.done() && second.done(); });
  EXPECT_EQ(first.code, IREE_STATUS_CANCELLED);
  EXPECT_EQ(second.code, IREE_STATUS_CANCELLED);
}

TEST_P(CancelRequestTest, CancellationBurstYieldsToUnrelatedWork) {
  constexpr size_t kCount = 80;
  std::array<iree_async_handle_poll_operation_t, kCount> operations = {};
  std::vector<std::unique_ptr<CancelJoin>> joins;
  for (size_t i = 0; i < kCount; ++i) {
    joins.push_back(std::make_unique<CancelJoin>(proactor_));
    operations[i] = MakeWait(joins.back().get());
    IREE_ASSERT_OK(
        iree_async_proactor_submit_one(proactor_, &operations[i].base));
  }
  Dispatch([&] {
    for (size_t i = 0; i < kCount; ++i) {
      joins[i]->Cancel(&operations[i].base);
    }
  });
  CompletionTracker marker;
  iree_async_operation_t marker_operation = {};
  iree_async_operation_initialize(&marker_operation,
                                  IREE_ASYNC_OPERATION_TYPE_NOP, 0,
                                  CompletionTracker::Callback, &marker);
  IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &marker_operation));
  PollUntilCondition([&] {
    if (marker.call_count != 1) {
      return false;
    }
    for (const auto& join : joins) {
      if (!join->done()) {
        return false;
      }
    }
    return true;
  });
  IREE_EXPECT_OK(marker.ConsumeStatus());
  for (const auto& join : joins) {
    EXPECT_EQ(join->code, IREE_STATUS_CANCELLED);
    EXPECT_EQ(join->receipts, 1);
    EXPECT_EQ(join->terminals, 1);
  }
}

TEST_P(CancelRequestTest, SilentAcceptRetiresWithoutClosingListener) {
  iree_async_address_t address;
  iree_async_socket_t* listener = CreateListener(&address);
  CancelJoin join(proactor_);
  iree_async_socket_accept_operation_t operation = {};
  InitAcceptOperation(&operation, listener, CancelJoin::Complete, &join);
  IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &operation.base));
  Dispatch([&] { join.Cancel(&operation.base); });
  PollUntilCondition([&] { return join.done(); });
  EXPECT_EQ(join.code, IREE_STATUS_CANCELLED);
  EXPECT_EQ(join.accepts, 0);
  iree_async_socket_release(listener);
}

TEST_P(CancelRequestTest, MultishotAcceptJoinsAfterIntermediateResult) {
  if (!iree_any_bit_set(iree_async_proactor_query_capabilities(proactor_),
                        IREE_ASYNC_PROACTOR_CAPABILITY_MULTISHOT)) {
    GTEST_SKIP() << "Multishot is disabled for this backend configuration";
  }
  iree_async_address_t address;
  iree_async_socket_t* listener = CreateListener(&address);
  CancelJoin join(proactor_);
  iree_async_socket_accept_operation_t operation = {};
  InitAcceptOperation(&operation, listener, CancelJoin::Complete, &join);
  operation.base.flags = IREE_ASYNC_OPERATION_FLAG_MULTISHOT;
  IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &operation.base));
  iree_async_socket_t* client = nullptr;
  IREE_ASSERT_OK(iree_async_socket_create(proactor_, IREE_ASYNC_SOCKET_TYPE_TCP,
                                          0, &client));
  CompletionTracker connected;
  iree_async_socket_connect_operation_t connect = {};
  InitConnectOperation(&connect, client, address, CompletionTracker::Callback,
                       &connected);
  IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &connect.base));
  PollUntilCondition(
      [&] { return join.accepts == 1 && connected.call_count == 1; });
  IREE_EXPECT_OK(connected.ConsumeStatus());
  EXPECT_EQ(join.terminals, 0);
  join.Cancel(&operation.base);
  PollUntilCondition([&] { return join.done(); });
  EXPECT_EQ(join.code, IREE_STATUS_CANCELLED);
  EXPECT_EQ(join.receipts, 1);
  EXPECT_EQ(join.terminals, 1);
  iree_async_socket_release(client);
  iree_async_socket_release(listener);
}

TEST_P(CancelRequestTest, ConnectResultAndCancellationReceiptJoin) {
  iree_async_address_t address;
  iree_async_socket_t* listener = CreateListener(&address);
  iree_async_socket_t* client = nullptr;
  IREE_ASSERT_OK(iree_async_socket_create(proactor_, IREE_ASYNC_SOCKET_TYPE_TCP,
                                          0, &client));
  CancelJoin join(proactor_);
  iree_async_socket_connect_operation_t operation = {};
  InitConnectOperation(&operation, client, address, CancelJoin::Complete,
                       &join);
  IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &operation.base));
  join.Cancel(&operation.base);
  PollUntilCondition([&] { return join.done(); });
  EXPECT_TRUE(join.code == IREE_STATUS_OK ||
              join.code == IREE_STATUS_CANCELLED);
  EXPECT_EQ(join.terminals, 1);
  EXPECT_EQ(join.receipts, 1);
  iree_async_socket_release(client);
  iree_async_socket_release(listener);
}

CTS_REGISTER_TEST_SUITE(CancelRequestTest);

}  // namespace
}  // namespace iree::async::cts
