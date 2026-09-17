// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/carrier.h"

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace iree {
namespace net {
namespace {

struct MockCarrier {
  iree_net_carrier_t base;
  uint8_t reservation_storage[64];
  int query_budget_count = 0;
  int send_count = 0;
  int begin_send_count = 0;
  int commit_send_count = 0;
  int abort_send_count = 0;
  int error_count = 0;
  iree_status_code_t error_code = IREE_STATUS_OK;
  iree_host_size_t reserved_size = 0;
  iree_net_send_completion_callback_t pending_completion = {0};

  static void Destroy(iree_net_carrier_t* carrier) {
    IREE_ASSERT(false && "stack carrier must not be released");
  }

  static iree_status_t Activate(iree_net_carrier_t* carrier) {
    iree_net_carrier_set_state(carrier, IREE_NET_CARRIER_STATE_ACTIVE);
    return iree_ok_status();
  }

  static void Deactivate(iree_net_carrier_t* carrier,
                         iree_net_carrier_deactivate_callback_fn_t callback,
                         void* user_data) {
    iree_net_carrier_set_state(carrier, IREE_NET_CARRIER_STATE_DEACTIVATED);
    callback(user_data);
  }

  static iree_net_carrier_send_budget_t QuerySendBudget(
      iree_net_carrier_t* carrier) {
    MockCarrier* mock = reinterpret_cast<MockCarrier*>(carrier);
    ++mock->query_budget_count;
    return {/*.bytes=*/4096, /*.slots=*/4};
  }

  static iree_status_t Send(iree_net_carrier_t* carrier,
                            const iree_net_send_params_t* params) {
    MockCarrier* mock = reinterpret_cast<MockCarrier*>(carrier);
    ++mock->send_count;
    mock->pending_completion = params->completion_callback;
    return iree_ok_status();
  }

  static iree_status_t BeginSend(iree_net_carrier_t* carrier,
                                 iree_host_size_t size, void** out_ptr,
                                 iree_net_carrier_send_handle_t* out_handle) {
    MockCarrier* mock = reinterpret_cast<MockCarrier*>(carrier);
    ++mock->begin_send_count;
    mock->reserved_size = size;
    *out_ptr = mock->reservation_storage;
    *out_handle = 42;
    return iree_ok_status();
  }

  static iree_status_t CommitSend(
      iree_net_carrier_t* carrier, iree_net_carrier_send_handle_t handle,
      iree_net_send_completion_callback_t completion_callback) {
    MockCarrier* mock = reinterpret_cast<MockCarrier*>(carrier);
    EXPECT_EQ(handle, 42u);
    ++mock->commit_send_count;
    mock->pending_completion = completion_callback;
    return iree_ok_status();
  }

  static void AbortSend(iree_net_carrier_t* carrier,
                        iree_net_carrier_send_handle_t handle) {
    MockCarrier* mock = reinterpret_cast<MockCarrier*>(carrier);
    EXPECT_EQ(handle, 42u);
    ++mock->abort_send_count;
  }

  static iree_status_t Shutdown(iree_net_carrier_t* carrier) {
    return iree_ok_status();
  }

  static iree_status_t HandleReceive(void* user_data, iree_async_span_t data,
                                     iree_async_buffer_lease_t* lease) {
    return iree_ok_status();
  }

  static void HandleError(void* user_data, iree_status_t status) {
    MockCarrier* mock = static_cast<MockCarrier*>(user_data);
    ++mock->error_count;
    mock->error_code = iree_status_code(status);
    iree_status_free(status);
  }

  void Initialize();
  void Deinitialize() { iree_net_carrier_deinitialize(&base); }

  static const iree_net_carrier_vtable_t kVtable;
};

const iree_net_carrier_vtable_t MockCarrier::kVtable = {
    /*.destroy=*/MockCarrier::Destroy,
    /*.activate=*/MockCarrier::Activate,
    /*.deactivate=*/MockCarrier::Deactivate,
    /*.query_send_budget=*/MockCarrier::QuerySendBudget,
    /*.send=*/MockCarrier::Send,
    /*.begin_send=*/MockCarrier::BeginSend,
    /*.commit_send=*/MockCarrier::CommitSend,
    /*.abort_send=*/MockCarrier::AbortSend,
    /*.shutdown=*/MockCarrier::Shutdown,
};

void MockCarrier::Initialize() {
  iree_net_carrier_initialize(&kVtable, IREE_NET_CARRIER_CAPABILITY_RELIABLE,
                              /*max_send_spans=*/8, iree_allocator_system(),
                              &base);
  IREE_ASSERT_OK(iree_net_carrier_set_handlers(
      &base, {/*.on_receive=*/MockCarrier::HandleReceive,
              /*.on_error=*/MockCarrier::HandleError,
              /*.user_data=*/this}));
}

struct SendCompletion {
  int count = 0;
  iree_status_code_t status_code = IREE_STATUS_UNKNOWN;
  iree_host_size_t bytes_transferred = 0;

  static void Handle(void* user_data, iree_status_t status,
                     iree_host_size_t bytes_transferred) {
    SendCompletion* completion = static_cast<SendCompletion*>(user_data);
    ++completion->count;
    completion->status_code = iree_status_code(status);
    completion->bytes_transferred = bytes_transferred;
    iree_status_free(status);
  }
};

class CarrierTest : public ::testing::Test {
 protected:
  void SetUp() override { carrier_.Initialize(); }
  void TearDown() override { carrier_.Deinitialize(); }

  MockCarrier carrier_;
};

TEST_F(CarrierTest, BeginSendValidatesAndClearsOutputs) {
  void* data = reinterpret_cast<void*>(UINTPTR_MAX);
  iree_net_carrier_send_handle_t handle = UINT64_MAX;
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      iree_net_carrier_begin_send(&carrier_.base, 0, &data, &handle));
  EXPECT_EQ(data, nullptr);
  EXPECT_EQ(handle, 0u);
  EXPECT_EQ(carrier_.begin_send_count, 0);

  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      iree_net_carrier_begin_send(&carrier_.base, 1, nullptr, &handle));
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      iree_net_carrier_begin_send(&carrier_.base, 1, &data, nullptr));
  EXPECT_EQ(carrier_.begin_send_count, 0);
}

TEST_F(CarrierTest, SendValidatesSpanListBeforeSubmission) {
  SendCompletion completion;
  iree_net_send_params_t params = {
      /*.data=*/iree_async_span_list_empty(),
      /*.flags=*/IREE_NET_SEND_FLAG_NONE,
      /*.completion_callback=*/
      {
          /*.fn=*/SendCompletion::Handle,
          /*.user_data=*/&completion,
      },
  };
  IREE_EXPECT_STATUS_IS(IREE_STATUS_INVALID_ARGUMENT,
                        iree_net_carrier_send(&carrier_.base, &params));

  params.data = {/*.values=*/nullptr, /*.count=*/1};
  IREE_EXPECT_STATUS_IS(IREE_STATUS_INVALID_ARGUMENT,
                        iree_net_carrier_send(&carrier_.base, &params));

  iree_async_span_t too_many_spans[9] = {};
  params.data = iree_async_span_list_make(too_many_spans, 9);
  IREE_EXPECT_STATUS_IS(IREE_STATUS_OUT_OF_RANGE,
                        iree_net_carrier_send(&carrier_.base, &params));

  iree_async_span_t empty_span = iree_async_span_empty();
  params.data = iree_async_span_list_make(&empty_span, 1);
  IREE_EXPECT_STATUS_IS(IREE_STATUS_INVALID_ARGUMENT,
                        iree_net_carrier_send(&carrier_.base, &params));

  uint8_t byte = 0;
  iree_async_span_t overflowing_spans[2] = {
      iree_async_span_from_ptr(&byte, IREE_HOST_SIZE_MAX),
      iree_async_span_from_ptr(&byte, 1),
  };
  params.data = iree_async_span_list_make(overflowing_spans, 2);
  IREE_EXPECT_STATUS_IS(IREE_STATUS_OUT_OF_RANGE,
                        iree_net_carrier_send(&carrier_.base, &params));
  EXPECT_EQ(carrier_.send_count, 0);
}

TEST_F(CarrierTest, AcceptedSendHasTerminalCompletion) {
  uint8_t payload[16] = {};
  iree_async_span_t span = iree_async_span_from_ptr(payload, sizeof(payload));
  SendCompletion completion;
  iree_net_send_params_t params = {
      /*.data=*/iree_async_span_list_make(&span, 1),
      /*.flags=*/IREE_NET_SEND_FLAG_NONE,
      /*.completion_callback=*/
      {
          /*.fn=*/SendCompletion::Handle,
          /*.user_data=*/&completion,
      },
  };
  IREE_ASSERT_OK(iree_net_carrier_send(&carrier_.base, &params));
  EXPECT_EQ(carrier_.send_count, 1);
  EXPECT_EQ(completion.count, 0);

  carrier_.pending_completion.fn(carrier_.pending_completion.user_data,
                                 iree_ok_status(), sizeof(payload));
  EXPECT_EQ(completion.count, 1);
  EXPECT_EQ(completion.status_code, IREE_STATUS_OK);
  EXPECT_EQ(completion.bytes_transferred, sizeof(payload));
}

TEST_F(CarrierTest, CommittedReservationHasTerminalCompletion) {
  void* data = nullptr;
  iree_net_carrier_send_handle_t handle = 0;
  IREE_ASSERT_OK(
      iree_net_carrier_begin_send(&carrier_.base, 32, &data, &handle));
  EXPECT_EQ(data, carrier_.reservation_storage);
  EXPECT_EQ(handle, 42u);

  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      iree_net_carrier_commit_send(&carrier_.base, handle, {0}));
  EXPECT_EQ(carrier_.commit_send_count, 0);

  SendCompletion completion;
  IREE_ASSERT_OK(iree_net_carrier_commit_send(
      &carrier_.base, handle,
      {/*.fn=*/SendCompletion::Handle, /*.user_data=*/&completion}));
  EXPECT_EQ(carrier_.commit_send_count, 1);
  EXPECT_EQ(completion.count, 0);

  carrier_.pending_completion.fn(carrier_.pending_completion.user_data,
                                 iree_ok_status(), carrier_.reserved_size);
  EXPECT_EQ(completion.count, 1);
  EXPECT_EQ(completion.status_code, IREE_STATUS_OK);
  EXPECT_EQ(completion.bytes_transferred, 32u);
}

TEST_F(CarrierTest, TerminalErrorStopsAdmissionAndBudget) {
  EXPECT_TRUE(iree_net_carrier_report_terminal_error(
      &carrier_.base,
      iree_make_status(IREE_STATUS_UNAVAILABLE, "connection lost")));
  EXPECT_EQ(carrier_.error_count, 1);
  EXPECT_EQ(carrier_.error_code, IREE_STATUS_UNAVAILABLE);

  iree_net_carrier_send_budget_t budget =
      iree_net_carrier_query_send_budget(&carrier_.base);
  EXPECT_EQ(budget.bytes, 0u);
  EXPECT_EQ(budget.slots, 0u);
  EXPECT_EQ(carrier_.query_budget_count, 0);

  void* data = nullptr;
  iree_net_carrier_send_handle_t handle = 0;
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_UNAVAILABLE,
      iree_net_carrier_begin_send(&carrier_.base, 16, &data, &handle));
  EXPECT_EQ(data, nullptr);
  EXPECT_EQ(handle, 0u);
  EXPECT_EQ(carrier_.begin_send_count, 0);
}

}  // namespace
}  // namespace net
}  // namespace iree
