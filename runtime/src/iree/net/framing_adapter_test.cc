// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/framing_adapter.h"

#include <cstring>
#include <memory>
#include <vector>

#include "iree/async/buffer_pool.h"
#include "iree/base/api.h"
#include "iree/net/carrier.h"
#include "iree/net/message_endpoint.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace iree {
namespace net {
namespace {

//===----------------------------------------------------------------------===//
// Test protocol: 4-byte little-endian length prefix
//===----------------------------------------------------------------------===//

static constexpr iree_host_size_t kHeaderSize = 4;

static iree_status_t TestFrameLength(void* user_data,
                                     iree_const_byte_span_t available,
                                     iree_host_size_t* out_frame_size) {
  *out_frame_size = 0;
  if (available.data_length < kHeaderSize) return iree_ok_status();
  uint32_t frame_size =
      (uint32_t)available.data[0] | ((uint32_t)available.data[1] << 8) |
      ((uint32_t)available.data[2] << 16) | ((uint32_t)available.data[3] << 24);
  if (frame_size < kHeaderSize) {
    return iree_make_status(IREE_STATUS_DATA_LOSS,
                            "frame is smaller than its header");
  }
  *out_frame_size = static_cast<iree_host_size_t>(frame_size);
  return iree_ok_status();
}

static iree_net_frame_length_callback_t TestFrameLengthCallback() {
  return {
      /*.fn=*/TestFrameLength,
      /*.user_data=*/nullptr,
      /*.max_header_size=*/kHeaderSize,
  };
}

static std::vector<uint8_t> MakeFrame(const std::string& payload) {
  uint32_t frame_size = static_cast<uint32_t>(kHeaderSize + payload.size());
  std::vector<uint8_t> frame(frame_size);
  frame[0] = frame_size & 0xFF;
  frame[1] = (frame_size >> 8) & 0xFF;
  frame[2] = (frame_size >> 16) & 0xFF;
  frame[3] = (frame_size >> 24) & 0xFF;
  memcpy(frame.data() + kHeaderSize, payload.data(), payload.size());
  return frame;
}

//===----------------------------------------------------------------------===//
// Mock carrier for testing
//===----------------------------------------------------------------------===//

struct CapturedSend {
  std::vector<std::vector<uint8_t>> span_data;
  iree_net_send_flags_t flags;
};

struct PendingSendCompletion {
  iree_net_send_completion_callback_t callback;
  iree_host_size_t bytes_transferred;
};

struct MockCarrier {
  iree_net_carrier_t base;
  std::vector<CapturedSend> sends;
  std::vector<PendingSendCompletion> pending_completions;
  std::vector<uint8_t> reservation_storage;
  iree_status_code_t next_activate_error = IREE_STATUS_OK;
  iree_status_code_t next_send_error = IREE_STATUS_OK;
  iree_host_size_t send_budget_bytes = SIZE_MAX;
  uint32_t send_budget_slots = UINT32_MAX;
  bool reservation_live = false;
  int destroy_count = 0;

  static void Destroy(iree_net_carrier_t* carrier) {
    MockCarrier* mock = reinterpret_cast<MockCarrier*>(carrier);
    ++mock->destroy_count;
    iree_net_carrier_deinitialize(carrier);
  }

  static iree_status_t Activate(iree_net_carrier_t* carrier) {
    MockCarrier* mock = reinterpret_cast<MockCarrier*>(carrier);
    if (mock->next_activate_error != IREE_STATUS_OK) {
      iree_status_code_t error = mock->next_activate_error;
      mock->next_activate_error = IREE_STATUS_OK;
      return iree_status_from_code(error);
    }
    iree_net_carrier_set_state(carrier, IREE_NET_CARRIER_STATE_ACTIVE);
    return iree_ok_status();
  }

  static void Deactivate(iree_net_carrier_t* carrier,
                         iree_net_carrier_deactivate_callback_fn_t callback,
                         void* user_data) {
    iree_net_carrier_set_state(carrier, IREE_NET_CARRIER_STATE_DEACTIVATED);
    if (callback) callback(user_data);
  }

  static iree_net_carrier_send_budget_t QuerySendBudget(
      iree_net_carrier_t* carrier) {
    MockCarrier* mock = reinterpret_cast<MockCarrier*>(carrier);
    return {mock->send_budget_bytes, mock->send_budget_slots};
  }

  static iree_status_t Send(iree_net_carrier_t* carrier,
                            const iree_net_send_params_t* params) {
    MockCarrier* mock = reinterpret_cast<MockCarrier*>(carrier);
    if (mock->next_send_error != IREE_STATUS_OK) {
      iree_status_code_t error = mock->next_send_error;
      mock->next_send_error = IREE_STATUS_OK;
      return iree_status_from_code(error);
    }
    CapturedSend captured;
    captured.flags = params->flags;
    iree_host_size_t total_length = 0;
    for (iree_host_size_t i = 0; i < params->data.count; ++i) {
      iree_async_span_t span = params->data.values[i];
      uint8_t* ptr = iree_async_span_ptr(span);
      captured.span_data.push_back(
          std::vector<uint8_t>(ptr, ptr + span.length));
      total_length += span.length;
    }
    mock->sends.push_back(std::move(captured));
    mock->pending_completions.push_back(
        {params->completion_callback, total_length});
    return iree_ok_status();
  }

  static iree_status_t BeginSend(iree_net_carrier_t* carrier,
                                 iree_host_size_t size, void** out_ptr,
                                 iree_net_carrier_send_handle_t* out_handle) {
    MockCarrier* mock = reinterpret_cast<MockCarrier*>(carrier);
    if (mock->reservation_live) {
      return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                              "mock reservation already live");
    }
    mock->reservation_storage.resize(size);
    mock->reservation_live = true;
    *out_ptr = mock->reservation_storage.data();
    *out_handle = 1;
    return iree_ok_status();
  }

  static iree_status_t CommitSend(
      iree_net_carrier_t* carrier, iree_net_carrier_send_handle_t handle,
      iree_net_send_completion_callback_t callback) {
    MockCarrier* mock = reinterpret_cast<MockCarrier*>(carrier);
    if (!mock->reservation_live || handle != 1) {
      return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                              "invalid mock reservation");
    }
    CapturedSend captured;
    captured.flags = IREE_NET_SEND_FLAG_NONE;
    captured.span_data.push_back(mock->reservation_storage);
    mock->sends.push_back(std::move(captured));
    mock->pending_completions.push_back(
        {callback, mock->reservation_storage.size()});
    mock->reservation_live = false;
    return iree_ok_status();
  }

  static void AbortSend(iree_net_carrier_t* carrier,
                        iree_net_carrier_send_handle_t handle) {
    MockCarrier* mock = reinterpret_cast<MockCarrier*>(carrier);
    IREE_ASSERT(mock->reservation_live && handle == 1);
    mock->reservation_live = false;
    mock->reservation_storage.clear();
  }

  static iree_status_t Shutdown(iree_net_carrier_t* carrier) {
    return iree_ok_status();
  }

  static const iree_net_carrier_vtable_t kVtable;

  static std::unique_ptr<MockCarrier> Create() {
    auto mock = std::make_unique<MockCarrier>();
    iree_net_carrier_initialize(&kVtable, IREE_NET_CARRIER_CAPABILITY_RELIABLE,
                                8, iree_allocator_system(), &mock->base);
    return mock;
  }

  // Injects recv data into the adapter's receive handler.
  iree_status_t InjectRecv(const std::vector<uint8_t>& data,
                           iree_async_buffer_lease_t* lease) {
    if (iree_net_carrier_has_terminal_error(&base)) {
      return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                              "carrier receive path is terminal");
    }
    iree_async_span_t span =
        lease ? lease->span
              : iree_async_span_from_ptr(const_cast<uint8_t*>(data.data()),
                                         data.size());
    span.length = data.size();
    iree_status_t status =
        base.handlers.on_receive(base.handlers.user_data, span, lease);
    if (!iree_status_is_ok(status)) {
      iree_net_carrier_report_terminal_error(&base, status);
    }
    return iree_ok_status();
  }

  void InjectError(iree_status_t status) {
    iree_net_carrier_report_terminal_error(&base, status);
  }

  void CompleteNextSend(iree_status_t status) {
    ASSERT_FALSE(pending_completions.empty());
    PendingSendCompletion completion = pending_completions.front();
    pending_completions.erase(pending_completions.begin());
    completion.callback.fn(completion.callback.user_data, status,
                           completion.bytes_transferred);
  }
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

//===----------------------------------------------------------------------===//
// Mock lease for testing
//===----------------------------------------------------------------------===//

struct MockLease {
  std::vector<uint8_t> data;
  iree_async_buffer_lease_t lease;
  int release_count = 0;

  static void ReleaseCallback(void* user_data,
                              iree_async_buffer_index_t buffer_index) {
    MockLease* self = static_cast<MockLease*>(user_data);
    self->release_count++;
  }

  explicit MockLease(const std::vector<uint8_t>& buffer_data)
      : data(buffer_data), release_count(0) {
    memset(&lease, 0, sizeof(lease));
    lease.span = iree_async_span_from_ptr(data.data(), data.size());
    lease.release.fn = ReleaseCallback;
    lease.release.user_data = this;
    lease.buffer_index = 0;
  }

  MockLease(std::initializer_list<std::vector<uint8_t>> frame_list)
      : release_count(0) {
    for (const auto& frame : frame_list) {
      data.insert(data.end(), frame.begin(), frame.end());
    }
    memset(&lease, 0, sizeof(lease));
    lease.span = iree_async_span_from_ptr(data.data(), data.size());
    lease.release.fn = ReleaseCallback;
    lease.release.user_data = this;
    lease.buffer_index = 0;
  }
};

//===----------------------------------------------------------------------===//
// Test context for tracking received messages
//===----------------------------------------------------------------------===//

struct ReceivedMessage {
  std::vector<uint8_t> data;
  bool had_lease;
};

struct TestContext {
  std::vector<ReceivedMessage> messages;
  std::vector<iree_status_code_t> errors;
  iree_status_code_t next_error = IREE_STATUS_OK;
  bool deactivated = false;

  static iree_status_t OnMessage(void* user_data,
                                 iree_const_byte_span_t message,
                                 iree_async_buffer_lease_t* lease) {
    TestContext* ctx = static_cast<TestContext*>(user_data);
    if (ctx->next_error != IREE_STATUS_OK) {
      iree_status_code_t error = ctx->next_error;
      ctx->next_error = IREE_STATUS_OK;
      return iree_make_status(error, "injected error");
    }
    ReceivedMessage received;
    received.data.assign(message.data, message.data + message.data_length);
    received.had_lease = (lease != nullptr);
    ctx->messages.push_back(std::move(received));
    return iree_ok_status();
  }

  static void OnError(void* user_data, iree_status_t status) {
    TestContext* ctx = static_cast<TestContext*>(user_data);
    ctx->errors.push_back(iree_status_code(status));
    iree_status_free(status);
  }

  static void OnDeactivated(void* user_data) {
    TestContext* ctx = static_cast<TestContext*>(user_data);
    ctx->deactivated = true;
  }

  iree_net_message_endpoint_callbacks_t MakeCallbacks() {
    return {OnMessage, OnError, this};
  }
};

struct SendCompletion {
  int count = 0;
  iree_status_code_t status_code = IREE_STATUS_UNKNOWN;

  static void Complete(void* user_data, iree_status_t status,
                       iree_host_size_t bytes_transferred) {
    (void)bytes_transferred;
    SendCompletion* completion = static_cast<SendCompletion*>(user_data);
    ++completion->count;
    completion->status_code = iree_status_code(status);
    iree_status_free(status);
  }

  iree_net_send_completion_callback_t callback() { return {Complete, this}; }
};

struct MovedLeaseCapture {
  iree_const_byte_span_t message = {nullptr, 0};
  iree_async_buffer_lease_t lease = {};

  static iree_status_t OnMessage(void* user_data,
                                 iree_const_byte_span_t message,
                                 iree_async_buffer_lease_t* lease) {
    auto* capture = static_cast<MovedLeaseCapture*>(user_data);
    capture->message = message;
    capture->lease = *lease;
    memset(lease, 0, sizeof(*lease));
    return iree_ok_status();
  }

  static void OnError(void* user_data, iree_status_t status) {
    (void)user_data;
    iree_status_free(status);
  }
};

//===----------------------------------------------------------------------===//
// Test constants
//===----------------------------------------------------------------------===//

static constexpr iree_host_size_t kMaxFrameSize = 1024;
//===----------------------------------------------------------------------===//
// Test fixture
//===----------------------------------------------------------------------===//

class FramingAdapterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    mock_carrier_ = MockCarrier::Create();
    iree_net_frame_length_callback_t frame_length = TestFrameLengthCallback();
    IREE_ASSERT_OK(iree_net_framing_adapter_allocate(
        &mock_carrier_->base, frame_length, kMaxFrameSize,
        iree_allocator_system(), &adapter_));
    endpoint_ = iree_net_framing_adapter_as_endpoint(adapter_);
  }

  void TearDown() override {
    if (adapter_) {
      if (iree_net_carrier_state(&mock_carrier_->base) ==
          IREE_NET_CARRIER_STATE_ACTIVE) {
        IREE_ASSERT_OK(iree_net_message_endpoint_deactivate(
            endpoint_, /*callback=*/nullptr, /*user_data=*/nullptr));
      }
      iree_net_framing_adapter_free(adapter_);
      adapter_ = nullptr;
    }
  }

  void ActivateWithCallbacks() {
    iree_net_message_endpoint_set_callbacks(endpoint_, ctx_.MakeCallbacks());
    IREE_ASSERT_OK(iree_net_message_endpoint_activate(endpoint_));
  }

  // Injects recv data with a mock lease (simulates a carrier delivering data).
  iree_status_t InjectRecv(const std::vector<uint8_t>& data) {
    MockLease lease(data);
    return mock_carrier_->InjectRecv(data, &lease.lease);
  }

  // Injects receive data without a transferable backing lease.
  iree_status_t InjectBorrowed(const std::vector<uint8_t>& data) {
    return mock_carrier_->InjectRecv(data, nullptr);
  }

  iree_net_framing_adapter_t* adapter_ = nullptr;
  iree_net_message_endpoint_t endpoint_;
  std::unique_ptr<MockCarrier> mock_carrier_;
  TestContext ctx_;
};

//===----------------------------------------------------------------------===//
// Allocation validation tests
//===----------------------------------------------------------------------===//

TEST_F(FramingAdapterTest, AllocateAndFree) { EXPECT_NE(adapter_, nullptr); }

TEST_F(FramingAdapterTest, AllocateRequiresCarrier) {
  iree_net_framing_adapter_t* adapter = nullptr;
  iree_net_frame_length_callback_t frame_length = TestFrameLengthCallback();
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      iree_net_framing_adapter_allocate(nullptr, frame_length, kMaxFrameSize,
                                        iree_allocator_system(), &adapter));
}

TEST_F(FramingAdapterTest, AllocateRequiresFrameLengthFn) {
  auto carrier = MockCarrier::Create();
  iree_net_framing_adapter_t* adapter = nullptr;
  iree_net_frame_length_callback_t frame_length = {
      /*.fn=*/nullptr,
      /*.user_data=*/nullptr,
      /*.max_header_size=*/kHeaderSize,
  };
  IREE_EXPECT_STATUS_IS(IREE_STATUS_INVALID_ARGUMENT,
                        iree_net_framing_adapter_allocate(
                            &carrier->base, frame_length, kMaxFrameSize,
                            iree_allocator_system(), &adapter));
}

TEST_F(FramingAdapterTest, AllocateRequiresNonZeroMaxFrameSize) {
  auto carrier = MockCarrier::Create();
  iree_net_framing_adapter_t* adapter = nullptr;
  iree_net_frame_length_callback_t frame_length = TestFrameLengthCallback();
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      iree_net_framing_adapter_allocate(&carrier->base, frame_length, 0,
                                        iree_allocator_system(), &adapter));
}

TEST_F(FramingAdapterTest, AllocateRejectsActivatedCarrier) {
  auto carrier = MockCarrier::Create();
  iree_net_carrier_set_state(&carrier->base, IREE_NET_CARRIER_STATE_ACTIVE);
  iree_net_framing_adapter_t* adapter = nullptr;
  iree_net_frame_length_callback_t frame_length = TestFrameLengthCallback();
  IREE_EXPECT_STATUS_IS(IREE_STATUS_FAILED_PRECONDITION,
                        iree_net_framing_adapter_allocate(
                            &carrier->base, frame_length, kMaxFrameSize,
                            iree_allocator_system(), &adapter));
}

TEST_F(FramingAdapterTest, FailedAllocationRetainsCarrierOwnership) {
  auto carrier = MockCarrier::Create();
  iree_net_framing_adapter_t* adapter = nullptr;
  iree_net_frame_length_callback_t frame_length = TestFrameLengthCallback();
  IREE_EXPECT_STATUS_IS(IREE_STATUS_INVALID_ARGUMENT,
                        iree_net_framing_adapter_allocate(
                            &carrier->base, frame_length, kMaxFrameSize,
                            iree_allocator_null(), &adapter));
  EXPECT_EQ(adapter, nullptr);
  EXPECT_EQ(carrier->destroy_count, 0);

  iree_net_carrier_release(&carrier->base);
  EXPECT_EQ(carrier->destroy_count, 1);
}

//===----------------------------------------------------------------------===//
// Activation tests
//===----------------------------------------------------------------------===//

TEST_F(FramingAdapterTest, ActivateRequiresCallbacks) {
  IREE_EXPECT_STATUS_IS(IREE_STATUS_FAILED_PRECONDITION,
                        iree_net_message_endpoint_activate(endpoint_));
}

TEST_F(FramingAdapterTest, ActivateRequiresErrorCallback) {
  iree_net_message_endpoint_set_callbacks(
      endpoint_, {TestContext::OnMessage, nullptr, &ctx_});
  IREE_EXPECT_STATUS_IS(IREE_STATUS_FAILED_PRECONDITION,
                        iree_net_message_endpoint_activate(endpoint_));
}

TEST_F(FramingAdapterTest, DoubleActivateFails) {
  ActivateWithCallbacks();
  IREE_EXPECT_STATUS_IS(IREE_STATUS_FAILED_PRECONDITION,
                        iree_net_message_endpoint_activate(endpoint_));
}

TEST_F(FramingAdapterTest, FailedActivationRollsBackForRetry) {
  iree_net_message_endpoint_set_callbacks(endpoint_, ctx_.MakeCallbacks());
  mock_carrier_->next_activate_error = IREE_STATUS_RESOURCE_EXHAUSTED;

  IREE_EXPECT_STATUS_IS(IREE_STATUS_RESOURCE_EXHAUSTED,
                        iree_net_message_endpoint_activate(endpoint_));
  EXPECT_EQ(iree_net_carrier_state(&mock_carrier_->base),
            IREE_NET_CARRIER_STATE_CREATED);

  IREE_ASSERT_OK(iree_net_message_endpoint_activate(endpoint_));
  EXPECT_EQ(iree_net_carrier_state(&mock_carrier_->base),
            IREE_NET_CARRIER_STATE_ACTIVE);
}

//===----------------------------------------------------------------------===//
// Receive path: zero-copy (frame fits in single buffer)
//===----------------------------------------------------------------------===//

TEST_F(FramingAdapterTest, SingleCompleteFrame) {
  ActivateWithCallbacks();

  auto frame = MakeFrame("Hello");
  IREE_ASSERT_OK(InjectRecv(frame));

  ASSERT_EQ(ctx_.messages.size(), 1u);
  EXPECT_EQ(ctx_.messages[0].data, frame);
  EXPECT_TRUE(ctx_.messages[0].had_lease);
}

TEST_F(FramingAdapterTest, BorrowedCompleteFrameGetsOwnedLease) {
  ActivateWithCallbacks();

  auto frame = MakeFrame("Borrowed frame");
  IREE_ASSERT_OK(InjectBorrowed(frame));

  ASSERT_EQ(ctx_.messages.size(), 1u);
  EXPECT_EQ(ctx_.messages[0].data, frame);
  EXPECT_TRUE(ctx_.messages[0].had_lease);
}

TEST_F(FramingAdapterTest, MultipleFramesInOneBuffer) {
  ActivateWithCallbacks();

  auto frame1 = MakeFrame("First");
  auto frame2 = MakeFrame("Second");
  auto frame3 = MakeFrame("Third");

  std::vector<uint8_t> buffer;
  buffer.insert(buffer.end(), frame1.begin(), frame1.end());
  buffer.insert(buffer.end(), frame2.begin(), frame2.end());
  buffer.insert(buffer.end(), frame3.begin(), frame3.end());

  MockLease lease(buffer);
  IREE_ASSERT_OK(mock_carrier_->InjectRecv(buffer, &lease.lease));

  ASSERT_EQ(ctx_.messages.size(), 3u);
  EXPECT_EQ(ctx_.messages[0].data, frame1);
  EXPECT_EQ(ctx_.messages[1].data, frame2);
  EXPECT_EQ(ctx_.messages[2].data, frame3);
  for (int i = 0; i < 3; ++i) {
    EXPECT_TRUE(ctx_.messages[i].had_lease);
  }
}

TEST_F(FramingAdapterTest, EmptyPayloadFrame) {
  ActivateWithCallbacks();

  auto frame = MakeFrame("");
  ASSERT_EQ(frame.size(), kHeaderSize);

  IREE_ASSERT_OK(InjectRecv(frame));

  ASSERT_EQ(ctx_.messages.size(), 1u);
  EXPECT_EQ(ctx_.messages[0].data, frame);
  EXPECT_TRUE(ctx_.messages[0].had_lease);
}

TEST_F(FramingAdapterTest, ZeroCopyPathDeliversReceiveLease) {
  ActivateWithCallbacks();

  auto frame = MakeFrame("Zero-copy frame");
  IREE_ASSERT_OK(InjectRecv(frame));

  ASSERT_EQ(ctx_.messages.size(), 1u);
  EXPECT_TRUE(ctx_.messages[0].had_lease);
}

//===----------------------------------------------------------------------===//
// Receive path: copy path (frame spans multiple buffers)
//===----------------------------------------------------------------------===//

TEST_F(FramingAdapterTest, FrameSpansTwoBuffers) {
  ActivateWithCallbacks();

  auto frame = MakeFrame("Split across buffers");
  iree_host_size_t split_point = frame.size() / 2;

  std::vector<uint8_t> first_half(frame.begin(), frame.begin() + split_point);
  IREE_ASSERT_OK(InjectRecv(first_half));
  EXPECT_EQ(ctx_.messages.size(), 0u);

  std::vector<uint8_t> second_half(frame.begin() + split_point, frame.end());
  IREE_ASSERT_OK(InjectRecv(second_half));

  ASSERT_EQ(ctx_.messages.size(), 1u);
  EXPECT_EQ(ctx_.messages[0].data, frame);
  EXPECT_TRUE(ctx_.messages[0].had_lease);
}

TEST_F(FramingAdapterTest, BorrowedFragmentsGetOwnedLease) {
  ActivateWithCallbacks();

  auto frame = MakeFrame("Borrowed fragments");
  iree_host_size_t split_point = frame.size() / 2;
  std::vector<uint8_t> first_half(frame.begin(), frame.begin() + split_point);
  std::vector<uint8_t> second_half(frame.begin() + split_point, frame.end());

  IREE_ASSERT_OK(InjectBorrowed(first_half));
  IREE_ASSERT_OK(InjectBorrowed(second_half));

  ASSERT_EQ(ctx_.messages.size(), 1u);
  EXPECT_EQ(ctx_.messages[0].data, frame);
  EXPECT_TRUE(ctx_.messages[0].had_lease);
}

TEST_F(FramingAdapterTest, PartialHeaderThenRest) {
  ActivateWithCallbacks();

  auto frame = MakeFrame("test");

  // Just 2 bytes of the 4-byte header.
  std::vector<uint8_t> partial_header(frame.begin(), frame.begin() + 2);
  IREE_ASSERT_OK(InjectRecv(partial_header));
  EXPECT_EQ(ctx_.messages.size(), 0u);

  std::vector<uint8_t> rest(frame.begin() + 2, frame.end());
  IREE_ASSERT_OK(InjectRecv(rest));

  ASSERT_EQ(ctx_.messages.size(), 1u);
  EXPECT_EQ(ctx_.messages[0].data, frame);
}

TEST_F(FramingAdapterTest, ByteByByte) {
  ActivateWithCallbacks();

  auto frame = MakeFrame("byte by byte");
  for (size_t i = 0; i < frame.size(); ++i) {
    std::vector<uint8_t> single_byte = {frame[i]};
    IREE_ASSERT_OK(InjectRecv(single_byte));
  }

  ASSERT_EQ(ctx_.messages.size(), 1u);
  EXPECT_EQ(ctx_.messages[0].data, frame);
  EXPECT_TRUE(ctx_.messages[0].had_lease);
}

TEST_F(FramingAdapterTest, CopyPathAlwaysDeliversNonNullLease) {
  ActivateWithCallbacks();

  // Split at header boundary to force copy path.
  auto frame = MakeFrame("Must have lease");

  std::vector<uint8_t> header(frame.begin(), frame.begin() + kHeaderSize);
  IREE_ASSERT_OK(InjectRecv(header));

  std::vector<uint8_t> payload(frame.begin() + kHeaderSize, frame.end());
  IREE_ASSERT_OK(InjectRecv(payload));

  ASSERT_EQ(ctx_.messages.size(), 1u);
  EXPECT_TRUE(ctx_.messages[0].had_lease);
}

TEST_F(FramingAdapterTest, CopyPathLeaseCanMoveBeyondCallback) {
  MovedLeaseCapture capture;
  iree_net_message_endpoint_set_callbacks(
      endpoint_,
      {MovedLeaseCapture::OnMessage, MovedLeaseCapture::OnError, &capture});
  IREE_ASSERT_OK(iree_net_message_endpoint_activate(endpoint_));

  auto frame = MakeFrame("Move host-backed lease");
  iree_host_size_t split_point = frame.size() / 2;
  std::vector<uint8_t> first_half(frame.begin(), frame.begin() + split_point);
  std::vector<uint8_t> second_half(frame.begin() + split_point, frame.end());
  IREE_ASSERT_OK(InjectRecv(first_half));
  IREE_ASSERT_OK(InjectRecv(second_half));

  ASSERT_NE(capture.lease.release.fn, nullptr);
  EXPECT_EQ(
      std::vector<uint8_t>(capture.message.data,
                           capture.message.data + capture.message.data_length),
      frame);
  iree_async_buffer_lease_release(&capture.lease);
}

TEST_F(FramingAdapterTest, CopyPathReleasesHostLease) {
  ActivateWithCallbacks();

  // Force copy path by splitting frame across two recvs.
  auto frame = MakeFrame("Host buffer lifecycle");
  iree_host_size_t split_point = frame.size() / 2;

  std::vector<uint8_t> first_half(frame.begin(), frame.begin() + split_point);
  IREE_ASSERT_OK(InjectRecv(first_half));

  std::vector<uint8_t> second_half(frame.begin() + split_point, frame.end());
  IREE_ASSERT_OK(InjectRecv(second_half));

  ASSERT_EQ(ctx_.messages.size(), 1u);
  EXPECT_TRUE(ctx_.messages[0].had_lease);
}

TEST_F(FramingAdapterTest, MultipleCopyPathFramesReleaseHostLeases) {
  ActivateWithCallbacks();

  // Send several frames, each split across two recvs.
  for (int i = 0; i < 8; ++i) {
    auto frame = MakeFrame("Frame " + std::to_string(i));
    iree_host_size_t split_point = frame.size() / 2;

    std::vector<uint8_t> first_half(frame.begin(), frame.begin() + split_point);
    IREE_ASSERT_OK(InjectRecv(first_half));

    std::vector<uint8_t> second_half(frame.begin() + split_point, frame.end());
    IREE_ASSERT_OK(InjectRecv(second_half));
  }

  // All host-backed leases are released after synchronous delivery.
  ASSERT_EQ(ctx_.messages.size(), 8u);
}

//===----------------------------------------------------------------------===//
// Receive path: mixed zero-copy and copy path
//===----------------------------------------------------------------------===//

TEST_F(FramingAdapterTest, CompleteFrameThenPartial) {
  ActivateWithCallbacks();

  auto frame1 = MakeFrame("Complete");
  auto frame2 = MakeFrame("Partial frame here");
  iree_host_size_t partial_bytes = frame2.size() / 2;

  // Buffer contains complete frame1 + partial frame2.
  std::vector<uint8_t> buffer;
  buffer.insert(buffer.end(), frame1.begin(), frame1.end());
  buffer.insert(buffer.end(), frame2.begin(), frame2.begin() + partial_bytes);

  MockLease lease(buffer);
  IREE_ASSERT_OK(mock_carrier_->InjectRecv(buffer, &lease.lease));

  ASSERT_EQ(ctx_.messages.size(), 1u);
  EXPECT_EQ(ctx_.messages[0].data, frame1);

  // Finish frame2.
  std::vector<uint8_t> rest(frame2.begin() + partial_bytes, frame2.end());
  IREE_ASSERT_OK(InjectRecv(rest));

  ASSERT_EQ(ctx_.messages.size(), 2u);
  EXPECT_EQ(ctx_.messages[1].data, frame2);
}

TEST_F(FramingAdapterTest, BufferedCompletionThenZeroCopy) {
  ActivateWithCallbacks();

  auto frame1 = MakeFrame("First");
  auto frame2 = MakeFrame("Second");

  // Partial frame1.
  iree_host_size_t split_point = frame1.size() / 2;
  std::vector<uint8_t> partial(frame1.begin(), frame1.begin() + split_point);
  IREE_ASSERT_OK(InjectRecv(partial));
  EXPECT_EQ(ctx_.messages.size(), 0u);

  // Rest of frame1 + complete frame2 in same buffer.
  std::vector<uint8_t> buffer;
  buffer.insert(buffer.end(), frame1.begin() + split_point, frame1.end());
  buffer.insert(buffer.end(), frame2.begin(), frame2.end());

  MockLease lease(buffer);
  IREE_ASSERT_OK(mock_carrier_->InjectRecv(buffer, &lease.lease));

  ASSERT_EQ(ctx_.messages.size(), 2u);
  EXPECT_EQ(ctx_.messages[0].data, frame1);
  EXPECT_EQ(ctx_.messages[1].data, frame2);
  EXPECT_TRUE(ctx_.messages[0].had_lease);
  EXPECT_TRUE(ctx_.messages[1].had_lease);
}

//===----------------------------------------------------------------------===//
// Send path
//===----------------------------------------------------------------------===//

TEST_F(FramingAdapterTest, SendForwardsToCarrier) {
  ActivateWithCallbacks();

  std::vector<uint8_t> data = {0x01, 0x02, 0x03, 0x04};
  iree_async_span_t span = iree_async_span_from_ptr(data.data(), data.size());
  SendCompletion completion;
  iree_net_message_endpoint_send_params_t params;
  params.data = iree_async_span_list_make(&span, 1);
  params.completion_callback = completion.callback();

  IREE_ASSERT_OK(iree_net_message_endpoint_send(endpoint_, &params));
  EXPECT_EQ(completion.count, 0);

  ASSERT_EQ(mock_carrier_->sends.size(), 1u);
  ASSERT_EQ(mock_carrier_->sends[0].span_data.size(), 1u);
  EXPECT_EQ(mock_carrier_->sends[0].span_data[0], data);
  EXPECT_EQ(mock_carrier_->sends[0].flags, IREE_NET_SEND_FLAG_NONE);
  mock_carrier_->CompleteNextSend(iree_ok_status());
  EXPECT_EQ(completion.count, 1);
  EXPECT_EQ(completion.status_code, IREE_STATUS_OK);
}

TEST_F(FramingAdapterTest, SendMultipleSpans) {
  ActivateWithCallbacks();

  std::vector<uint8_t> data1 = {0xAA, 0xBB};
  std::vector<uint8_t> data2 = {0xCC, 0xDD, 0xEE};
  iree_async_span_t spans[2] = {
      iree_async_span_from_ptr(data1.data(), data1.size()),
      iree_async_span_from_ptr(data2.data(), data2.size()),
  };
  SendCompletion completion;
  iree_net_message_endpoint_send_params_t params;
  params.data = iree_async_span_list_make(spans, 2);
  params.completion_callback = completion.callback();

  IREE_ASSERT_OK(iree_net_message_endpoint_send(endpoint_, &params));
  EXPECT_EQ(completion.count, 0);

  ASSERT_EQ(mock_carrier_->sends.size(), 1u);
  ASSERT_EQ(mock_carrier_->sends[0].span_data.size(), 2u);
  EXPECT_EQ(mock_carrier_->sends[0].span_data[0], data1);
  EXPECT_EQ(mock_carrier_->sends[0].span_data[1], data2);
  mock_carrier_->CompleteNextSend(iree_ok_status());
  EXPECT_EQ(completion.count, 1);
}

TEST_F(FramingAdapterTest, ReservedSendForwardsTerminalCompletion) {
  ActivateWithCallbacks();

  void* data = nullptr;
  iree_net_carrier_send_handle_t handle = 0;
  IREE_ASSERT_OK(
      iree_net_message_endpoint_begin_send(endpoint_, 4, &data, &handle));
  const uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};
  memcpy(data, payload, sizeof(payload));

  SendCompletion completion;
  IREE_ASSERT_OK(iree_net_message_endpoint_commit_send(endpoint_, handle,
                                                       completion.callback()));
  EXPECT_EQ(completion.count, 0);
  ASSERT_EQ(mock_carrier_->sends.size(), 1u);
  ASSERT_EQ(mock_carrier_->sends[0].span_data.size(), 1u);
  EXPECT_EQ(mock_carrier_->sends[0].span_data[0],
            std::vector<uint8_t>(payload, payload + sizeof(payload)));

  mock_carrier_->CompleteNextSend(iree_ok_status());
  EXPECT_EQ(completion.count, 1);
  EXPECT_EQ(completion.status_code, IREE_STATUS_OK);
}

TEST_F(FramingAdapterTest, SendCarrierError) {
  ActivateWithCallbacks();

  mock_carrier_->next_send_error = IREE_STATUS_RESOURCE_EXHAUSTED;

  std::vector<uint8_t> data = {0x01};
  iree_async_span_t span = iree_async_span_from_ptr(data.data(), data.size());
  SendCompletion completion;
  iree_net_message_endpoint_send_params_t params;
  params.data = iree_async_span_list_make(&span, 1);
  params.completion_callback = completion.callback();

  IREE_EXPECT_STATUS_IS(IREE_STATUS_RESOURCE_EXHAUSTED,
                        iree_net_message_endpoint_send(endpoint_, &params));
  EXPECT_EQ(completion.count, 0);
}

TEST_F(FramingAdapterTest, SendRequiresCompletionCallback) {
  ActivateWithCallbacks();

  std::vector<uint8_t> data = {0x01};
  iree_async_span_t span = iree_async_span_from_ptr(data.data(), data.size());
  iree_net_message_endpoint_send_params_t params = {
      /*.data=*/iree_async_span_list_make(&span, 1),
      /*.completion_callback=*/{nullptr, nullptr},
  };

  IREE_EXPECT_STATUS_IS(IREE_STATUS_INVALID_ARGUMENT,
                        iree_net_message_endpoint_send(endpoint_, &params));
  EXPECT_TRUE(mock_carrier_->sends.empty());
}

TEST_F(FramingAdapterTest, SendRequiresNonEmptyData) {
  ActivateWithCallbacks();

  SendCompletion completion;
  iree_net_message_endpoint_send_params_t params = {
      /*.data=*/iree_async_span_list_empty(),
      /*.completion_callback=*/completion.callback(),
  };
  IREE_EXPECT_STATUS_IS(IREE_STATUS_INVALID_ARGUMENT,
                        iree_net_message_endpoint_send(endpoint_, &params));

  iree_async_span_t empty_span = iree_async_span_empty();
  params.data = iree_async_span_list_make(&empty_span, 1);
  IREE_EXPECT_STATUS_IS(IREE_STATUS_INVALID_ARGUMENT,
                        iree_net_message_endpoint_send(endpoint_, &params));

  EXPECT_TRUE(mock_carrier_->sends.empty());
  EXPECT_EQ(completion.count, 0);
}

TEST_F(FramingAdapterTest, SendRejectsPayloadLengthOverflow) {
  ActivateWithCallbacks();

  uint8_t byte = 0;
  iree_async_span_t spans[2] = {
      iree_async_span_from_ptr(&byte, IREE_HOST_SIZE_MAX),
      iree_async_span_from_ptr(&byte, 1),
  };
  SendCompletion completion;
  iree_net_message_endpoint_send_params_t params = {
      /*.data=*/iree_async_span_list_make(spans, 2),
      /*.completion_callback=*/completion.callback(),
  };
  IREE_EXPECT_STATUS_IS(IREE_STATUS_OUT_OF_RANGE,
                        iree_net_message_endpoint_send(endpoint_, &params));

  EXPECT_TRUE(mock_carrier_->sends.empty());
  EXPECT_EQ(completion.count, 0);
}

TEST_F(FramingAdapterTest, QuerySendBudgetDelegatesToCarrier) {
  mock_carrier_->send_budget_bytes = 4096;
  mock_carrier_->send_budget_slots = 16;

  iree_net_carrier_send_budget_t budget =
      iree_net_message_endpoint_query_send_budget(endpoint_);

  EXPECT_EQ(budget.bytes, 4096u);
  EXPECT_EQ(budget.slots, 16u);
}

//===----------------------------------------------------------------------===//
// Deactivation
//===----------------------------------------------------------------------===//

TEST_F(FramingAdapterTest, DeactivationCallbackFires) {
  ActivateWithCallbacks();

  IREE_ASSERT_OK(iree_net_message_endpoint_deactivate(
      endpoint_, TestContext::OnDeactivated, &ctx_));

  EXPECT_TRUE(ctx_.deactivated);
}

TEST_F(FramingAdapterTest, DeactivationWithNullCallback) {
  ActivateWithCallbacks();

  IREE_ASSERT_OK(
      iree_net_message_endpoint_deactivate(endpoint_, nullptr, nullptr));

  // Should not crash.
}

//===----------------------------------------------------------------------===//
// Error propagation
//===----------------------------------------------------------------------===//

TEST_F(FramingAdapterTest, HandlerErrorPropagates) {
  ActivateWithCallbacks();

  ctx_.next_error = IREE_STATUS_INTERNAL;

  auto frame = MakeFrame("error");
  IREE_ASSERT_OK(InjectRecv(frame));

  EXPECT_EQ(ctx_.messages.size(), 0u);
  ASSERT_EQ(ctx_.errors.size(), 1u);
  EXPECT_EQ(ctx_.errors[0], IREE_STATUS_INTERNAL);
}

TEST_F(FramingAdapterTest, MalformedFrameReportsTerminalDataLoss) {
  ActivateWithCallbacks();

  std::vector<uint8_t> malformed_header(kHeaderSize, 0);
  IREE_ASSERT_OK(InjectRecv(malformed_header));

  EXPECT_TRUE(ctx_.messages.empty());
  ASSERT_EQ(ctx_.errors.size(), 1u);
  EXPECT_EQ(ctx_.errors[0], IREE_STATUS_DATA_LOSS);
  EXPECT_TRUE(iree_net_carrier_has_terminal_error(&mock_carrier_->base));
}

TEST_F(FramingAdapterTest, OrderlyCarrierEofReportsTerminalUnavailability) {
  ActivateWithCallbacks();

  IREE_ASSERT_OK(InjectBorrowed({}));

  EXPECT_TRUE(ctx_.messages.empty());
  ASSERT_EQ(ctx_.errors.size(), 1u);
  EXPECT_EQ(ctx_.errors[0], IREE_STATUS_UNAVAILABLE);
  EXPECT_TRUE(iree_net_carrier_has_terminal_error(&mock_carrier_->base));
}

TEST_F(FramingAdapterTest, CarrierEofRejectsPartialFrame) {
  ActivateWithCallbacks();

  std::vector<uint8_t> partial_header = {8, 0};
  IREE_ASSERT_OK(InjectRecv(partial_header));
  IREE_ASSERT_OK(InjectBorrowed({}));

  EXPECT_TRUE(ctx_.messages.empty());
  ASSERT_EQ(ctx_.errors.size(), 1u);
  EXPECT_EQ(ctx_.errors[0], IREE_STATUS_DATA_LOSS);
  EXPECT_TRUE(iree_net_carrier_has_terminal_error(&mock_carrier_->base));
}

TEST_F(FramingAdapterTest, HandlerErrorStopsProcessingMultipleFrames) {
  ActivateWithCallbacks();

  auto frame1 = MakeFrame("First");
  auto frame2 = MakeFrame("Second");

  std::vector<uint8_t> buffer;
  buffer.insert(buffer.end(), frame1.begin(), frame1.end());
  buffer.insert(buffer.end(), frame2.begin(), frame2.end());

  ctx_.next_error = IREE_STATUS_CANCELLED;

  MockLease lease(buffer);
  IREE_ASSERT_OK(mock_carrier_->InjectRecv(buffer, &lease.lease));

  // Error on first frame stops processing; second frame never delivered.
  EXPECT_EQ(ctx_.messages.size(), 0u);
  ASSERT_EQ(ctx_.errors.size(), 1u);
  EXPECT_EQ(ctx_.errors[0], IREE_STATUS_CANCELLED);
}

TEST_F(FramingAdapterTest, CarrierReportsFirstTerminalErrorExactlyOnce) {
  ActivateWithCallbacks();

  mock_carrier_->InjectError(
      iree_make_status(IREE_STATUS_UNAVAILABLE, "peer disconnected"));
  mock_carrier_->InjectError(
      iree_make_status(IREE_STATUS_DATA_LOSS, "duplicate transport error"));

  ASSERT_EQ(ctx_.errors.size(), 1u);
  EXPECT_EQ(ctx_.errors[0], IREE_STATUS_UNAVAILABLE);
  EXPECT_TRUE(iree_net_carrier_has_terminal_error(&mock_carrier_->base));
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_UNAVAILABLE,
      iree_net_carrier_clone_terminal_error(&mock_carrier_->base));
}

//===----------------------------------------------------------------------===//
// Callback swap (protocol handoff)
//===----------------------------------------------------------------------===//

struct SecondHandler {
  std::vector<ReceivedMessage> messages;

  static iree_status_t OnMessage(void* user_data,
                                 iree_const_byte_span_t message,
                                 iree_async_buffer_lease_t* lease) {
    SecondHandler* handler = static_cast<SecondHandler*>(user_data);
    ReceivedMessage received;
    received.data.assign(message.data, message.data + message.data_length);
    received.had_lease = (lease != nullptr);
    handler->messages.push_back(std::move(received));
    return iree_ok_status();
  }

  static void OnError(void* user_data, iree_status_t status) {
    iree_status_free(status);
  }

  iree_net_message_endpoint_callbacks_t MakeCallbacks() {
    return {OnMessage, OnError, this};
  }
};

TEST_F(FramingAdapterTest, CallbackSwapRedirectsMessages) {
  ActivateWithCallbacks();

  auto frame1 = MakeFrame("Before swap");
  IREE_ASSERT_OK(InjectRecv(frame1));
  ASSERT_EQ(ctx_.messages.size(), 1u);

  // Swap to a different handler.
  SecondHandler second_handler;
  iree_net_message_endpoint_set_callbacks(endpoint_,
                                          second_handler.MakeCallbacks());

  auto frame2 = MakeFrame("After swap");
  IREE_ASSERT_OK(InjectRecv(frame2));

  // First handler got frame1 only.
  EXPECT_EQ(ctx_.messages.size(), 1u);
  EXPECT_EQ(ctx_.messages[0].data, frame1);

  // Second handler got frame2 only.
  ASSERT_EQ(second_handler.messages.size(), 1u);
  EXPECT_EQ(second_handler.messages[0].data, frame2);
}

TEST_F(FramingAdapterTest, CallbackSwapMidFragment) {
  ActivateWithCallbacks();

  // Start a fragmented frame under the first handler.
  auto frame = MakeFrame("Fragment across handlers");
  iree_host_size_t split_point = frame.size() / 2;

  std::vector<uint8_t> first_half(frame.begin(), frame.begin() + split_point);
  IREE_ASSERT_OK(InjectRecv(first_half));
  EXPECT_EQ(ctx_.messages.size(), 0u);

  // Swap handler before the frame is complete.
  SecondHandler second_handler;
  iree_net_message_endpoint_set_callbacks(endpoint_,
                                          second_handler.MakeCallbacks());

  // Complete the frame under the second handler.
  std::vector<uint8_t> second_half(frame.begin() + split_point, frame.end());
  IREE_ASSERT_OK(InjectRecv(second_half));

  // First handler never got the frame.
  EXPECT_EQ(ctx_.messages.size(), 0u);

  // Second handler got the completed frame.
  ASSERT_EQ(second_handler.messages.size(), 1u);
  EXPECT_EQ(second_handler.messages[0].data, frame);
}

//===----------------------------------------------------------------------===//
// Frame size limits
//===----------------------------------------------------------------------===//

TEST_F(FramingAdapterTest, OversizedFrameReturnsError) {
  ActivateWithCallbacks();

  // Frame larger than max_frame_size (header-encoded size exceeds limit).
  std::string payload(kMaxFrameSize, 'x');
  auto frame = MakeFrame(payload);
  ASSERT_GT(frame.size(), kMaxFrameSize);

  IREE_ASSERT_OK(InjectRecv(frame));
  EXPECT_EQ(ctx_.messages.size(), 0u);
  ASSERT_EQ(ctx_.errors.size(), 1u);
  EXPECT_EQ(ctx_.errors[0], IREE_STATUS_RESOURCE_EXHAUSTED);
}

TEST_F(FramingAdapterTest, MaxSizeFrameSucceeds) {
  ActivateWithCallbacks();

  // Frame exactly at max_frame_size (header + payload = 1024).
  std::string payload(kMaxFrameSize - kHeaderSize, 'y');
  auto frame = MakeFrame(payload);
  ASSERT_EQ(frame.size(), kMaxFrameSize);

  IREE_ASSERT_OK(InjectRecv(frame));

  ASSERT_EQ(ctx_.messages.size(), 1u);
  EXPECT_EQ(ctx_.messages[0].data, frame);
}

//===----------------------------------------------------------------------===//
// Endpoint view semantics
//===----------------------------------------------------------------------===//

TEST_F(FramingAdapterTest, EndpointIsCopyable) {
  // Endpoint is a value type (two pointers), can be copied freely.
  iree_net_message_endpoint_t copy = endpoint_;
  EXPECT_EQ(copy.self, endpoint_.self);
  EXPECT_EQ(copy.vtable, endpoint_.vtable);

  // Operations via the copy work the same as via the original.
  iree_net_message_endpoint_set_callbacks(copy, ctx_.MakeCallbacks());
  IREE_ASSERT_OK(iree_net_message_endpoint_activate(copy));

  auto frame = MakeFrame("via copy");
  IREE_ASSERT_OK(InjectRecv(frame));

  ASSERT_EQ(ctx_.messages.size(), 1u);
  EXPECT_EQ(ctx_.messages[0].data, frame);
}

}  // namespace
}  // namespace net
}  // namespace iree
