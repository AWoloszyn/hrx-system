// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/carrier/tcp/connection.h"

#include <array>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "iree/async/operations/net.h"
#include "iree/async/proactor_platform.h"
#include "iree/async/slab.h"
#include "iree/net/message_endpoint.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace iree {
namespace {

struct ReceivePool {
  // Slab providing receive storage.
  iree_async_slab_t* slab = nullptr;

  // Proactor registration covering |slab|.
  iree_async_region_t* region = nullptr;

  // Pool suballocating registered receive buffers.
  iree_async_buffer_pool_t* pool = nullptr;
};

struct OperationResult {
  // Completion status code.
  iree_status_code_t status_code = IREE_STATUS_UNKNOWN;

  // True after the operation callback fires.
  bool completed = false;
};

struct EndpointReadyResult {
  // Fixture flag proving callback affinity to proactor polling.
  bool* is_polling = nullptr;

  // Number of terminal callbacks observed.
  int callback_count = 0;

  // Completion status code.
  iree_status_code_t status_code = IREE_STATUS_UNKNOWN;

  // Borrowed endpoint returned on success.
  iree_net_message_endpoint_t endpoint = {};
};

struct MessageResult {
  // Fixture flag proving callback affinity to proactor polling.
  bool* is_polling = nullptr;

  // Payloads observed in callback order.
  std::vector<std::string> messages;

  // Number of terminal error callbacks observed.
  int error_count = 0;

  // First terminal endpoint status code.
  iree_status_code_t error_code = IREE_STATUS_OK;

  // Status returned by message callbacks.
  iree_status_code_t message_status = IREE_STATUS_OK;
};

struct SendResult {
  // Fixture flag proving callback affinity to proactor polling.
  bool* is_polling = nullptr;

  // Optional callback-order trace.
  std::vector<int>* callback_order = nullptr;

  // Value appended to |callback_order|.
  int identifier = 0;

  // Number of terminal callbacks observed.
  int callback_count = 0;

  // Completion status code.
  iree_status_code_t status_code = IREE_STATUS_UNKNOWN;

  // Payload bytes reported by the endpoint.
  iree_host_size_t bytes_transferred = 0;
};

struct EndpointDeactivateResult {
  // Optional callback-order trace.
  std::vector<int>* callback_order = nullptr;

  // Value appended to |callback_order|.
  int identifier = 0;

  // True after endpoint deactivation completes.
  bool completed = false;
};

struct ConnectionDeactivateResult {
  // Number of terminal callbacks observed.
  int callback_count = 0;

  // True after connection deactivation completes.
  bool completed = false;
};

enum class MalformedHeaderKind {
  kMagic,
  kVersion,
  kFlags,
  kEndpointOrdinal,
  kReserved,
  kEmptyPayload,
};

struct MalformedHeaderCase {
  // Stable parameter name used by GoogleTest.
  const char* name;

  // Header field corrupted by this case.
  MalformedHeaderKind kind;
};

struct BlockingAllocator {
  enum class Gate {
    kNone,
    kAllocation,
    kFree,
  };

  // Mutex serializing the one-shot allocator gate.
  std::mutex mutex;

  // Notifies the test when an allocator command enters and may resume.
  std::condition_variable condition;

  // Allocator command family stopped by the one-shot gate.
  Gate gate = Gate::kNone;

  // True while an allocator command is waiting at the gate.
  bool entered = false;

  // True when the waiting allocation may proceed.
  bool released = false;

  // True when the gated operation returned without entering the gate.
  bool attempt_completed = false;

  // Number of allocation commands observed by this wrapper.
  iree_host_size_t allocation_count = 0;

  // Number of free commands observed by this wrapper.
  iree_host_size_t free_count = 0;

  iree_allocator_t allocator() { return {this, Control}; }

  void Arm(Gate new_gate) {
    std::lock_guard<std::mutex> lock(mutex);
    gate = new_gate;
    entered = false;
    released = false;
    attempt_completed = false;
  }

  bool WaitUntilEnteredOrCompleted() {
    std::unique_lock<std::mutex> lock(mutex);
    condition.wait(lock, [&] { return entered || attempt_completed; });
    return entered;
  }

  void CompleteAttempt() {
    std::lock_guard<std::mutex> lock(mutex);
    attempt_completed = true;
    condition.notify_all();
  }

  void Release() {
    std::lock_guard<std::mutex> lock(mutex);
    released = true;
    condition.notify_all();
  }

  iree_host_size_t AllocationCount() {
    std::lock_guard<std::mutex> lock(mutex);
    return allocation_count;
  }

  iree_host_size_t OutstandingAllocationCount() {
    std::lock_guard<std::mutex> lock(mutex);
    return allocation_count - free_count;
  }

  static iree_status_t Control(void* self, iree_allocator_command_t command,
                               const void* params, void** inout_ptr) {
    auto* allocator = static_cast<BlockingAllocator*>(self);
    const bool is_allocation = command == IREE_ALLOCATOR_COMMAND_MALLOC ||
                               command == IREE_ALLOCATOR_COMMAND_CALLOC;
    const bool is_free = command == IREE_ALLOCATOR_COMMAND_FREE;
    if (is_allocation || is_free) {
      std::unique_lock<std::mutex> lock(allocator->mutex);
      if (is_allocation) {
        ++allocator->allocation_count;
      } else {
        ++allocator->free_count;
      }
      const bool should_stop =
          (allocator->gate == Gate::kAllocation && is_allocation) ||
          (allocator->gate == Gate::kFree && is_free);
      if (should_stop) {
        allocator->gate = Gate::kNone;
        allocator->entered = true;
        allocator->condition.notify_all();
        allocator->condition.wait(lock, [&] { return allocator->released; });
      }
    }
    iree_allocator_t system_allocator = iree_allocator_system();
    return system_allocator.ctl(system_allocator.self, command, params,
                                inout_ptr);
  }
};

static void OperationCompleted(void* user_data,
                               iree_async_operation_t* operation,
                               iree_status_t status,
                               iree_async_completion_flags_t flags) {
  (void)operation;
  (void)flags;
  auto* result = static_cast<OperationResult*>(user_data);
  result->status_code = iree_status_code(status);
  result->completed = true;
  iree_status_free(status);
}

static void EndpointReady(void* user_data, iree_status_t status,
                          iree_net_message_endpoint_t endpoint) {
  auto* result = static_cast<EndpointReadyResult*>(user_data);
  EXPECT_TRUE(*result->is_polling);
  ++result->callback_count;
  result->status_code = iree_status_code(status);
  result->endpoint = endpoint;
  iree_status_free(status);
}

static iree_status_t MessageReceived(void* user_data,
                                     iree_const_byte_span_t message,
                                     iree_async_buffer_lease_t* lease) {
  auto* result = static_cast<MessageResult*>(user_data);
  EXPECT_TRUE(*result->is_polling);
  EXPECT_NE(lease, nullptr);
  result->messages.emplace_back(reinterpret_cast<const char*>(message.data),
                                message.data_length);
  return result->message_status == IREE_STATUS_OK
             ? iree_ok_status()
             : iree_make_status(result->message_status,
                                "message rejected for testing");
}

static void EndpointError(void* user_data, iree_status_t status) {
  auto* result = static_cast<MessageResult*>(user_data);
  EXPECT_TRUE(*result->is_polling);
  if (result->error_count++ == 0) {
    result->error_code = iree_status_code(status);
  }
  iree_status_free(status);
}

static void SendCompleted(void* user_data, iree_status_t status,
                          iree_host_size_t bytes_transferred) {
  auto* result = static_cast<SendResult*>(user_data);
  EXPECT_TRUE(*result->is_polling);
  ++result->callback_count;
  result->status_code = iree_status_code(status);
  result->bytes_transferred = bytes_transferred;
  if (result->callback_order) {
    result->callback_order->push_back(result->identifier);
  }
  iree_status_free(status);
}

static void EndpointDeactivated(void* user_data) {
  auto* result = static_cast<EndpointDeactivateResult*>(user_data);
  result->completed = true;
  if (result->callback_order) {
    result->callback_order->push_back(result->identifier);
  }
}

static void ConnectionDeactivated(void* user_data) {
  auto* result = static_cast<ConnectionDeactivateResult*>(user_data);
  ++result->callback_count;
  result->completed = true;
}

class TcpConnectionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    IREE_ASSERT_OK(iree_async_proactor_create_platform(
        iree_async_proactor_options_default(), iree_allocator_system(),
        &proactor_));
  }

  void TearDown() override {
    DeactivateAndRelease(&client_connection_);
    DeactivateAndRelease(&server_connection_);
    iree_async_socket_release(client_socket_);
    iree_async_socket_release(server_socket_);
    ReleaseReceivePool(&client_receive_pool_);
    ReleaseReceivePool(&server_receive_pool_);
    iree_async_proactor_release(proactor_);
  }

  void Poll() {
    is_polling_ = true;
    iree_status_t status = iree_async_proactor_poll(
        proactor_, iree_infinite_timeout(), /*out_completed_count=*/nullptr);
    is_polling_ = false;
    IREE_ASSERT_OK(status);
  }

  void PollUntil(const std::function<bool()>& condition) {
    while (!condition()) {
      Poll();
    }
  }

  void CreateReceivePool(iree_host_size_t buffer_size,
                         iree_host_size_t buffer_count,
                         ReceivePool* out_receive_pool) {
    iree_async_slab_options_t options = {};
    options.buffer_size = buffer_size;
    options.buffer_count = buffer_count;
    IREE_ASSERT_OK(iree_async_slab_create(options, iree_allocator_system(),
                                          &out_receive_pool->slab));
    IREE_ASSERT_OK(iree_async_proactor_register_slab(
        proactor_, out_receive_pool->slab, IREE_ASYNC_BUFFER_ACCESS_FLAG_WRITE,
        &out_receive_pool->region));
    IREE_ASSERT_OK(iree_async_buffer_pool_create(out_receive_pool->region,
                                                 iree_allocator_system(),
                                                 &out_receive_pool->pool));
  }

  static void ReleaseReceivePool(ReceivePool* receive_pool) {
    iree_async_buffer_pool_release(receive_pool->pool);
    iree_async_region_release(receive_pool->region);
    iree_async_slab_release(receive_pool->slab);
    *receive_pool = {};
  }

  void EstablishSockets() {
    iree_async_socket_t* listener = nullptr;
    IREE_ASSERT_OK(iree_async_socket_create(
        proactor_, IREE_ASYNC_SOCKET_TYPE_TCP,
        IREE_ASYNC_SOCKET_OPTION_REUSE_ADDR, &listener));
    iree_async_address_t bind_address;
    IREE_ASSERT_OK(iree_async_address_from_ipv4(
        iree_make_cstring_view("127.0.0.1"), 0, &bind_address));
    IREE_ASSERT_OK(iree_async_socket_bind(listener, &bind_address));
    IREE_ASSERT_OK(iree_async_socket_listen(listener, 1));
    iree_async_address_t listen_address;
    IREE_ASSERT_OK(
        iree_async_socket_query_local_address(listener, &listen_address));

    OperationResult accept_result;
    iree_async_socket_accept_operation_t accept_operation = {};
    iree_async_operation_initialize(
        &accept_operation.base, IREE_ASYNC_OPERATION_TYPE_SOCKET_ACCEPT,
        IREE_ASYNC_OPERATION_FLAG_NONE, OperationCompleted, &accept_result);
    accept_operation.listen_socket = listener;
    IREE_ASSERT_OK(
        iree_async_proactor_submit_one(proactor_, &accept_operation.base));

    IREE_ASSERT_OK(iree_async_socket_create(
        proactor_, IREE_ASYNC_SOCKET_TYPE_TCP,
        IREE_ASYNC_SOCKET_OPTION_NO_DELAY, &client_socket_));
    OperationResult connect_result;
    iree_async_socket_connect_operation_t connect_operation = {};
    iree_async_operation_initialize(
        &connect_operation.base, IREE_ASYNC_OPERATION_TYPE_SOCKET_CONNECT,
        IREE_ASYNC_OPERATION_FLAG_NONE, OperationCompleted, &connect_result);
    connect_operation.socket = client_socket_;
    connect_operation.address = listen_address;
    IREE_ASSERT_OK(
        iree_async_proactor_submit_one(proactor_, &connect_operation.base));

    PollUntil(
        [&] { return accept_result.completed && connect_result.completed; });
    ASSERT_EQ(accept_result.status_code, IREE_STATUS_OK);
    ASSERT_EQ(connect_result.status_code, IREE_STATUS_OK);
    server_socket_ = accept_operation.accepted_socket;
    ASSERT_NE(server_socket_, nullptr);
    iree_async_socket_release(listener);
  }

  void CreateConnectionPair(
      iree_net_tcp_connection_options_t client_options =
          iree_net_tcp_connection_options_default(),
      iree_net_tcp_connection_options_t server_options =
          iree_net_tcp_connection_options_default(),
      iree_host_size_t receive_buffer_size = 4096,
      iree_host_size_t receive_buffer_count = 4,
      iree_allocator_t client_host_allocator = iree_allocator_system(),
      iree_allocator_t server_host_allocator = iree_allocator_system()) {
    CreateReceivePool(receive_buffer_size, receive_buffer_count,
                      &client_receive_pool_);
    CreateReceivePool(receive_buffer_size, receive_buffer_count,
                      &server_receive_pool_);
    EstablishSockets();
    IREE_ASSERT_OK(iree_net_tcp_connection_create(
        proactor_, client_socket_, client_receive_pool_.pool, &client_options,
        client_host_allocator, &client_connection_));
    IREE_ASSERT_OK(iree_net_tcp_connection_create(
        proactor_, server_socket_, server_receive_pool_.pool, &server_options,
        server_host_allocator, &server_connection_));
  }

  iree_net_message_endpoint_t OpenEndpoint(
      iree_net_connection_t* connection,
      EndpointReadyResult* out_ready_result = nullptr) {
    EndpointReadyResult ready_result;
    ready_result.is_polling = &is_polling_;
    iree_status_t status = iree_net_connection_open_endpoint(
        connection, {EndpointReady, &ready_result});
    const iree_status_code_t status_code = iree_status_code(status);
    IREE_EXPECT_OK(status);
    if (status_code != IREE_STATUS_OK) {
      return {};
    }
    PollUntil([&] { return ready_result.callback_count == 1; });
    EXPECT_EQ(ready_result.status_code, IREE_STATUS_OK);
    if (out_ready_result) {
      *out_ready_result = ready_result;
    }
    return ready_result.endpoint;
  }

  void ActivateEndpoint(iree_net_message_endpoint_t endpoint,
                        MessageResult* message_result) {
    message_result->is_polling = &is_polling_;
    iree_net_message_endpoint_set_callbacks(
        endpoint,
        {MessageReceived, EndpointError, static_cast<void*>(message_result)});
    IREE_ASSERT_OK(iree_net_message_endpoint_activate(endpoint));
  }

  MessageResult* CreateMessageResult() {
    auto result = std::make_unique<MessageResult>();
    MessageResult* result_ptr = result.get();
    message_results_.push_back(std::move(result));
    return result_ptr;
  }

  iree_status_t SendMessage(iree_net_message_endpoint_t endpoint,
                            iree_async_span_list_t data,
                            SendResult* send_result) {
    iree_net_message_endpoint_send_params_t params = {
        data,
        {SendCompleted, send_result},
    };
    return iree_net_message_endpoint_send(endpoint, &params);
  }

  void DeactivateAndRelease(iree_net_connection_t** connection_ptr) {
    iree_net_connection_t* connection = *connection_ptr;
    if (!connection) {
      return;
    }
    ConnectionDeactivateResult result;
    iree_net_connection_deactivate(connection,
                                   {ConnectionDeactivated, &result});
    PollUntil([&] { return result.completed; });
    iree_net_connection_release(connection);
    *connection_ptr = nullptr;
  }

  // Platform proactor shared by the two focused connection peers.
  iree_async_proactor_t* proactor_ = nullptr;

  // True only while the fixture is polling |proactor_|.
  bool is_polling_ = false;

  // Client receive resources.
  ReceivePool client_receive_pool_;

  // Server receive resources.
  ReceivePool server_receive_pool_;

  // Connected client socket retained by the fixture.
  iree_async_socket_t* client_socket_ = nullptr;

  // Connected server socket retained by the fixture.
  iree_async_socket_t* server_socket_ = nullptr;

  // Client connection under test.
  iree_net_connection_t* client_connection_ = nullptr;

  // Server connection under test.
  iree_net_connection_t* server_connection_ = nullptr;

  // Callback state retained through fixture connection teardown.
  std::vector<std::unique_ptr<MessageResult>> message_results_;

  // Fixture-owned allocator used by deterministic admission-race tests.
  BlockingAllocator blocking_allocator_;
};

class TcpConnectionMalformedHeaderTest
    : public TcpConnectionTest,
      public ::testing::WithParamInterface<MalformedHeaderCase> {};

TEST(TcpConnectionOptionsTest, Defaults) {
  const iree_net_tcp_connection_options_t options =
      iree_net_tcp_connection_options_default();
  EXPECT_EQ(options.max_endpoint_count,
            IREE_NET_TCP_DEFAULT_MAX_ENDPOINT_COUNT);
  EXPECT_EQ(options.max_frame_size, IREE_NET_TCP_DEFAULT_MAX_FRAME_SIZE);
  EXPECT_EQ(options.max_pending_frames_per_endpoint,
            IREE_NET_TCP_DEFAULT_MAX_PENDING_FRAMES_PER_ENDPOINT);
  EXPECT_EQ(options.carrier_options.max_send_operations,
            IREE_NET_TCP_DEFAULT_MAX_SEND_OPERATIONS);
}

TEST_F(TcpConnectionTest, RoutesOrdinalsAndHandlesScatterOverflow) {
  iree_net_tcp_connection_options_t options =
      iree_net_tcp_connection_options_default();
  options.max_endpoint_count = 2;
  options.carrier_options.max_send_operations = 4;
  CreateConnectionPair(options, options);

  iree_net_message_endpoint_t client_endpoint_0 =
      OpenEndpoint(client_connection_);
  iree_net_message_endpoint_t client_endpoint_1 =
      OpenEndpoint(client_connection_);
  iree_net_message_endpoint_t server_endpoint_0 =
      OpenEndpoint(server_connection_);
  iree_net_message_endpoint_t server_endpoint_1 =
      OpenEndpoint(server_connection_);
  MessageResult* client_messages_0 = CreateMessageResult();
  MessageResult* client_messages_1 = CreateMessageResult();
  MessageResult* server_messages_0 = CreateMessageResult();
  MessageResult* server_messages_1 = CreateMessageResult();
  ActivateEndpoint(client_endpoint_0, client_messages_0);
  ActivateEndpoint(client_endpoint_1, client_messages_1);
  ActivateEndpoint(server_endpoint_0, server_messages_0);
  ActivateEndpoint(server_endpoint_1, server_messages_1);

  std::array<uint8_t, IREE_ASYNC_SOCKET_SEND_MAX_BUFFERS> bytes = {};
  std::array<iree_async_span_t, IREE_ASYNC_SOCKET_SEND_MAX_BUFFERS> spans;
  std::string expected;
  for (iree_host_size_t i = 0; i < spans.size(); ++i) {
    bytes[i] = static_cast<uint8_t>('a' + i);
    spans[i] = iree_async_span_from_ptr(&bytes[i], 1);
    expected.push_back(static_cast<char>(bytes[i]));
  }
  SendResult scatter_result;
  scatter_result.is_polling = &is_polling_;
  IREE_ASSERT_OK(SendMessage(
      client_endpoint_1, iree_async_span_list_make(spans.data(), spans.size()),
      &scatter_result));

  constexpr char kDirectPayload[] = "direct endpoint zero";
  void* direct_data = nullptr;
  iree_net_carrier_send_handle_t direct_handle = 0;
  IREE_ASSERT_OK(iree_net_message_endpoint_begin_send(
      server_endpoint_0, sizeof(kDirectPayload), &direct_data, &direct_handle));
  memcpy(direct_data, kDirectPayload, sizeof(kDirectPayload));
  SendResult direct_result;
  direct_result.is_polling = &is_polling_;
  IREE_ASSERT_OK(iree_net_message_endpoint_commit_send(
      server_endpoint_0, direct_handle, {SendCompleted, &direct_result}));

  PollUntil([&] {
    return scatter_result.callback_count == 1 &&
           direct_result.callback_count == 1 &&
           server_messages_1->messages.size() == 1 &&
           client_messages_0->messages.size() == 1;
  });
  EXPECT_EQ(scatter_result.status_code, IREE_STATUS_OK);
  EXPECT_EQ(scatter_result.bytes_transferred, expected.size());
  EXPECT_EQ(server_messages_1->messages[0], expected);
  EXPECT_TRUE(server_messages_0->messages.empty());
  EXPECT_EQ(direct_result.status_code, IREE_STATUS_OK);
  EXPECT_EQ(direct_result.bytes_transferred, sizeof(kDirectPayload));
  EXPECT_EQ(client_messages_0->messages[0],
            std::string(kDirectPayload, sizeof(kDirectPayload)));
  EXPECT_TRUE(client_messages_1->messages.empty());
}

TEST_F(TcpConnectionTest, DeliversFramesSentBeforePeerEndpointActivation) {
  iree_net_tcp_connection_options_t options =
      iree_net_tcp_connection_options_default();
  options.carrier_options.max_send_operations = 2;
  CreateConnectionPair(options, options,
                       /*receive_buffer_size=*/64,
                       /*receive_buffer_count=*/2);

  iree_net_message_endpoint_t client_endpoint =
      OpenEndpoint(client_connection_);
  MessageResult* client_messages = CreateMessageResult();
  ActivateEndpoint(client_endpoint, client_messages);

  std::array<std::string, 2> payloads = {
      std::string(256 * 1024, 'p'),
      std::string(128 * 1024, 'q'),
  };
  std::array<iree_async_span_t, 2> payload_spans;
  std::array<SendResult, 2> send_results;
  for (iree_host_size_t i = 0; i < payloads.size(); ++i) {
    payload_spans[i] =
        iree_async_span_from_ptr(payloads[i].data(), payloads[i].size());
    send_results[i].is_polling = &is_polling_;
    IREE_ASSERT_OK(SendMessage(client_endpoint,
                               iree_async_span_list_make(&payload_spans[i], 1),
                               &send_results[i]));
  }
  PollUntil([&] {
    return send_results[0].callback_count == 1 &&
           send_results[1].callback_count == 1;
  });
  EXPECT_EQ(send_results[0].status_code, IREE_STATUS_OK);
  EXPECT_EQ(send_results[1].status_code, IREE_STATUS_OK);

  iree_net_message_endpoint_t server_endpoint =
      OpenEndpoint(server_connection_);
  MessageResult* server_messages = CreateMessageResult();
  ActivateEndpoint(server_endpoint, server_messages);
  PollUntil(
      [&] { return server_messages->messages.size() == payloads.size(); });
  EXPECT_EQ(server_messages->messages[0], payloads[0]);
  EXPECT_EQ(server_messages->messages[1], payloads[1]);
  EXPECT_EQ(server_messages->error_count, 0);
}

TEST_F(TcpConnectionTest, EndpointDeactivationWaitsForAcceptedSend) {
  CreateConnectionPair();
  iree_net_message_endpoint_t client_endpoint =
      OpenEndpoint(client_connection_);
  iree_net_message_endpoint_t server_endpoint =
      OpenEndpoint(server_connection_);
  MessageResult* client_messages = CreateMessageResult();
  MessageResult* server_messages = CreateMessageResult();
  ActivateEndpoint(client_endpoint, client_messages);
  ActivateEndpoint(server_endpoint, server_messages);

  std::vector<int> callback_order;
  std::string payload(256 * 1024, 'd');
  iree_async_span_t payload_span =
      iree_async_span_from_ptr(payload.data(), payload.size());
  SendResult send_result;
  send_result.is_polling = &is_polling_;
  send_result.callback_order = &callback_order;
  send_result.identifier = 1;
  IREE_ASSERT_OK(SendMessage(client_endpoint,
                             iree_async_span_list_make(&payload_span, 1),
                             &send_result));

  EndpointDeactivateResult deactivate_result;
  deactivate_result.callback_order = &callback_order;
  deactivate_result.identifier = 2;
  IREE_ASSERT_OK(iree_net_message_endpoint_deactivate(
      client_endpoint, EndpointDeactivated, &deactivate_result));
  EXPECT_FALSE(deactivate_result.completed);
  PollUntil([&] { return deactivate_result.completed; });
  EXPECT_EQ(send_result.callback_count, 1);
  EXPECT_EQ(send_result.status_code, IREE_STATUS_OK);
  EXPECT_EQ(callback_order, (std::vector<int>{1, 2}));
}

TEST_F(TcpConnectionTest, ConcurrentDeactivationPreservesAdmittedSend) {
  iree_net_tcp_connection_options_t options =
      iree_net_tcp_connection_options_default();
  options.carrier_options.max_send_operations = 2;
  CreateConnectionPair(options, options,
                       /*receive_buffer_size=*/4096,
                       /*receive_buffer_count=*/4,
                       blocking_allocator_.allocator());
  iree_net_message_endpoint_t client_endpoint =
      OpenEndpoint(client_connection_);
  iree_net_message_endpoint_t server_endpoint =
      OpenEndpoint(server_connection_);
  MessageResult* client_messages = CreateMessageResult();
  MessageResult* server_messages = CreateMessageResult();
  ActivateEndpoint(client_endpoint, client_messages);
  ActivateEndpoint(server_endpoint, server_messages);

  std::array<uint8_t, IREE_ASYNC_SOCKET_SEND_MAX_BUFFERS> bytes = {};
  std::array<iree_async_span_t, IREE_ASYNC_SOCKET_SEND_MAX_BUFFERS> spans;
  std::string expected;
  for (iree_host_size_t i = 0; i < spans.size(); ++i) {
    bytes[i] = static_cast<uint8_t>('A' + i);
    spans[i] = iree_async_span_from_ptr(&bytes[i], 1);
    expected.push_back(static_cast<char>(bytes[i]));
  }
  std::vector<int> callback_order;
  SendResult send_result;
  send_result.is_polling = &is_polling_;
  send_result.callback_order = &callback_order;
  send_result.identifier = 1;

  blocking_allocator_.Arm(BlockingAllocator::Gate::kAllocation);
  iree_status_code_t submit_status = IREE_STATUS_UNKNOWN;
  std::thread submit_thread([&] {
    iree_status_t status = SendMessage(
        client_endpoint, iree_async_span_list_make(spans.data(), spans.size()),
        &send_result);
    submit_status = iree_status_code(status);
    iree_status_free(status);
    blocking_allocator_.CompleteAttempt();
  });
  const bool allocation_entered =
      blocking_allocator_.WaitUntilEnteredOrCompleted();
  if (!allocation_entered) {
    submit_thread.join();
    ADD_FAILURE() << "overflow send did not reach its staging allocation";
    return;
  }

  EndpointDeactivateResult deactivate_result;
  deactivate_result.callback_order = &callback_order;
  deactivate_result.identifier = 2;
  iree_status_t deactivate_status = iree_net_message_endpoint_deactivate(
      client_endpoint, EndpointDeactivated, &deactivate_result);
  const iree_status_code_t deactivate_status_code =
      iree_status_code(deactivate_status);
  IREE_EXPECT_OK(deactivate_status);
  if (deactivate_status_code == IREE_STATUS_OK) {
    EXPECT_FALSE(deactivate_result.completed);
  }
  blocking_allocator_.Release();
  submit_thread.join();
  if (deactivate_status_code != IREE_STATUS_OK) {
    return;
  }

  EXPECT_EQ(submit_status, IREE_STATUS_OK);
  PollUntil([&] {
    return deactivate_result.completed && server_messages->messages.size() == 1;
  });
  EXPECT_EQ(send_result.callback_count, 1);
  EXPECT_EQ(send_result.status_code, IREE_STATUS_OK);
  EXPECT_EQ(server_messages->messages[0], expected);
  EXPECT_EQ(callback_order, (std::vector<int>{1, 2}));
}

TEST_F(TcpConnectionTest, ConcurrentDeactivationRejectsPreparingReservation) {
  CreateConnectionPair(iree_net_tcp_connection_options_default(),
                       iree_net_tcp_connection_options_default(),
                       /*receive_buffer_size=*/4096,
                       /*receive_buffer_count=*/4,
                       blocking_allocator_.allocator());
  iree_net_message_endpoint_t client_endpoint =
      OpenEndpoint(client_connection_);
  MessageResult* client_messages = CreateMessageResult();
  ActivateEndpoint(client_endpoint, client_messages);

  void* reservation_data = nullptr;
  iree_net_carrier_send_handle_t reservation_handle = 0;
  iree_status_code_t begin_status = IREE_STATUS_UNKNOWN;
  blocking_allocator_.Arm(BlockingAllocator::Gate::kAllocation);
  std::thread begin_thread([&] {
    iree_status_t status = iree_net_message_endpoint_begin_send(
        client_endpoint, 64, &reservation_data, &reservation_handle);
    begin_status = iree_status_code(status);
    iree_status_free(status);
    blocking_allocator_.CompleteAttempt();
  });
  const bool allocation_entered =
      blocking_allocator_.WaitUntilEnteredOrCompleted();
  if (!allocation_entered) {
    begin_thread.join();
    ADD_FAILURE() << "direct send did not reach its frame allocation";
    return;
  }

  EndpointDeactivateResult deactivate_result;
  iree_status_t deactivate_status = iree_net_message_endpoint_deactivate(
      client_endpoint, EndpointDeactivated, &deactivate_result);
  const iree_status_code_t deactivate_status_code =
      iree_status_code(deactivate_status);
  IREE_EXPECT_OK(deactivate_status);
  if (deactivate_status_code == IREE_STATUS_OK) {
    EXPECT_FALSE(deactivate_result.completed);
  }
  blocking_allocator_.Release();
  begin_thread.join();
  if (deactivate_status_code != IREE_STATUS_OK) {
    return;
  }

  EXPECT_EQ(begin_status, IREE_STATUS_FAILED_PRECONDITION);
  EXPECT_EQ(reservation_data, nullptr);
  EXPECT_EQ(reservation_handle, 0u);
  PollUntil([&] { return deactivate_result.completed; });
}

TEST_F(TcpConnectionTest, DirectReservationAbortRestoresAdmission) {
  iree_net_tcp_connection_options_t options =
      iree_net_tcp_connection_options_default();
  options.carrier_options.max_send_operations = 1;
  CreateConnectionPair(options, options);
  iree_net_message_endpoint_t endpoint = OpenEndpoint(client_connection_);
  MessageResult* messages = CreateMessageResult();
  ActivateEndpoint(endpoint, messages);

  void* first_data = nullptr;
  iree_net_carrier_send_handle_t first_handle = 0;
  IREE_ASSERT_OK(iree_net_message_endpoint_begin_send(endpoint, 32, &first_data,
                                                      &first_handle));
  EXPECT_EQ(iree_net_message_endpoint_query_send_budget(endpoint).slots, 0u);
  iree_net_message_endpoint_abort_send(endpoint, first_handle);
  EXPECT_EQ(iree_net_message_endpoint_query_send_budget(endpoint).slots, 1u);

  void* second_data = nullptr;
  iree_net_carrier_send_handle_t second_handle = 0;
  IREE_ASSERT_OK(iree_net_message_endpoint_begin_send(
      endpoint, 64, &second_data, &second_handle));
  EXPECT_NE(second_handle, first_handle);
  iree_net_message_endpoint_abort_send(endpoint, second_handle);
}

TEST_F(TcpConnectionTest, DeactivationInvalidatesDirectReservation) {
  CreateConnectionPair();
  iree_net_message_endpoint_t endpoint = OpenEndpoint(client_connection_);
  MessageResult* messages = CreateMessageResult();
  ActivateEndpoint(endpoint, messages);

  void* data = nullptr;
  iree_net_carrier_send_handle_t handle = 0;
  IREE_ASSERT_OK(
      iree_net_message_endpoint_begin_send(endpoint, 32, &data, &handle));
  EndpointDeactivateResult deactivate_result;
  IREE_ASSERT_OK(iree_net_message_endpoint_deactivate(
      endpoint, EndpointDeactivated, &deactivate_result));
  PollUntil([&] { return deactivate_result.completed; });

  SendResult send_result;
  send_result.is_polling = &is_polling_;
  IREE_EXPECT_STATUS_IS(IREE_STATUS_FAILED_PRECONDITION,
                        iree_net_message_endpoint_commit_send(
                            endpoint, handle, {SendCompleted, &send_result}));
  EXPECT_EQ(send_result.callback_count, 0);
}

TEST_F(TcpConnectionTest, ReservedCommitReturnsDirectMessageFailure) {
  CreateConnectionPair();
  iree_net_message_endpoint_t client_endpoint =
      OpenEndpoint(client_connection_);
  iree_net_message_endpoint_t server_endpoint =
      OpenEndpoint(server_connection_);
  MessageResult* client_messages = CreateMessageResult();
  MessageResult* server_messages = CreateMessageResult();
  server_messages->message_status = IREE_STATUS_DATA_LOSS;
  ActivateEndpoint(client_endpoint, client_messages);
  ActivateEndpoint(server_endpoint, server_messages);

  void* reservation_data = nullptr;
  iree_net_carrier_send_handle_t reservation_handle = 0;
  IREE_ASSERT_OK(iree_net_message_endpoint_begin_send(
      server_endpoint, 1, &reservation_data, &reservation_handle));
  *static_cast<uint8_t*>(reservation_data) = 0x7A;

  uint8_t payload = 0x5A;
  iree_async_span_t span = iree_async_span_from_ptr(&payload, 1);
  SendResult trigger_result;
  trigger_result.is_polling = &is_polling_;
  IREE_ASSERT_OK(SendMessage(
      client_endpoint, iree_async_span_list_make(&span, 1), &trigger_result));
  PollUntil([&] {
    return trigger_result.callback_count == 1 &&
           server_messages->error_count == 1;
  });
  ASSERT_EQ(server_messages->error_code, IREE_STATUS_DATA_LOSS);

  SendResult commit_result;
  commit_result.is_polling = &is_polling_;
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_DATA_LOSS,
      iree_net_message_endpoint_commit_send(server_endpoint, reservation_handle,
                                            {SendCompleted, &commit_result}));
  EXPECT_EQ(commit_result.callback_count, 0);

  DeactivateAndRelease(&server_connection_);
  EXPECT_EQ(commit_result.callback_count, 0);
}

TEST_F(TcpConnectionTest, ReservedCommitReturnsDeferredMessageFailure) {
  CreateConnectionPair();
  iree_net_message_endpoint_t client_endpoint =
      OpenEndpoint(client_connection_);
  iree_net_message_endpoint_t server_endpoint =
      OpenEndpoint(server_connection_);
  iree_net_message_endpoint_t client_marker_endpoint =
      OpenEndpoint(client_connection_);
  iree_net_message_endpoint_t server_marker_endpoint =
      OpenEndpoint(server_connection_);
  MessageResult* client_messages = CreateMessageResult();
  MessageResult* client_marker_messages = CreateMessageResult();
  MessageResult* server_marker_messages = CreateMessageResult();
  ActivateEndpoint(client_endpoint, client_messages);
  ActivateEndpoint(client_marker_endpoint, client_marker_messages);
  ActivateEndpoint(server_marker_endpoint, server_marker_messages);

  uint8_t payload = 0x5A;
  iree_async_span_t span = iree_async_span_from_ptr(&payload, 1);
  SendResult queued_result;
  SendResult marker_result;
  queued_result.is_polling = &is_polling_;
  marker_result.is_polling = &is_polling_;
  IREE_ASSERT_OK(SendMessage(
      client_endpoint, iree_async_span_list_make(&span, 1), &queued_result));
  IREE_ASSERT_OK(SendMessage(client_marker_endpoint,
                             iree_async_span_list_make(&span, 1),
                             &marker_result));
  // Ordered wire delivery proves the earlier frame reached the inactive
  // endpoint before this marker reaches its active endpoint.
  PollUntil([&] {
    return queued_result.callback_count == 1 &&
           marker_result.callback_count == 1 &&
           server_marker_messages->messages.size() == 1;
  });

  MessageResult* server_messages = CreateMessageResult();
  server_messages->message_status = IREE_STATUS_DATA_LOSS;
  ActivateEndpoint(server_endpoint, server_messages);
  void* reservation_data = nullptr;
  iree_net_carrier_send_handle_t reservation_handle = 0;
  IREE_ASSERT_OK(iree_net_message_endpoint_begin_send(
      server_endpoint, 1, &reservation_data, &reservation_handle));
  *static_cast<uint8_t*>(reservation_data) = 0x7A;
  PollUntil([&] { return server_messages->error_count == 1; });
  ASSERT_EQ(server_messages->error_code, IREE_STATUS_DATA_LOSS);

  SendResult commit_result;
  commit_result.is_polling = &is_polling_;
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_DATA_LOSS,
      iree_net_message_endpoint_commit_send(server_endpoint, reservation_handle,
                                            {SendCompleted, &commit_result}));
  EXPECT_EQ(commit_result.callback_count, 0);

  DeactivateAndRelease(&server_connection_);
  EXPECT_EQ(commit_result.callback_count, 0);
}

TEST_F(TcpConnectionTest, InvalidScatterOverflowRestoresAdmission) {
  iree_net_tcp_connection_options_t options =
      iree_net_tcp_connection_options_default();
  options.carrier_options.max_send_operations = 1;
  CreateConnectionPair(options, options);
  iree_net_message_endpoint_t endpoint = OpenEndpoint(client_connection_);
  MessageResult* messages = CreateMessageResult();
  ActivateEndpoint(endpoint, messages);

  std::array<uint8_t, IREE_ASYNC_SOCKET_SEND_MAX_BUFFERS> bytes = {};
  std::array<iree_async_span_t, IREE_ASYNC_SOCKET_SEND_MAX_BUFFERS> spans;
  for (iree_host_size_t i = 0; i < spans.size(); ++i) {
    spans[i] = iree_async_span_from_ptr(&bytes[i], 1);
  }
  spans.back() = iree_async_span_from_ptr(nullptr, 1);
  SendResult send_result;
  send_result.is_polling = &is_polling_;
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      SendMessage(endpoint,
                  iree_async_span_list_make(spans.data(), spans.size()),
                  &send_result));
  EXPECT_EQ(send_result.callback_count, 0);
  EXPECT_EQ(iree_net_message_endpoint_query_send_budget(endpoint).slots, 1u);
}

TEST_F(TcpConnectionTest, ReportsConfiguredFrameLimitAsTerminalError) {
  iree_net_tcp_connection_options_t client_options =
      iree_net_tcp_connection_options_default();
  iree_net_tcp_connection_options_t server_options = client_options;
  server_options.max_frame_size = 64;
  CreateConnectionPair(client_options, server_options);
  iree_net_message_endpoint_t client_endpoint =
      OpenEndpoint(client_connection_);
  iree_net_message_endpoint_t server_endpoint =
      OpenEndpoint(server_connection_);
  MessageResult* client_messages = CreateMessageResult();
  MessageResult* server_messages = CreateMessageResult();
  ActivateEndpoint(client_endpoint, client_messages);
  ActivateEndpoint(server_endpoint, server_messages);

  std::array<uint8_t, 128> payload = {};
  iree_async_span_t payload_span =
      iree_async_span_from_ptr(payload.data(), payload.size());
  SendResult send_result;
  send_result.is_polling = &is_polling_;
  IREE_ASSERT_OK(SendMessage(client_endpoint,
                             iree_async_span_list_make(&payload_span, 1),
                             &send_result));
  PollUntil([&] {
    return send_result.callback_count == 1 && server_messages->error_count == 1;
  });
  EXPECT_EQ(send_result.status_code, IREE_STATUS_OK);
  EXPECT_EQ(server_messages->error_code, IREE_STATUS_RESOURCE_EXHAUSTED);

  SendResult rejected_result;
  rejected_result.is_polling = &is_polling_;
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_RESOURCE_EXHAUSTED,
      SendMessage(server_endpoint, iree_async_span_list_make(&payload_span, 1),
                  &rejected_result));
  EXPECT_EQ(rejected_result.callback_count, 0);
}

TEST_P(TcpConnectionMalformedHeaderTest, ReportsDataLoss) {
  CreateConnectionPair();
  iree_net_message_endpoint_t server_endpoint =
      OpenEndpoint(server_connection_);
  MessageResult* server_messages = CreateMessageResult();
  ActivateEndpoint(server_endpoint, server_messages);

  std::array<uint8_t, 17> wire_data = {};
  wire_data[0] = 'I';
  wire_data[1] = 'R';
  wire_data[2] = 'N';
  wire_data[3] = '1';
  wire_data[4] = 1;
  wire_data[8] = 1;
  wire_data[16] = 0x5A;
  iree_host_size_t wire_length = wire_data.size();
  switch (GetParam().kind) {
    case MalformedHeaderKind::kMagic:
      wire_data[0] = 'X';
      break;
    case MalformedHeaderKind::kVersion:
      wire_data[4] = 2;
      break;
    case MalformedHeaderKind::kFlags:
      wire_data[6] = 1;
      break;
    case MalformedHeaderKind::kEndpointOrdinal:
      wire_data[12] = IREE_NET_TCP_DEFAULT_MAX_ENDPOINT_COUNT;
      break;
    case MalformedHeaderKind::kReserved:
      wire_data[14] = 1;
      break;
    case MalformedHeaderKind::kEmptyPayload:
      wire_data[8] = 0;
      wire_length = 16;
      break;
  }
  iree_async_span_t invalid_span =
      iree_async_span_from_ptr(wire_data.data(), wire_length);
  OperationResult send_result;
  iree_async_socket_send_operation_t send_operation = {};
  iree_async_operation_initialize(
      &send_operation.base, IREE_ASYNC_OPERATION_TYPE_SOCKET_SEND,
      IREE_ASYNC_OPERATION_FLAG_NONE, OperationCompleted, &send_result);
  send_operation.socket = client_socket_;
  send_operation.buffers = iree_async_span_list_make(&invalid_span, 1);
  IREE_ASSERT_OK(
      iree_async_proactor_submit_one(proactor_, &send_operation.base));

  PollUntil([&] {
    return send_result.completed && server_messages->error_count == 1;
  });
  EXPECT_EQ(send_result.status_code, IREE_STATUS_OK);
  EXPECT_EQ(server_messages->error_code, IREE_STATUS_DATA_LOSS);
  EXPECT_TRUE(server_messages->messages.empty());
}

INSTANTIATE_TEST_SUITE_P(
    WireHeader, TcpConnectionMalformedHeaderTest,
    ::testing::Values(
        MalformedHeaderCase{"Magic", MalformedHeaderKind::kMagic},
        MalformedHeaderCase{"Version", MalformedHeaderKind::kVersion},
        MalformedHeaderCase{"Flags", MalformedHeaderKind::kFlags},
        MalformedHeaderCase{"EndpointOrdinal",
                            MalformedHeaderKind::kEndpointOrdinal},
        MalformedHeaderCase{"Reserved", MalformedHeaderKind::kReserved},
        MalformedHeaderCase{"EmptyPayload",
                            MalformedHeaderKind::kEmptyPayload}),
    [](const ::testing::TestParamInfo<MalformedHeaderCase>& info) {
      return info.param.name;
    });

TEST_F(TcpConnectionTest, ConnectionDrainCancelsPendingEndpointReady) {
  CreateConnectionPair();
  EndpointReadyResult ready_result;
  ready_result.is_polling = &is_polling_;
  IREE_ASSERT_OK(iree_net_connection_open_endpoint(
      client_connection_, {EndpointReady, &ready_result}));

  ConnectionDeactivateResult deactivate_result;
  iree_net_connection_deactivate(client_connection_,
                                 {ConnectionDeactivated, &deactivate_result});
  EXPECT_FALSE(deactivate_result.completed);
  PollUntil([&] {
    return ready_result.callback_count == 1 && deactivate_result.completed;
  });
  EXPECT_EQ(ready_result.status_code, IREE_STATUS_CANCELLED);
  EXPECT_EQ(ready_result.endpoint.self, nullptr);

  iree_net_connection_release(client_connection_);
  client_connection_ = nullptr;
}

TEST_F(TcpConnectionTest, ConnectionDrainClosesInactiveEndpointAdmission) {
  iree_net_tcp_connection_options_t options =
      iree_net_tcp_connection_options_default();
  options.max_endpoint_count = 2;
  CreateConnectionPair(
      options, options,
      /*receive_buffer_size=*/64,
      /*receive_buffer_count=*/4,
      /*client_host_allocator=*/iree_allocator_system(),
      /*server_host_allocator=*/blocking_allocator_.allocator());

  iree_net_message_endpoint_t client_endpoint =
      OpenEndpoint(client_connection_);
  ActivateEndpoint(client_endpoint, CreateMessageResult());

  OpenEndpoint(server_connection_);

  iree_net_message_endpoint_t server_active_endpoint =
      OpenEndpoint(server_connection_);
  ActivateEndpoint(server_active_endpoint, CreateMessageResult());
  OperationResult activation_barrier;
  iree_async_nop_operation_t barrier = {};
  iree_async_operation_initialize(&barrier.base, IREE_ASYNC_OPERATION_TYPE_NOP,
                                  IREE_ASYNC_OPERATION_FLAG_NONE,
                                  OperationCompleted, &activation_barrier);
  IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &barrier.base));
  PollUntil([&] { return activation_barrier.completed; });

  void* reservation_data = nullptr;
  iree_net_carrier_send_handle_t reservation_handle = 0;
  IREE_ASSERT_OK(iree_net_message_endpoint_begin_send(
      server_active_endpoint, 1, &reservation_data, &reservation_handle));
  *static_cast<uint8_t*>(reservation_data) = 42;

  blocking_allocator_.Arm(BlockingAllocator::Gate::kFree);
  ConnectionDeactivateResult deactivate_result;
  std::thread deactivate_thread([&] {
    iree_net_connection_deactivate(server_connection_,
                                   {ConnectionDeactivated, &deactivate_result});
    blocking_allocator_.CompleteAttempt();
  });
  const bool free_entered = blocking_allocator_.WaitUntilEnteredOrCompleted();
  if (!free_entered) {
    deactivate_thread.join();
    ADD_FAILURE() << "connection drain did not release its reservation";
    return;
  }

  auto finish_deactivation = [&] {
    blocking_allocator_.Release();
    deactivate_thread.join();
    if (!deactivate_result.completed) {
      PollUntil([&] { return deactivate_result.completed; });
    }
  };

  const iree_host_size_t allocation_count_before =
      blocking_allocator_.AllocationCount();
  std::array<std::string, 2> payloads = {
      std::string(128, 'a'),
      std::string(128, 'b'),
  };
  std::array<iree_async_span_t, 2> spans;
  std::array<SendResult, 2> send_results;
  bool sends_accepted = true;
  for (iree_host_size_t i = 0; i < payloads.size(); ++i) {
    spans[i] = iree_async_span_from_ptr(payloads[i].data(), payloads[i].size());
    send_results[i].is_polling = &is_polling_;
    iree_status_t send_status =
        SendMessage(client_endpoint, iree_async_span_list_make(&spans[i], 1),
                    &send_results[i]);
    const iree_status_code_t send_status_code = iree_status_code(send_status);
    IREE_EXPECT_OK(send_status);
    sends_accepted &= send_status_code == IREE_STATUS_OK;
  }
  if (!sends_accepted) {
    finish_deactivation();
    return;
  }
  // Each frame spans multiple receive buffers and allocates reassembly
  // storage. Starting the second reassembly proves the first frame reached
  // the connection admission boundary while deactivation is blocked.
  PollUntil([&] {
    return blocking_allocator_.AllocationCount() >=
               allocation_count_before + payloads.size() &&
           send_results[0].callback_count == 1 &&
           send_results[1].callback_count == 1;
  });
  finish_deactivation();
  EXPECT_EQ(deactivate_result.callback_count, 1);

  // Destruction asserts that every pending-frame record was returned. A frame
  // admitted after the inactive endpoint was cleared violates that invariant.
  iree_net_connection_release(server_connection_);
  server_connection_ = nullptr;
  EXPECT_EQ(blocking_allocator_.OutstandingAllocationCount(), 0u);
}

TEST_F(TcpConnectionTest, DeactivationKeepsSendCallbacksOnOwningProactor) {
  CreateConnectionPair();
  iree_net_message_endpoint_t client_endpoint =
      OpenEndpoint(client_connection_);
  iree_net_message_endpoint_t server_endpoint =
      OpenEndpoint(server_connection_);
  ActivateEndpoint(client_endpoint, CreateMessageResult());
  ActivateEndpoint(server_endpoint, CreateMessageResult());

  OperationResult activation_barrier;
  iree_async_nop_operation_t barrier = {};
  iree_async_operation_initialize(&barrier.base, IREE_ASYNC_OPERATION_TYPE_NOP,
                                  IREE_ASYNC_OPERATION_FLAG_NONE,
                                  OperationCompleted, &activation_barrier);
  IREE_ASSERT_OK(iree_async_proactor_submit_one(proactor_, &barrier.base));
  PollUntil([&] { return activation_barrier.completed; });

  uint8_t payload = 42;
  iree_async_span_t span = iree_async_span_from_ptr(&payload, 1);
  SendResult first_result;
  first_result.is_polling = &is_polling_;
  SendResult second_result;
  second_result.is_polling = &is_polling_;
  IREE_ASSERT_OK(SendMessage(
      client_endpoint, iree_async_span_list_make(&span, 1), &first_result));
  IREE_ASSERT_OK(SendMessage(
      client_endpoint, iree_async_span_list_make(&span, 1), &second_result));
  EXPECT_EQ(first_result.callback_count, 0);
  EXPECT_EQ(second_result.callback_count, 0);

  ConnectionDeactivateResult deactivate_result;
  std::thread producer([&] {
    iree_net_connection_deactivate(client_connection_,
                                   {ConnectionDeactivated, &deactivate_result});
  });
  producer.join();
  EXPECT_EQ(first_result.callback_count, 0);
  EXPECT_EQ(second_result.callback_count, 0);

  PollUntil([&] { return deactivate_result.completed; });
  EXPECT_EQ(first_result.callback_count, 1);
  EXPECT_EQ(second_result.callback_count, 1);
  EXPECT_EQ(second_result.status_code, IREE_STATUS_CANCELLED);
  iree_net_connection_release(client_connection_);
  client_connection_ = nullptr;
}

}  // namespace
}  // namespace iree
