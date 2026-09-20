// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/async/platform/iocp/notification.h"

#include "iree/async/platform/iocp/proactor.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace {

// Counts the real native dependency calls; the notification implementation and
// its normal poll/submit/cancel paths remain the subject under test.
struct NativeCalls {
  // Real functions delegated to by the counting wrappers.
  decltype(iree_async_proactor_iocp_t::nt_wait_api) api = {};
  // Number of reusable packet creations.
  int creations = 0;
  // Number of association attempts.
  int associations = 0;
  // Number of withdrawals requested.
  int cancellations = 0;
  // Optional native admission failure, without issuing an association.
  LONG association_failure = 0;

  static thread_local NativeCalls* current;
  static LONG NTAPI Create(PHANDLE packet, ACCESS_MASK access,
                           PVOID attributes) {
    ++current->creations;
    return current->api.NtCreateWaitCompletionPacket(packet, access,
                                                     attributes);
  }
  static LONG NTAPI Associate(HANDLE packet, HANDLE port, HANDLE target,
                              PVOID key, PVOID context, LONG status,
                              ULONG_PTR information, LONG* signaled) {
    ++current->associations;
    if (current->association_failure) {
      return current->association_failure;
    }
    return current->api.NtAssociateWaitCompletionPacket(
        packet, port, target, key, context, status, information, signaled);
  }
  static LONG NTAPI Cancel(HANDLE packet, BOOLEAN remove) {
    ++current->cancellations;
    return current->api.NtCancelWaitCompletionPacket(packet, remove);
  }
};
thread_local NativeCalls* NativeCalls::current = nullptr;

struct Wait {
  // Accepted operation storage, reused only after terminal completion.
  iree_async_notification_wait_operation_t operation = {};
  // Terminal callback count across submissions.
  int completions = 0;
  // Last terminal status code.
  iree_status_code_t code = IREE_STATUS_UNKNOWN;
  // Remaining callback-driven submissions.
  int resubmissions = 0;

  static void Complete(void* context, iree_async_operation_t*,
                       iree_status_t status, iree_async_completion_flags_t) {
    auto* wait = static_cast<Wait*>(context);
    wait->code = iree_status_code(status);
    iree_status_free(status);
    ++wait->completions;
    if (wait->resubmissions) {
      --wait->resubmissions;
      auto* notification = wait->operation.notification;
      wait->Initialize(notification);
      IREE_ASSERT_OK(iree_async_proactor_submit_one(notification->proactor,
                                                    &wait->operation.base));
    }
  }
  void Initialize(iree_async_notification_t* notification) {
    iree_async_operation_zero(&operation.base, sizeof(operation));
    iree_async_operation_initialize(
        &operation.base, IREE_ASYNC_OPERATION_TYPE_NOTIFICATION_WAIT,
        IREE_ASYNC_OPERATION_FLAG_NONE, Complete, this);
    operation.notification = notification;
  }
};

class IocpNotificationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    IREE_ASSERT_OK(
        iree_async_proactor_create_iocp(iree_async_proactor_options_default(),
                                        iree_allocator_system(), &proactor_));
    auto* iocp = iree_async_proactor_iocp_cast(proactor_);
    if (!iocp->nt_wait_api.available) {
      GTEST_SKIP() << "native wait packets unavailable";
    }
    calls_.api = iocp->nt_wait_api;
    NativeCalls::current = &calls_;
    iocp->nt_wait_api.NtCreateWaitCompletionPacket = NativeCalls::Create;
    iocp->nt_wait_api.NtAssociateWaitCompletionPacket = NativeCalls::Associate;
    iocp->nt_wait_api.NtCancelWaitCompletionPacket = NativeCalls::Cancel;
    iree_notification_state_initialize(&state_);
    IREE_ASSERT_OK(
        iree_async_notification_native_initialize(&state_, &native_));
    IREE_ASSERT_OK(iree_async_notification_create_shared(proactor_, &native_,
                                                         &notification_));
  }
  void TearDown() override {
    iree_async_notification_release(notification_);
    iree_async_proactor_release(proactor_);
    iree_async_notification_native_deinitialize(&native_);
    NativeCalls::current = nullptr;
  }
  void Poll() {
    IREE_ASSERT_OK(
        iree_async_proactor_poll(proactor_, iree_infinite_timeout(), nullptr));
  }
  void Progress() {
    iree_async_proactor_wake(proactor_);
    Poll();
  }
  void Complete(Wait& wait, int count = 1) {
    while (wait.completions < count) {
      Poll();
    }
  }
  void Unregister(iree_async_relay_t* relay) {
    bool completed = false;
    iree_async_proactor_unregister_relay(
        proactor_, relay,
        {+[](void* context) { *static_cast<bool*>(context) = true; },
         &completed});
    while (!completed) {
      Poll();
    }
  }

  // Independent caller-owned shared state and native resources.
  iree_notification_state_t state_ = {};
  // Native publication bundle borrowed by the managed receiver.
  iree_async_notification_native_t native_ = {};
  // Poll owner, retained through native retirement.
  iree_async_proactor_t* proactor_ = nullptr;
  // Caller-owned receiver reference.
  iree_async_notification_t* notification_ = nullptr;
  // Native dependency accounting.
  NativeCalls calls_;
};

TEST_F(IocpNotificationTest, IdleReceiverOwnsNoNativeAssociation) {
  EXPECT_EQ(calls_.creations, 1);
  EXPECT_EQ(calls_.associations, 0);
  iree_async_notification_release(notification_);
  notification_ = nullptr;
  EXPECT_EQ(calls_.cancellations, 0);
}

TEST_F(IocpNotificationTest, CallbackRearmReusesPacketWithoutWithdrawal) {
  constexpr int kCycles = 64;
  Wait wait;
  wait.Initialize(notification_);
  wait.resubmissions = kCycles - 1;
  IREE_ASSERT_OK(
      iree_async_proactor_submit_one(proactor_, &wait.operation.base));
  for (int i = 0; i < kCycles; ++i) {
    Progress();
    ASSERT_EQ(calls_.associations, i + 1);
    iree_async_notification_native_signal(&native_, 1);
    Complete(wait, i + 1);
    ASSERT_EQ(wait.code, IREE_STATUS_OK);
    EXPECT_EQ(calls_.creations, 1);
    EXPECT_EQ(calls_.cancellations, 0);
    EXPECT_EQ(calls_.associations, i + 1);
  }
}

TEST_F(IocpNotificationTest, CancellationReturnsTheLastConsumerWithoutSignal) {
  Wait wait;
  wait.Initialize(notification_);
  IREE_ASSERT_OK(
      iree_async_proactor_submit_one(proactor_, &wait.operation.base));
  Progress();
  ASSERT_EQ(calls_.associations, 1);
  // The accepted operation is now the receiver's only owner. Native resources
  // remain caller-owned and alive through terminal completion.
  iree_async_notification_release(notification_);
  notification_ = nullptr;
  IREE_ASSERT_OK(iree_async_proactor_cancel(proactor_, &wait.operation.base));
  Complete(wait);
  EXPECT_EQ(wait.code, IREE_STATUS_CANCELLED);
  EXPECT_EQ(calls_.cancellations, 1);
  EXPECT_EQ(iree_notification_state_query_epoch(&state_), 0u);
}

TEST_F(IocpNotificationTest, NativeAdmissionFailureCompletesAcceptedWait) {
  // Dependency failure occurs before native ownership, without invalidating
  // any live handle or changing the subject's own implementation.
  calls_.association_failure = static_cast<LONG>(0xC000009AL);
  Wait wait;
  wait.Initialize(notification_);
  IREE_ASSERT_OK(
      iree_async_proactor_submit_one(proactor_, &wait.operation.base));
  Complete(wait);
  EXPECT_EQ(wait.code, IREE_STATUS_INTERNAL);
  EXPECT_EQ(calls_.associations, 1);
  EXPECT_EQ(calls_.cancellations, 0);
  wait.Initialize(notification_);
  IREE_ASSERT_OK(
      iree_async_proactor_submit_one(proactor_, &wait.operation.base));
  Complete(wait, 2);
  EXPECT_EQ(wait.code, IREE_STATUS_INTERNAL);
  EXPECT_EQ(calls_.associations, 1);
}

TEST_F(IocpNotificationTest, CancellationJoinsAlreadyDequeuedNativeDelivery) {
  Wait wait;
  wait.Initialize(notification_);
  IREE_ASSERT_OK(
      iree_async_proactor_submit_one(proactor_, &wait.operation.base));
  Progress();
  iree_async_notification_native_signal(&native_, 1);
  auto* iocp = iree_async_proactor_iocp_cast(proactor_);
  OVERLAPPED_ENTRY entry = {};
  ULONG count = 0;
  do {
    ASSERT_TRUE(GetQueuedCompletionStatusEx(
        reinterpret_cast<HANDLE>(iocp->completion_port.handle), &entry, 1,
        &count, INFINITE, FALSE));
  } while (!count || entry.lpCompletionKey !=
                         IREE_ASYNC_IOCP_SHARED_NOTIFICATION_COMPLETION_KEY);
  ASSERT_EQ(reinterpret_cast<void*>(entry.lpOverlapped), notification_);
  // Model the poller's still-owned batch entry. All owners remain retained;
  // cancellation must not publish terminal completion before its dispatch.
  IREE_ASSERT_OK(iree_async_proactor_cancel(proactor_, &wait.operation.base));
  Progress();
  EXPECT_EQ(wait.completions, 0);
  EXPECT_EQ(calls_.cancellations, 1);
  iree_async_iocp_notification_wake(notification_);
  Progress();
  EXPECT_EQ(wait.completions, 1);
  EXPECT_EQ(wait.code, IREE_STATUS_CANCELLED);
  EXPECT_EQ(calls_.cancellations, 1);
  EXPECT_EQ(calls_.associations, 1);
}

TEST_F(IocpNotificationTest, FaultCallbackAdmissionSurvivesRemainingConsumers) {
  calls_.association_failure = static_cast<LONG>(0xC000009AL);
  struct State {
    // Poll owner for callback-driven registration.
    iree_async_proactor_t* proactor;
    // Source whose native monitor reports admission failure.
    iree_async_notification_t* source;
    // Valid sink retained through every relay.
    iree_async_notification_t* sink;
    // Persistent handles unregistered after all fault callbacks return.
    iree_async_relay_t* relays[3] = {};
    // Number of delivered fault callbacks.
    int faults = 0;
    static void Fault(void* context, iree_async_relay_t*,
                      iree_status_t status) {
      auto* state = static_cast<State*>(context);
      EXPECT_EQ(iree_status_code(status), IREE_STATUS_INTERNAL);
      iree_status_free(status);
      if (++state->faults == 1) {
        IREE_ASSERT_OK(iree_async_proactor_register_relay(
            state->proactor,
            iree_async_relay_source_from_notification(state->source),
            iree_async_relay_sink_signal_notification(state->sink, 1),
            IREE_ASYNC_RELAY_FLAG_PERSISTENT, {Fault, state},
            &state->relays[2]));
      }
    }
  } state = {proactor_, notification_, nullptr};
  IREE_ASSERT_OK(iree_async_notification_create(
      proactor_, IREE_ASYNC_NOTIFICATION_FLAG_NONE, &state.sink));
  for (int i = 0; i < 2; ++i) {
    IREE_ASSERT_OK(iree_async_proactor_register_relay(
        proactor_, iree_async_relay_source_from_notification(notification_),
        iree_async_relay_sink_signal_notification(state.sink, 1),
        IREE_ASYNC_RELAY_FLAG_PERSISTENT, {State::Fault, &state},
        &state.relays[i]));
  }
  while (state.faults < 3) {
    Poll();
  }
  EXPECT_EQ(state.faults, 3);
  EXPECT_EQ(calls_.associations, 1);
  for (auto* relay : state.relays) {
    Unregister(relay);
  }
  iree_async_notification_release(state.sink);
}

TEST_F(IocpNotificationTest, DestructionJoinsSharedRelayUnregistration) {
  iree_async_notification_t* sink = nullptr;
  IREE_ASSERT_OK(iree_async_notification_create(
      proactor_, IREE_ASYNC_NOTIFICATION_FLAG_NONE, &sink));
  iree_async_relay_t* relay = nullptr;
  IREE_ASSERT_OK(iree_async_proactor_register_relay(
      proactor_, iree_async_relay_source_from_notification(notification_),
      iree_async_relay_sink_signal_notification(sink, 1),
      IREE_ASYNC_RELAY_FLAG_PERSISTENT, iree_async_relay_error_callback_none(),
      &relay));
  Progress();
  ASSERT_EQ(calls_.associations, 1);
  bool completed = false;
  iree_async_proactor_unregister_relay(
      proactor_, relay,
      {+[](void* context) { *static_cast<bool*>(context) = true; },
       &completed});
  iree_async_notification_release(sink);
  iree_async_notification_release(notification_);
  notification_ = nullptr;
  iree_async_proactor_release(proactor_);
  proactor_ = nullptr;
  EXPECT_TRUE(completed);
  EXPECT_EQ(calls_.cancellations, 1);
}

TEST_F(IocpNotificationTest, RelaySourceIsLocalButSinkMayUseAnotherProactor) {
  iree_async_proactor_t* peer = nullptr;
  IREE_ASSERT_OK(iree_async_proactor_create_iocp(
      iree_async_proactor_options_default(), iree_allocator_system(), &peer));
  iree_async_notification_t* sink = nullptr;
  IREE_ASSERT_OK(iree_async_notification_create(
      peer, IREE_ASYNC_NOTIFICATION_FLAG_NONE, &sink));
  iree_async_relay_t* relay = nullptr;
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      iree_async_proactor_register_relay(
          proactor_, iree_async_relay_source_from_notification(sink),
          iree_async_relay_sink_signal_notification(notification_, 1),
          IREE_ASYNC_RELAY_FLAG_NONE, iree_async_relay_error_callback_none(),
          &relay));
  EXPECT_EQ(relay, nullptr);
  IREE_ASSERT_OK(iree_async_proactor_register_relay(
      proactor_, iree_async_relay_source_from_notification(notification_),
      iree_async_relay_sink_signal_notification(sink, 1),
      IREE_ASYNC_RELAY_FLAG_NONE, iree_async_relay_error_callback_none(),
      &relay));
  Progress();
  iree_async_notification_native_signal(&native_, 1);
  while (!iree_async_notification_query_epoch(sink)) {
    Poll();
  }
  EXPECT_EQ(iree_async_notification_query_epoch(sink), 1u);
  iree_async_notification_release(sink);
  iree_async_proactor_release(peer);
}

}  // namespace
