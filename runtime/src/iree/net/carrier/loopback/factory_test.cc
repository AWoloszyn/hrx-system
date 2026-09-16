// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/carrier/loopback/factory.h"

#include <functional>
#include <string>
#include <vector>

#include "iree/async/proactor_platform.h"
#include "iree/net/connection.h"
#include "iree/net/message_endpoint.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace iree {
namespace {

enum PollSide {
  kNotPolling = 0,
  kClientPolling = 1,
  kServerPolling = 2,
};

struct ConnectState {
  int* current_poll_side = nullptr;
  PollSide expected_poll_side = kNotPolling;
  int callback_count = 0;
  iree_status_code_t status_code = IREE_STATUS_UNKNOWN;
  iree_net_connection_t* connection = nullptr;

  static void OnConnect(void* user_data, iree_status_t status,
                        iree_net_connection_t* connection) {
    auto* self = static_cast<ConnectState*>(user_data);
    EXPECT_EQ(*self->current_poll_side, self->expected_poll_side);
    ++self->callback_count;
    self->status_code = iree_status_code(status);
    self->connection = connection;
    iree_status_free(status);
  }

  iree_net_transport_connect_callback_t callback() {
    return {/*.fn=*/OnConnect, /*.user_data=*/this};
  }
};

struct AcceptState {
  int* current_poll_side = nullptr;
  PollSide expected_poll_side = kNotPolling;
  int* sequence = nullptr;
  int last_sequence = 0;
  std::vector<iree_status_code_t> status_codes;
  std::vector<iree_net_connection_t*> connections;

  static void OnAccept(void* user_data, iree_status_t status,
                       iree_net_connection_t* connection) {
    auto* self = static_cast<AcceptState*>(user_data);
    EXPECT_EQ(*self->current_poll_side, self->expected_poll_side);
    if (self->sequence) self->last_sequence = ++*self->sequence;
    self->status_codes.push_back(iree_status_code(status));
    self->connections.push_back(connection);
    iree_status_free(status);
  }

  iree_net_listener_accept_callback_t callback() {
    return {/*.fn=*/OnAccept, /*.user_data=*/this};
  }
};

struct StopState {
  int* current_poll_side = nullptr;
  PollSide expected_poll_side = kNotPolling;
  int* sequence = nullptr;
  int completed_sequence = 0;
  bool completed = false;

  static void OnStopped(void* user_data) {
    auto* self = static_cast<StopState*>(user_data);
    EXPECT_EQ(*self->current_poll_side, self->expected_poll_side);
    if (self->sequence) self->completed_sequence = ++*self->sequence;
    self->completed = true;
  }

  iree_net_listener_stopped_callback_t callback() {
    return {/*.fn=*/OnStopped, /*.user_data=*/this};
  }
};

struct EndpointReadyState {
  int* current_poll_side = nullptr;
  PollSide expected_poll_side = kNotPolling;
  int callback_count = 0;
  iree_status_code_t status_code = IREE_STATUS_UNKNOWN;
  iree_net_message_endpoint_t endpoint = {};

  static void OnReady(void* user_data, iree_status_t status,
                      iree_net_message_endpoint_t endpoint) {
    auto* self = static_cast<EndpointReadyState*>(user_data);
    EXPECT_EQ(*self->current_poll_side, self->expected_poll_side);
    ++self->callback_count;
    self->status_code = iree_status_code(status);
    self->endpoint = endpoint;
    iree_status_free(status);
  }

  iree_net_endpoint_ready_callback_t callback() {
    return {/*.fn=*/OnReady, /*.user_data=*/this};
  }
};

struct MessageState {
  int* current_poll_side = nullptr;
  PollSide expected_poll_side = kNotPolling;
  std::vector<std::string> messages;

  static iree_status_t OnMessage(void* user_data,
                                 iree_const_byte_span_t message,
                                 iree_async_buffer_lease_t* lease) {
    auto* self = static_cast<MessageState*>(user_data);
    EXPECT_EQ(*self->current_poll_side, self->expected_poll_side);
    EXPECT_NE(lease, nullptr);
    self->messages.emplace_back(reinterpret_cast<const char*>(message.data),
                                message.data_length);
    return iree_ok_status();
  }

  static void OnError(void* user_data, iree_status_t status) {
    auto* self = static_cast<MessageState*>(user_data);
    EXPECT_EQ(*self->current_poll_side, self->expected_poll_side);
    ADD_FAILURE() << "unexpected endpoint error: "
                  << iree_status_code_string(iree_status_code(status));
    iree_status_free(status);
  }

  iree_net_message_endpoint_callbacks_t callbacks() {
    return {
        /*.on_message=*/OnMessage,
        /*.on_error=*/OnError,
        /*.user_data=*/this,
    };
  }
};

struct SendState {
  int* current_poll_side = nullptr;
  PollSide expected_poll_side = kNotPolling;
  int callback_count = 0;
  iree_status_code_t status_code = IREE_STATUS_UNKNOWN;

  static void OnComplete(void* user_data, iree_status_t status,
                         iree_host_size_t bytes_transferred) {
    auto* self = static_cast<SendState*>(user_data);
    EXPECT_EQ(*self->current_poll_side, self->expected_poll_side);
    EXPECT_EQ(bytes_transferred, 7u);
    ++self->callback_count;
    self->status_code = iree_status_code(status);
    iree_status_free(status);
  }

  iree_net_send_completion_callback_t callback() {
    return {/*.fn=*/OnComplete, /*.user_data=*/this};
  }
};

struct DeactivateState {
  int* current_poll_side = nullptr;
  PollSide expected_poll_side = kNotPolling;
  bool completed = false;

  static void OnDeactivated(void* user_data) {
    auto* self = static_cast<DeactivateState*>(user_data);
    EXPECT_EQ(*self->current_poll_side, self->expected_poll_side);
    self->completed = true;
  }

  iree_net_connection_deactivate_callback_t callback() {
    return {/*.fn=*/OnDeactivated, /*.user_data=*/this};
  }
};

struct ControlledAllocator {
  iree_allocator_t delegate = iree_allocator_system();
  int allocations_before_failure = -1;

  static iree_status_t Control(void* self, iree_allocator_command_t command,
                               const void* params, void** inout_ptr) {
    auto* allocator = static_cast<ControlledAllocator*>(self);
    const bool is_allocation = command == IREE_ALLOCATOR_COMMAND_MALLOC ||
                               command == IREE_ALLOCATOR_COMMAND_CALLOC ||
                               command == IREE_ALLOCATOR_COMMAND_REALLOC;
    if (is_allocation && allocator->allocations_before_failure == 0) {
      return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                              "intentional allocation failure");
    }
    if (is_allocation && allocator->allocations_before_failure > 0) {
      --allocator->allocations_before_failure;
    }
    return allocator->delegate.ctl(allocator->delegate.self, command, params,
                                   inout_ptr);
  }

  iree_allocator_t value() { return {/*.self=*/this, /*.ctl=*/Control}; }
};

class LoopbackFactoryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    IREE_ASSERT_OK(iree_async_proactor_create_platform(
        iree_async_proactor_options_default(), iree_allocator_system(),
        &client_proactor_));
    IREE_ASSERT_OK(iree_async_proactor_create_platform(
        iree_async_proactor_options_default(), iree_allocator_system(),
        &server_proactor_));
    IREE_ASSERT_OK(iree_net_loopback_factory_create(
        /*options=*/nullptr, iree_allocator_system(), &factory_));
  }

  void TearDown() override {
    StopAndFreeListener();
    for (iree_net_connection_t*& connection : client_connections_) {
      DeactivateAndRelease(connection, client_proactor_, kClientPolling);
    }
    for (iree_net_connection_t*& connection : server_connections_) {
      DeactivateAndRelease(connection, server_proactor_, kServerPolling);
    }
    iree_net_transport_factory_release(factory_);
    if (owns_client_proactor_) iree_async_proactor_release(client_proactor_);
    if (owns_server_proactor_) iree_async_proactor_release(server_proactor_);
  }

  void Poll(iree_async_proactor_t* proactor, PollSide side) {
    current_poll_side_ = side;
    IREE_ASSERT_OK(iree_async_proactor_poll(proactor, iree_infinite_timeout(),
                                            /*out_completed_count=*/nullptr));
    current_poll_side_ = kNotPolling;
  }

  void PollUntil(iree_async_proactor_t* proactor, PollSide side,
                 const std::function<bool()>& condition) {
    while (!condition()) Poll(proactor, side);
  }

  void CreateListener(iree_string_view_t name, AcceptState* accept_state) {
    accept_state->current_poll_side = &current_poll_side_;
    accept_state->expected_poll_side = kServerPolling;
    IREE_ASSERT_OK(iree_net_transport_factory_create_listener(
        factory_, name, server_proactor_, /*receive_pool=*/nullptr,
        accept_state->callback(), iree_allocator_system(), &listener_));
  }

  void StopAndFreeListener() {
    if (!listener_) return;
    StopState stop_state;
    stop_state.current_poll_side = &current_poll_side_;
    stop_state.expected_poll_side = kServerPolling;
    IREE_ASSERT_OK(iree_net_listener_stop(listener_, stop_state.callback()));
    PollUntil(server_proactor_, kServerPolling,
              [&] { return stop_state.completed; });
    iree_net_listener_free(listener_);
    listener_ = nullptr;
  }

  iree_net_message_endpoint_t OpenEndpoint(iree_net_connection_t* connection,
                                           iree_async_proactor_t* proactor,
                                           PollSide side) {
    EndpointReadyState ready_state;
    ready_state.current_poll_side = &current_poll_side_;
    ready_state.expected_poll_side = side;
    IREE_EXPECT_OK(
        iree_net_connection_open_endpoint(connection, ready_state.callback()));
    PollUntil(proactor, side, [&] { return ready_state.callback_count == 1; });
    EXPECT_EQ(ready_state.status_code, IREE_STATUS_OK);
    return ready_state.endpoint;
  }

  void DeactivateAndRelease(iree_net_connection_t*& connection,
                            iree_async_proactor_t* proactor, PollSide side) {
    if (!connection) return;
    DeactivateState deactivate_state;
    deactivate_state.current_poll_side = &current_poll_side_;
    deactivate_state.expected_poll_side = side;
    current_poll_side_ = side;
    iree_net_connection_deactivate(connection, deactivate_state.callback());
    current_poll_side_ = kNotPolling;
    if (!deactivate_state.completed) {
      PollUntil(proactor, side, [&] { return deactivate_state.completed; });
    }
    iree_net_connection_release(connection);
    connection = nullptr;
  }

  int current_poll_side_ = kNotPolling;
  bool owns_client_proactor_ = true;
  bool owns_server_proactor_ = true;
  iree_async_proactor_t* client_proactor_ = nullptr;
  iree_async_proactor_t* server_proactor_ = nullptr;
  iree_net_transport_factory_t* factory_ = nullptr;
  iree_net_listener_t* listener_ = nullptr;
  std::vector<iree_net_connection_t*> client_connections_;
  std::vector<iree_net_connection_t*> server_connections_;
};

TEST_F(LoopbackFactoryTest, ValidatesOptionsAndReportsCapabilities) {
  const iree_net_transport_capabilities_t capabilities =
      iree_net_transport_factory_query_capabilities(factory_);
  EXPECT_TRUE(
      iree_any_bit_set(capabilities, IREE_NET_TRANSPORT_CAPABILITY_RELIABLE));
  EXPECT_TRUE(
      iree_any_bit_set(capabilities, IREE_NET_TRANSPORT_CAPABILITY_ORDERED));

  iree_net_loopback_factory_options_t options =
      iree_net_loopback_factory_options_default();
  options.max_endpoint_count = 0;
  iree_net_transport_factory_t* invalid_factory = nullptr;
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      iree_net_loopback_factory_create(&options, iree_allocator_system(),
                                       &invalid_factory));
  EXPECT_EQ(invalid_factory, nullptr);
}

TEST_F(LoopbackFactoryTest,
       RoutesCallbacksOnOwningProactorsAndCarriesMessages) {
  AcceptState accept_state;
  CreateListener(IREE_SV("service"), &accept_state);

  char address_storage[7] = {};
  iree_string_view_t bound_address = iree_string_view_empty();
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_OUT_OF_RANGE,
      iree_net_listener_query_bound_address(
          listener_, /*buffer_capacity=*/sizeof(address_storage) - 1,
          address_storage, &bound_address));
  EXPECT_TRUE(iree_string_view_is_empty(bound_address));
  IREE_ASSERT_OK(iree_net_listener_query_bound_address(
      listener_, sizeof(address_storage), address_storage, &bound_address));
  EXPECT_TRUE(iree_string_view_equal(bound_address, IREE_SV("service")));

  ConnectState connect_state;
  connect_state.current_poll_side = &current_poll_side_;
  connect_state.expected_poll_side = kClientPolling;
  IREE_ASSERT_OK(iree_net_transport_factory_connect(
      factory_, bound_address, client_proactor_, /*receive_pool=*/nullptr,
      connect_state.callback()));
  EXPECT_EQ(connect_state.callback_count, 0);
  EXPECT_TRUE(accept_state.connections.empty());

  PollUntil(client_proactor_, kClientPolling,
            [&] { return connect_state.callback_count == 1; });
  EXPECT_EQ(connect_state.status_code, IREE_STATUS_OK);
  ASSERT_NE(connect_state.connection, nullptr);
  EXPECT_TRUE(accept_state.connections.empty());

  PollUntil(server_proactor_, kServerPolling,
            [&] { return accept_state.connections.size() == 1; });
  ASSERT_EQ(accept_state.status_codes[0], IREE_STATUS_OK);
  ASSERT_NE(accept_state.connections[0], nullptr);
  EXPECT_EQ(iree_net_connection_proactor(connect_state.connection),
            client_proactor_);
  EXPECT_EQ(iree_net_connection_proactor(accept_state.connections[0]),
            server_proactor_);

  client_connections_.push_back(connect_state.connection);
  server_connections_.push_back(accept_state.connections[0]);
  iree_net_message_endpoint_t client_endpoint =
      OpenEndpoint(client_connections_[0], client_proactor_, kClientPolling);
  iree_net_message_endpoint_t server_endpoint =
      OpenEndpoint(server_connections_[0], server_proactor_, kServerPolling);
  MessageState client_messages;
  client_messages.current_poll_side = &current_poll_side_;
  client_messages.expected_poll_side = kClientPolling;
  iree_net_message_endpoint_set_callbacks(client_endpoint,
                                          client_messages.callbacks());
  IREE_ASSERT_OK(iree_net_message_endpoint_activate(client_endpoint));
  MessageState server_messages;
  server_messages.current_poll_side = &current_poll_side_;
  server_messages.expected_poll_side = kServerPolling;
  iree_net_message_endpoint_set_callbacks(server_endpoint,
                                          server_messages.callbacks());
  IREE_ASSERT_OK(iree_net_message_endpoint_activate(server_endpoint));

  char payload[] = "factory";
  iree_async_span_t payload_span =
      iree_async_span_from_ptr(payload, sizeof(payload) - 1);
  SendState send_state;
  send_state.current_poll_side = &current_poll_side_;
  send_state.expected_poll_side = kClientPolling;
  iree_net_message_endpoint_send_params_t send_params = {
      /*.data=*/iree_async_span_list_make(&payload_span, 1),
      /*.completion_callback=*/send_state.callback(),
  };
  IREE_ASSERT_OK(iree_net_message_endpoint_send(client_endpoint, &send_params));
  PollUntil(server_proactor_, kServerPolling,
            [&] { return server_messages.messages.size() == 1; });
  EXPECT_EQ(server_messages.messages[0], "factory");
  PollUntil(client_proactor_, kClientPolling,
            [&] { return send_state.callback_count == 1; });
  EXPECT_EQ(send_state.status_code, IREE_STATUS_OK);
}

TEST_F(LoopbackFactoryTest, MissingListenerFailsAsynchronously) {
  ConnectState connect_state;
  connect_state.current_poll_side = &current_poll_side_;
  connect_state.expected_poll_side = kClientPolling;
  IREE_ASSERT_OK(iree_net_transport_factory_connect(
      factory_, IREE_SV("missing"), client_proactor_,
      /*receive_pool=*/nullptr, connect_state.callback()));
  EXPECT_EQ(connect_state.callback_count, 0);
  PollUntil(client_proactor_, kClientPolling,
            [&] { return connect_state.callback_count == 1; });
  EXPECT_EQ(connect_state.status_code, IREE_STATUS_UNAVAILABLE);
  EXPECT_EQ(connect_state.connection, nullptr);
}

TEST_F(LoopbackFactoryTest,
       ConnectAllocationFailureRetiresClaimWithoutCallback) {
  iree_net_transport_factory_release(factory_);
  ControlledAllocator controlled_allocator;
  IREE_ASSERT_OK(iree_net_loopback_factory_create(
      /*options=*/nullptr, controlled_allocator.value(), &factory_));
  AcceptState accept_state;
  CreateListener(IREE_SV("failure"), &accept_state);

  // Allow the connect dispatch allocation and fail the accept dispatch
  // allocation after the listener has been claimed.
  controlled_allocator.allocations_before_failure = 1;
  ConnectState connect_state;
  connect_state.current_poll_side = &current_poll_side_;
  connect_state.expected_poll_side = kClientPolling;
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_RESOURCE_EXHAUSTED,
      iree_net_transport_factory_connect(
          factory_, IREE_SV("failure"), client_proactor_,
          /*receive_pool=*/nullptr, connect_state.callback()));
  EXPECT_EQ(connect_state.callback_count, 0);

  StopAndFreeListener();
  iree_net_transport_factory_release(factory_);
  factory_ = nullptr;
}

TEST_F(LoopbackFactoryTest, RejectsDuplicateNameAndReusesStoppedName) {
  AcceptState accept_state;
  CreateListener(IREE_SV("unique"), &accept_state);

  iree_net_listener_t* duplicate_listener = nullptr;
  IREE_EXPECT_STATUS_IS(IREE_STATUS_ALREADY_EXISTS,
                        iree_net_transport_factory_create_listener(
                            factory_, IREE_SV("unique"), server_proactor_,
                            /*receive_pool=*/nullptr, accept_state.callback(),
                            iree_allocator_system(), &duplicate_listener));
  EXPECT_EQ(duplicate_listener, nullptr);

  StopAndFreeListener();
  CreateListener(IREE_SV("unique"), &accept_state);
}

TEST_F(LoopbackFactoryTest, StopWaitsForClaimedAcceptAndRejectsLaterConnects) {
  int sequence = 0;
  AcceptState accept_state;
  accept_state.sequence = &sequence;
  CreateListener(IREE_SV("drain"), &accept_state);

  ConnectState accepted_connect;
  accepted_connect.current_poll_side = &current_poll_side_;
  accepted_connect.expected_poll_side = kClientPolling;
  IREE_ASSERT_OK(iree_net_transport_factory_connect(
      factory_, IREE_SV("drain"), client_proactor_,
      /*receive_pool=*/nullptr, accepted_connect.callback()));

  StopState stop_state;
  stop_state.current_poll_side = &current_poll_side_;
  stop_state.expected_poll_side = kServerPolling;
  stop_state.sequence = &sequence;
  IREE_ASSERT_OK(iree_net_listener_stop(listener_, stop_state.callback()));
  EXPECT_FALSE(stop_state.completed);

  ConnectState rejected_connect;
  rejected_connect.current_poll_side = &current_poll_side_;
  rejected_connect.expected_poll_side = kClientPolling;
  IREE_ASSERT_OK(iree_net_transport_factory_connect(
      factory_, IREE_SV("drain"), client_proactor_,
      /*receive_pool=*/nullptr, rejected_connect.callback()));

  PollUntil(client_proactor_, kClientPolling, [&] {
    return accepted_connect.callback_count == 1 &&
           rejected_connect.callback_count == 1;
  });
  ASSERT_EQ(accepted_connect.status_code, IREE_STATUS_OK);
  ASSERT_NE(accepted_connect.connection, nullptr);
  EXPECT_EQ(rejected_connect.status_code, IREE_STATUS_UNAVAILABLE);
  EXPECT_EQ(rejected_connect.connection, nullptr);
  EXPECT_FALSE(stop_state.completed);

  PollUntil(server_proactor_, kServerPolling, [&] {
    return accept_state.connections.size() == 1 && stop_state.completed;
  });
  ASSERT_EQ(accept_state.status_codes[0], IREE_STATUS_OK);
  ASSERT_NE(accept_state.connections[0], nullptr);
  EXPECT_LT(accept_state.last_sequence, stop_state.completed_sequence);

  client_connections_.push_back(accepted_connect.connection);
  server_connections_.push_back(accept_state.connections[0]);
  iree_net_listener_free(listener_);
  listener_ = nullptr;
}

TEST_F(LoopbackFactoryTest, PendingResourcesRetainFactoryAndProactors) {
  AcceptState accept_state;
  CreateListener(IREE_SV("retained"), &accept_state);

  ConnectState connect_state;
  connect_state.current_poll_side = &current_poll_side_;
  connect_state.expected_poll_side = kClientPolling;
  IREE_ASSERT_OK(iree_net_transport_factory_connect(
      factory_, IREE_SV("retained"), client_proactor_,
      /*receive_pool=*/nullptr, connect_state.callback()));

  iree_net_transport_factory_release(factory_);
  factory_ = nullptr;
  iree_async_proactor_release(client_proactor_);
  owns_client_proactor_ = false;
  iree_async_proactor_release(server_proactor_);
  owns_server_proactor_ = false;

  PollUntil(client_proactor_, kClientPolling,
            [&] { return connect_state.callback_count == 1; });
  PollUntil(server_proactor_, kServerPolling,
            [&] { return accept_state.connections.size() == 1; });
  ASSERT_EQ(connect_state.status_code, IREE_STATUS_OK);
  ASSERT_EQ(accept_state.status_codes[0], IREE_STATUS_OK);
  client_connections_.push_back(connect_state.connection);
  server_connections_.push_back(accept_state.connections[0]);
}

}  // namespace
}  // namespace iree
