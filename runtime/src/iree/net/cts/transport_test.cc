// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "iree/async/buffer_pool.h"
#include "iree/async/proactor_platform.h"
#include "iree/async/slab.h"
#include "iree/net/connection.h"
#include "iree/net/cts/transport_backend.h"
#include "iree/net/message_endpoint.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace iree::net::cts {
namespace {

enum PollSide {
  kNotPolling = 0,
  kClientPolling = 1,
  kServerPolling = 2,
};

struct ConnectState {
  int* current_poll_side = nullptr;
  PollSide expected_poll_side = kNotPolling;
  bool submitted = false;
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
  int callback_count = 0;
  iree_status_code_t status_code = IREE_STATUS_UNKNOWN;
  iree_net_connection_t* connection = nullptr;

  static void OnAccept(void* user_data, iree_status_t status,
                       iree_net_connection_t* connection) {
    auto* self = static_cast<AcceptState*>(user_data);
    EXPECT_EQ(*self->current_poll_side, self->expected_poll_side);
    ++self->callback_count;
    self->status_code = iree_status_code(status);
    self->connection = connection;
    iree_status_free(status);
  }

  iree_net_listener_accept_callback_t callback() {
    return {/*.fn=*/OnAccept, /*.user_data=*/this};
  }
};

struct StopState {
  int* current_poll_side = nullptr;
  PollSide expected_poll_side = kNotPolling;
  bool submitted = false;
  bool completed = false;

  static void OnStopped(void* user_data) {
    auto* self = static_cast<StopState*>(user_data);
    EXPECT_EQ(*self->current_poll_side, self->expected_poll_side);
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
  int error_count = 0;
  iree_status_code_t error_code = IREE_STATUS_OK;

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
    ++self->error_count;
    self->error_code = iree_status_code(status);
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
  iree_host_size_t expected_bytes = 0;
  int callback_count = 0;
  iree_status_code_t status_code = IREE_STATUS_UNKNOWN;

  static void OnComplete(void* user_data, iree_status_t status,
                         iree_host_size_t bytes_transferred) {
    auto* self = static_cast<SendState*>(user_data);
    EXPECT_EQ(*self->current_poll_side, self->expected_poll_side);
    EXPECT_EQ(bytes_transferred, self->expected_bytes);
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

struct ReceivePoolResources {
  // Host storage backing receive buffers.
  iree_async_slab_t* slab = nullptr;

  // Proactor registration covering |slab|.
  iree_async_region_t* region = nullptr;

  // Receive buffer pool suballocating |region|.
  iree_async_buffer_pool_t* pool = nullptr;
};

static iree_status_t CreateReceivePool(iree_async_proactor_t* proactor,
                                       ReceivePoolResources* out_resources) {
  *out_resources = ReceivePoolResources{};
  iree_async_slab_options_t slab_options = {};
  slab_options.buffer_size = 64 * 1024;
  slab_options.buffer_count = 16;
  iree_status_t status = iree_async_slab_create(
      slab_options, iree_allocator_system(), &out_resources->slab);
  if (iree_status_is_ok(status)) {
    status = iree_async_proactor_register_slab(
        proactor, out_resources->slab, IREE_ASYNC_BUFFER_ACCESS_FLAG_WRITE,
        &out_resources->region);
  }
  if (iree_status_is_ok(status)) {
    status = iree_async_buffer_pool_create(
        out_resources->region, iree_allocator_system(), &out_resources->pool);
  }
  if (!iree_status_is_ok(status)) {
    iree_async_buffer_pool_release(out_resources->pool);
    iree_async_region_release(out_resources->region);
    iree_async_slab_release(out_resources->slab);
    *out_resources = ReceivePoolResources{};
  }
  return status;
}

static void ReleaseReceivePool(ReceivePoolResources* resources) {
  iree_async_buffer_pool_release(resources->pool);
  iree_async_region_release(resources->region);
  iree_async_slab_release(resources->slab);
  *resources = ReceivePoolResources{};
}

class TransportTest : public ::testing::Test {
 protected:
  void SetUp() override {
    backend_ = &GetTransportBackend();
    IREE_ASSERT_OK(iree_async_proactor_create_platform(
        iree_async_proactor_options_default(), iree_allocator_system(),
        &client_proactor_));
    IREE_ASSERT_OK(iree_async_proactor_create_platform(
        iree_async_proactor_options_default(), iree_allocator_system(),
        &server_proactor_));
    IREE_ASSERT_OK(CreateReceivePool(client_proactor_, &client_receive_pool_));
    IREE_ASSERT_OK(CreateReceivePool(server_proactor_, &server_receive_pool_));
    iree_status_t status =
        backend_->create_factory(iree_allocator_system(), &factory_);
    if (iree_status_code(status) == IREE_STATUS_UNAVAILABLE) {
      iree_status_free(status);
      GTEST_SKIP() << backend_->name << " transport unavailable";
    }
    IREE_ASSERT_OK(status);

    connect_state_.current_poll_side = &current_poll_side_;
    connect_state_.expected_poll_side = kClientPolling;
    accept_state_.current_poll_side = &current_poll_side_;
    accept_state_.expected_poll_side = kServerPolling;
    stop_state_.current_poll_side = &current_poll_side_;
    stop_state_.expected_poll_side = kServerPolling;
  }

  void TearDown() override {
    DrainPendingConnection();
    StopAndFreeListener();
    DeactivateAndRelease(client_connection_, client_proactor_, kClientPolling);
    DeactivateAndRelease(server_connection_, server_proactor_, kServerPolling);
    iree_net_transport_factory_release(factory_);
    ReleaseReceivePool(&client_receive_pool_);
    ReleaseReceivePool(&server_receive_pool_);
    if (owns_client_proactor_) {
      iree_async_proactor_release(client_proactor_);
    }
    if (owns_server_proactor_) {
      iree_async_proactor_release(server_proactor_);
    }
  }

  void Poll(iree_async_proactor_t* proactor, PollSide side) {
    current_poll_side_ = side;
    iree_status_t status = iree_async_proactor_poll(
        proactor, iree_infinite_timeout(), /*out_completed_count=*/nullptr);
    current_poll_side_ = kNotPolling;
    IREE_ASSERT_OK(status);
  }

  void PollImmediate(iree_async_proactor_t* proactor, PollSide side) {
    current_poll_side_ = side;
    iree_status_t status = iree_async_proactor_poll(
        proactor, iree_immediate_timeout(), /*out_completed_count=*/nullptr);
    current_poll_side_ = kNotPolling;
    if (iree_status_is_deadline_exceeded(status)) {
      iree_status_free(status);
    } else {
      IREE_ASSERT_OK(status);
    }
  }

  void PollUntil(iree_async_proactor_t* proactor, PollSide side,
                 const std::function<bool()>& condition) {
    while (!condition()) {
      Poll(proactor, side);
    }
  }

  void CreateListener() {
    std::string bind_address;
    IREE_ASSERT_OK(backend_->make_bind_address(&bind_address));
    IREE_ASSERT_OK(iree_net_transport_factory_create_listener(
        factory_,
        iree_make_string_view(bind_address.data(), bind_address.size()),
        server_proactor_, server_receive_pool_.pool, accept_state_.callback(),
        iree_allocator_system(), &listener_));

    std::array<char, 1024> address_storage = {};
    iree_string_view_t bound_address = iree_string_view_empty();
    IREE_ASSERT_OK(iree_net_listener_query_bound_address(
        listener_, address_storage.size(), address_storage.data(),
        &bound_address));
    connect_address_.assign(bound_address.data, bound_address.size);
  }

  void SubmitConnect(iree_string_view_t address) {
    IREE_ASSERT_OK(iree_net_transport_factory_connect(
        factory_, address, client_proactor_, client_receive_pool_.pool,
        connect_state_.callback()));
    connect_state_.submitted = true;
  }

  void DrainPendingConnection() {
    if (!connect_state_.submitted) {
      return;
    }
    if (connect_state_.callback_count == 0) {
      PollUntil(client_proactor_, kClientPolling,
                [&] { return connect_state_.callback_count == 1; });
    }
    if (connect_state_.status_code == IREE_STATUS_OK && listener_ &&
        accept_state_.callback_count == 0) {
      PollUntil(server_proactor_, kServerPolling,
                [&] { return accept_state_.callback_count == 1; });
    }
    if (!client_connection_ && connect_state_.connection) {
      client_connection_ = connect_state_.connection;
      connect_state_.connection = nullptr;
    }
    if (!server_connection_ && accept_state_.connection) {
      server_connection_ = accept_state_.connection;
      accept_state_.connection = nullptr;
    }
    connect_state_.submitted = false;
  }

  void EstablishConnection() {
    CreateListener();
    SubmitConnect(iree_make_string_view(connect_address_.data(),
                                        connect_address_.size()));
    EXPECT_EQ(connect_state_.callback_count, 0);
    EXPECT_EQ(accept_state_.callback_count, 0);
    PollUntil(client_proactor_, kClientPolling,
              [&] { return connect_state_.callback_count == 1; });
    ASSERT_EQ(connect_state_.status_code, IREE_STATUS_OK);
    EXPECT_EQ(accept_state_.callback_count, 0);
    PollUntil(server_proactor_, kServerPolling,
              [&] { return accept_state_.callback_count == 1; });
    ASSERT_EQ(accept_state_.status_code, IREE_STATUS_OK);
    DrainPendingConnection();
    ASSERT_NE(client_connection_, nullptr);
    ASSERT_NE(server_connection_, nullptr);
    EXPECT_EQ(iree_net_connection_proactor(client_connection_),
              client_proactor_);
    EXPECT_EQ(iree_net_connection_proactor(server_connection_),
              server_proactor_);
  }

  EndpointReadyState* SubmitOpenEndpoint(iree_net_connection_t* connection,
                                         PollSide side) {
    auto state = std::make_unique<EndpointReadyState>();
    state->current_poll_side = &current_poll_side_;
    state->expected_poll_side = side;
    EndpointReadyState* state_ptr = state.get();
    endpoint_ready_states_.push_back(std::move(state));
    iree_status_t status =
        iree_net_connection_open_endpoint(connection, state_ptr->callback());
    const iree_status_code_t status_code = iree_status_code(status);
    IREE_EXPECT_OK(status);
    return status_code == IREE_STATUS_OK ? state_ptr : nullptr;
  }

  iree_net_message_endpoint_t OpenEndpoint(iree_net_connection_t* connection,
                                           iree_async_proactor_t* proactor,
                                           PollSide side) {
    EndpointReadyState* state = SubmitOpenEndpoint(connection, side);
    if (!state) {
      return {};
    }
    PollUntil(proactor, side, [&] { return state->callback_count == 1; });
    EXPECT_EQ(state->status_code, IREE_STATUS_OK);
    return state->endpoint;
  }

  void StopAndFreeListener() {
    if (!listener_) {
      return;
    }
    if (!stop_state_.submitted) {
      IREE_ASSERT_OK(iree_net_listener_stop(listener_, stop_state_.callback()));
      stop_state_.submitted = true;
    }
    if (!stop_state_.completed) {
      PollUntil(server_proactor_, kServerPolling,
                [&] { return stop_state_.completed; });
    }
    iree_net_listener_free(listener_);
    listener_ = nullptr;
  }

  void DeactivateAndRelease(iree_net_connection_t*& connection,
                            iree_async_proactor_t* proactor, PollSide side) {
    if (!connection) {
      return;
    }
    DeactivateState state;
    state.current_poll_side = &current_poll_side_;
    state.expected_poll_side = side;
    current_poll_side_ = side;
    iree_net_connection_deactivate(connection, state.callback());
    current_poll_side_ = kNotPolling;
    if (!state.completed) {
      PollUntil(proactor, side, [&] { return state.completed; });
    }
    iree_net_connection_release(connection);
    connection = nullptr;
  }

  const TransportBackend* backend_ = nullptr;
  int current_poll_side_ = kNotPolling;
  bool owns_client_proactor_ = true;
  bool owns_server_proactor_ = true;
  iree_async_proactor_t* client_proactor_ = nullptr;
  iree_async_proactor_t* server_proactor_ = nullptr;
  ReceivePoolResources client_receive_pool_;
  ReceivePoolResources server_receive_pool_;
  iree_net_transport_factory_t* factory_ = nullptr;
  iree_net_listener_t* listener_ = nullptr;
  iree_net_connection_t* client_connection_ = nullptr;
  iree_net_connection_t* server_connection_ = nullptr;
  std::string connect_address_;
  ConnectState connect_state_;
  AcceptState accept_state_;
  StopState stop_state_;
  std::vector<std::unique_ptr<EndpointReadyState>> endpoint_ready_states_;
  MessageState client_messages_;
  MessageState server_messages_;
  SendState client_send_;
  SendState server_send_;
};

TEST_F(TransportTest, ReportsRequiredCapabilities) {
  const iree_net_transport_capabilities_t capabilities =
      iree_net_transport_factory_query_capabilities(factory_);
  EXPECT_TRUE(iree_all_bits_set(capabilities, backend_->required_capabilities));
}

TEST_F(TransportTest, ListenerStopIsAsynchronousAndRefusesConnections) {
  CreateListener();
  IREE_ASSERT_OK(iree_net_listener_stop(listener_, stop_state_.callback()));
  stop_state_.submitted = true;
  EXPECT_FALSE(stop_state_.completed);
  PollUntil(server_proactor_, kServerPolling,
            [&] { return stop_state_.completed; });
  iree_net_listener_free(listener_);
  listener_ = nullptr;

  SubmitConnect(
      iree_make_string_view(connect_address_.data(), connect_address_.size()));
  EXPECT_EQ(connect_state_.callback_count, 0);
  PollUntil(client_proactor_, kClientPolling,
            [&] { return connect_state_.callback_count == 1; });
  EXPECT_NE(connect_state_.status_code, IREE_STATUS_OK);
  EXPECT_EQ(connect_state_.connection, nullptr);
}

TEST_F(TransportTest, RoutesBidirectionalMessagesOnOwningProactors) {
  EstablishConnection();
  iree_net_message_endpoint_t client_endpoint =
      OpenEndpoint(client_connection_, client_proactor_, kClientPolling);
  iree_net_message_endpoint_t server_endpoint =
      OpenEndpoint(server_connection_, server_proactor_, kServerPolling);
  ASSERT_NE(client_endpoint.self, nullptr);
  ASSERT_NE(server_endpoint.self, nullptr);

  client_messages_.current_poll_side = &current_poll_side_;
  client_messages_.expected_poll_side = kClientPolling;
  server_messages_.current_poll_side = &current_poll_side_;
  server_messages_.expected_poll_side = kServerPolling;
  iree_net_message_endpoint_set_callbacks(client_endpoint,
                                          client_messages_.callbacks());
  iree_net_message_endpoint_set_callbacks(server_endpoint,
                                          server_messages_.callbacks());
  IREE_ASSERT_OK(iree_net_message_endpoint_activate(client_endpoint));
  IREE_ASSERT_OK(iree_net_message_endpoint_activate(server_endpoint));

  char client_prefix[] = "client-";
  char client_suffix[] = "message";
  iree_async_span_t client_spans[] = {
      iree_async_span_from_ptr(client_prefix, sizeof(client_prefix) - 1),
      iree_async_span_from_ptr(client_suffix, sizeof(client_suffix) - 1),
  };
  client_send_.current_poll_side = &current_poll_side_;
  client_send_.expected_poll_side = kClientPolling;
  client_send_.expected_bytes =
      (sizeof(client_prefix) - 1) + (sizeof(client_suffix) - 1);
  iree_net_message_endpoint_send_params_t send_params = {
      /*.data=*/iree_async_span_list_make(client_spans,
                                          IREE_ARRAYSIZE(client_spans)),
      /*.completion_callback=*/client_send_.callback(),
  };
  IREE_ASSERT_OK(iree_net_message_endpoint_send(client_endpoint, &send_params));
  PollImmediate(client_proactor_, kClientPolling);
  PollUntil(server_proactor_, kServerPolling,
            [&] { return server_messages_.messages.size() == 1; });
  EXPECT_EQ(server_messages_.messages[0], "client-message");
  PollUntil(client_proactor_, kClientPolling,
            [&] { return client_send_.callback_count == 1; });
  EXPECT_EQ(client_send_.status_code, IREE_STATUS_OK);

  char server_payload[] = "server-message";
  iree_async_span_t server_span =
      iree_async_span_from_ptr(server_payload, sizeof(server_payload) - 1);
  server_send_.current_poll_side = &current_poll_side_;
  server_send_.expected_poll_side = kServerPolling;
  server_send_.expected_bytes = sizeof(server_payload) - 1;
  send_params = {
      /*.data=*/iree_async_span_list_make(&server_span, 1),
      /*.completion_callback=*/server_send_.callback(),
  };
  IREE_ASSERT_OK(iree_net_message_endpoint_send(server_endpoint, &send_params));
  PollImmediate(server_proactor_, kServerPolling);
  PollUntil(client_proactor_, kClientPolling,
            [&] { return client_messages_.messages.size() == 1; });
  EXPECT_EQ(client_messages_.messages[0], "server-message");
  PollUntil(server_proactor_, kServerPolling,
            [&] { return server_send_.callback_count == 1; });
  EXPECT_EQ(server_send_.status_code, IREE_STATUS_OK);
  EXPECT_EQ(client_messages_.error_count, 0);
  EXPECT_EQ(server_messages_.error_count, 0);
}

TEST_F(TransportTest, DeactivationCancelsPendingEndpointReadyCallback) {
  EstablishConnection();
  EndpointReadyState* ready_state =
      SubmitOpenEndpoint(client_connection_, kClientPolling);
  ASSERT_NE(ready_state, nullptr);

  DeactivateState deactivate_state;
  deactivate_state.current_poll_side = &current_poll_side_;
  deactivate_state.expected_poll_side = kClientPolling;
  current_poll_side_ = kClientPolling;
  iree_net_connection_deactivate(client_connection_,
                                 deactivate_state.callback());
  current_poll_side_ = kNotPolling;
  EXPECT_EQ(ready_state->callback_count, 0);
  EXPECT_FALSE(deactivate_state.completed);
  PollUntil(client_proactor_, kClientPolling, [&] {
    return ready_state->callback_count == 1 && deactivate_state.completed;
  });
  EXPECT_EQ(ready_state->status_code, IREE_STATUS_CANCELLED);
  EXPECT_EQ(ready_state->endpoint.self, nullptr);
  iree_net_connection_release(client_connection_);
  client_connection_ = nullptr;
}

TEST_F(TransportTest, PendingSubmissionRetainsFactoryAndProactors) {
  CreateListener();
  SubmitConnect(
      iree_make_string_view(connect_address_.data(), connect_address_.size()));

  iree_net_transport_factory_release(factory_);
  factory_ = nullptr;
  ReleaseReceivePool(&client_receive_pool_);
  ReleaseReceivePool(&server_receive_pool_);
  iree_async_proactor_release(client_proactor_);
  owns_client_proactor_ = false;
  iree_async_proactor_release(server_proactor_);
  owns_server_proactor_ = false;

  DrainPendingConnection();
  ASSERT_EQ(connect_state_.status_code, IREE_STATUS_OK);
  ASSERT_EQ(accept_state_.status_code, IREE_STATUS_OK);
  ASSERT_NE(client_connection_, nullptr);
  ASSERT_NE(server_connection_, nullptr);
  EXPECT_NE(
      OpenEndpoint(client_connection_, client_proactor_, kClientPolling).self,
      nullptr);
  EXPECT_NE(
      OpenEndpoint(server_connection_, server_proactor_, kServerPolling).self,
      nullptr);
}

}  // namespace
}  // namespace iree::net::cts
