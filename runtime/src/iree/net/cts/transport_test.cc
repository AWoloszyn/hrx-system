// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "iree/async/buffer_pool.h"
#include "iree/async/proactor_platform.h"
#include "iree/async/slab.h"
#include "iree/net/channel/control/control_channel.h"
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

struct ProtocolHandoffState {
  int* current_poll_side = nullptr;
  PollSide expected_poll_side = kNotPolling;
  iree_net_message_endpoint_t endpoint = {};
  bool message_callback_active = false;
  std::vector<std::string> bootstrap_messages;
  std::vector<std::string> operational_messages;
  int error_count = 0;

  static iree_status_t OnBootstrapMessage(void* user_data,
                                          iree_const_byte_span_t message,
                                          iree_async_buffer_lease_t* lease) {
    auto* self = static_cast<ProtocolHandoffState*>(user_data);
    EXPECT_EQ(*self->current_poll_side, self->expected_poll_side);
    EXPECT_NE(lease, nullptr);
    EXPECT_FALSE(self->message_callback_active);
    self->message_callback_active = true;
    self->bootstrap_messages.emplace_back(
        reinterpret_cast<const char*>(message.data), message.data_length);
    iree_net_message_endpoint_set_callbacks(self->endpoint,
                                            self->operational_callbacks());
    self->message_callback_active = false;
    return iree_ok_status();
  }

  static iree_status_t OnOperationalMessage(void* user_data,
                                            iree_const_byte_span_t message,
                                            iree_async_buffer_lease_t* lease) {
    auto* self = static_cast<ProtocolHandoffState*>(user_data);
    EXPECT_EQ(*self->current_poll_side, self->expected_poll_side);
    EXPECT_NE(lease, nullptr);
    EXPECT_FALSE(self->message_callback_active);
    self->message_callback_active = true;
    self->operational_messages.emplace_back(
        reinterpret_cast<const char*>(message.data), message.data_length);
    self->message_callback_active = false;
    return iree_ok_status();
  }

  static void OnError(void* user_data, iree_status_t status) {
    auto* self = static_cast<ProtocolHandoffState*>(user_data);
    EXPECT_EQ(*self->current_poll_side, self->expected_poll_side);
    ++self->error_count;
    iree_status_free(status);
  }

  iree_net_message_endpoint_callbacks_t bootstrap_callbacks() {
    return {
        /*.on_message=*/OnBootstrapMessage,
        /*.on_error=*/OnError,
        /*.user_data=*/this,
    };
  }

  iree_net_message_endpoint_callbacks_t operational_callbacks() {
    return {
        /*.on_message=*/OnOperationalMessage,
        /*.on_error=*/OnError,
        /*.user_data=*/this,
    };
  }
};

struct ControlMessageState {
  int* current_poll_side = nullptr;
  PollSide expected_poll_side = kNotPolling;
  std::vector<std::string> messages;
  std::vector<iree_net_control_data_flags_t> flags;
  int goaway_count = 0;
  uint32_t goaway_reason = 0;
  int error_count = 0;
  iree_status_code_t error_code = IREE_STATUS_OK;

  static iree_status_t OnData(void* user_data,
                              iree_net_control_data_flags_t flags,
                              iree_const_byte_span_t payload,
                              iree_async_buffer_lease_t* lease) {
    auto* self = static_cast<ControlMessageState*>(user_data);
    EXPECT_EQ(*self->current_poll_side, self->expected_poll_side);
    EXPECT_NE(lease, nullptr);
    self->messages.emplace_back(reinterpret_cast<const char*>(payload.data),
                                payload.data_length);
    self->flags.push_back(flags);
    return iree_ok_status();
  }

  static void OnGoaway(void* user_data, uint32_t reason_code) {
    auto* self = static_cast<ControlMessageState*>(user_data);
    EXPECT_EQ(*self->current_poll_side, self->expected_poll_side);
    ++self->goaway_count;
    self->goaway_reason = reason_code;
  }

  static void OnError(void* user_data, iree_status_t status) {
    auto* self = static_cast<ControlMessageState*>(user_data);
    EXPECT_EQ(*self->current_poll_side, self->expected_poll_side);
    ++self->error_count;
    self->error_code = iree_status_code(status);
    iree_status_free(status);
  }

  iree_net_control_channel_callbacks_t callbacks() {
    return {
        /*.on_data=*/OnData,
        /*.on_goaway=*/OnGoaway,
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
    iree_net_control_channel_free(client_control_channel_);
    iree_net_control_channel_free(server_control_channel_);
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
  ControlMessageState client_control_messages_;
  ControlMessageState server_control_messages_;
  iree_net_control_channel_t* client_control_channel_ = nullptr;
  iree_net_control_channel_t* server_control_channel_ = nullptr;
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
  iree_async_span_t client_span =
      iree_async_span_from_ptr(client_suffix, sizeof(client_suffix) - 1);
  client_send_.current_poll_side = &current_poll_side_;
  client_send_.expected_poll_side = kClientPolling;
  client_send_.expected_bytes =
      (sizeof(client_prefix) - 1) + (sizeof(client_suffix) - 1);
  iree_net_message_endpoint_send_params_t send_params = {
      /*.copied_prefix=*/
      iree_make_const_byte_span(client_prefix, sizeof(client_prefix) - 1),
      /*.data=*/iree_async_span_list_make(&client_span, 1),
      /*.completion_callback=*/client_send_.callback(),
  };
  IREE_ASSERT_OK(iree_net_message_endpoint_send(client_endpoint, &send_params));
  client_prefix[0] = 'X';
  PollImmediate(client_proactor_, kClientPolling);
  PollUntil(server_proactor_, kServerPolling,
            [&] { return server_messages_.messages.size() == 1; });
  EXPECT_EQ(server_messages_.messages[0], "client-message");
  PollUntil(client_proactor_, kClientPolling,
            [&] { return client_send_.callback_count == 1; });
  EXPECT_EQ(client_send_.status_code, IREE_STATUS_OK);

  char server_payload[] = "server-message";
  server_send_.current_poll_side = &current_poll_side_;
  server_send_.expected_poll_side = kServerPolling;
  server_send_.expected_bytes = sizeof(server_payload) - 1;
  send_params = {
      /*.copied_prefix=*/
      iree_make_const_byte_span(server_payload, sizeof(server_payload) - 1),
      /*.data=*/iree_async_span_list_empty(),
      /*.completion_callback=*/server_send_.callback(),
  };
  IREE_ASSERT_OK(iree_net_message_endpoint_send(server_endpoint, &send_params));
  server_payload[0] = 'X';
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

TEST_F(TransportTest, CallbackHandoffPreservesQueuedMessageOrder) {
  EstablishConnection();
  iree_net_message_endpoint_t client_endpoint =
      OpenEndpoint(client_connection_, client_proactor_, kClientPolling);
  iree_net_message_endpoint_t server_endpoint =
      OpenEndpoint(server_connection_, server_proactor_, kServerPolling);
  ASSERT_NE(client_endpoint.self, nullptr);
  ASSERT_NE(server_endpoint.self, nullptr);

  client_messages_.current_poll_side = &current_poll_side_;
  client_messages_.expected_poll_side = kClientPolling;
  iree_net_message_endpoint_set_callbacks(client_endpoint,
                                          client_messages_.callbacks());
  ProtocolHandoffState handoff_state;
  handoff_state.current_poll_side = &current_poll_side_;
  handoff_state.expected_poll_side = kServerPolling;
  handoff_state.endpoint = server_endpoint;
  iree_net_message_endpoint_set_callbacks(server_endpoint,
                                          handoff_state.bootstrap_callbacks());
  IREE_ASSERT_OK(iree_net_message_endpoint_activate(client_endpoint));
  IREE_ASSERT_OK(iree_net_message_endpoint_activate(server_endpoint));

  std::array<std::string, 3> messages = {
      "bootstrap",
      "control-1",
      "control-2",
  };
  std::array<SendState, 3> send_states;
  for (iree_host_size_t i = 0; i < messages.size(); ++i) {
    send_states[i].current_poll_side = &current_poll_side_;
    send_states[i].expected_poll_side = kClientPolling;
    send_states[i].expected_bytes = messages[i].size();
    iree_net_message_endpoint_send_params_t send_params = {
        /*.copied_prefix=*/iree_make_const_byte_span(messages[i].data(),
                                                     messages[i].size()),
        /*.data=*/iree_async_span_list_empty(),
        /*.completion_callback=*/send_states[i].callback(),
    };
    IREE_ASSERT_OK(
        iree_net_message_endpoint_send(client_endpoint, &send_params));
  }

  PollBothUntil([&] {
    const bool all_messages_received =
        handoff_state.bootstrap_messages.size() +
            handoff_state.operational_messages.size() ==
        messages.size();
    const bool all_sends_completed = std::all_of(
        send_states.begin(), send_states.end(),
        [](const SendState& state) { return state.callback_count == 1; });
    return all_messages_received && all_sends_completed;
  });

  EXPECT_FALSE(handoff_state.message_callback_active);
  EXPECT_EQ(handoff_state.bootstrap_messages,
            std::vector<std::string>({"bootstrap"}));
  EXPECT_EQ(handoff_state.operational_messages,
            std::vector<std::string>({"control-1", "control-2"}));
  EXPECT_EQ(handoff_state.error_count, 0);
  for (const SendState& send_state : send_states) {
    EXPECT_EQ(send_state.status_code, IREE_STATUS_OK);
  }
}

TEST_F(TransportTest, CarriesControlDataAndGoaway) {
  EstablishConnection();
  iree_net_message_endpoint_t client_endpoint =
      OpenEndpoint(client_connection_, client_proactor_, kClientPolling);
  iree_net_message_endpoint_t server_endpoint =
      OpenEndpoint(server_connection_, server_proactor_, kServerPolling);
  ASSERT_NE(client_endpoint.self, nullptr);
  ASSERT_NE(server_endpoint.self, nullptr);

  client_control_messages_.current_poll_side = &current_poll_side_;
  client_control_messages_.expected_poll_side = kClientPolling;
  server_control_messages_.current_poll_side = &current_poll_side_;
  server_control_messages_.expected_poll_side = kServerPolling;
  IREE_ASSERT_OK(iree_net_control_channel_allocate(
      client_endpoint, client_control_messages_.callbacks(),
      iree_allocator_system(), &client_control_channel_));
  IREE_ASSERT_OK(iree_net_control_channel_allocate(
      server_endpoint, server_control_messages_.callbacks(),
      iree_allocator_system(), &server_control_channel_));
  iree_net_control_channel_attach(client_control_channel_);
  iree_net_control_channel_attach(server_control_channel_);
  IREE_ASSERT_OK(iree_net_message_endpoint_activate(client_endpoint));
  IREE_ASSERT_OK(iree_net_message_endpoint_activate(server_endpoint));

  std::string borrowed_payload = "borrowed request";
  iree_async_span_t borrowed_span = iree_async_span_from_ptr(
      borrowed_payload.data(), borrowed_payload.size());
  SendState borrowed_send;
  borrowed_send.current_poll_side = &current_poll_side_;
  borrowed_send.expected_poll_side = kClientPolling;
  borrowed_send.expected_bytes =
      IREE_NET_CONTROL_MESSAGE_HEADER_SIZE + borrowed_payload.size();
  IREE_ASSERT_OK(iree_net_control_channel_send_data(
      client_control_channel_, 3, iree_async_span_list_make(&borrowed_span, 1),
      borrowed_send.callback()));

  std::string copied_prefix(4097, 'p');
  std::string copied_suffix(4096, 's');
  const std::string expected_copy = copied_prefix + copied_suffix;
  iree_async_span_t copied_spans[] = {
      iree_async_span_from_ptr(copied_prefix.data(), copied_prefix.size()),
      iree_async_span_from_ptr(copied_suffix.data(), copied_suffix.size()),
  };
  SendState copied_send;
  copied_send.current_poll_side = &current_poll_side_;
  copied_send.expected_poll_side = kServerPolling;
  copied_send.expected_bytes =
      IREE_NET_CONTROL_MESSAGE_HEADER_SIZE + expected_copy.size();
  IREE_ASSERT_OK(iree_net_control_channel_send_data_copy(
      server_control_channel_, 5, iree_async_span_list_make(copied_spans, 2),
      copied_send.callback()));
  std::fill(copied_prefix.begin(), copied_prefix.end(), 'x');
  std::fill(copied_suffix.begin(), copied_suffix.end(), 'x');

  SendState goaway_send;
  goaway_send.current_poll_side = &current_poll_side_;
  goaway_send.expected_poll_side = kClientPolling;
  goaway_send.expected_bytes = IREE_NET_CONTROL_MESSAGE_HEADER_SIZE;
  IREE_ASSERT_OK(iree_net_control_channel_send_goaway(
      client_control_channel_, 42, goaway_send.callback()));

  PollBothUntil([&] {
    return server_control_messages_.messages.size() == 1 &&
           server_control_messages_.goaway_count == 1 &&
           client_control_messages_.messages.size() == 1 &&
           borrowed_send.callback_count == 1 &&
           copied_send.callback_count == 1 && goaway_send.callback_count == 1;
  });

  EXPECT_EQ(server_control_messages_.messages,
            std::vector<std::string>({"borrowed request"}));
  EXPECT_EQ(server_control_messages_.flags,
            std::vector<iree_net_control_data_flags_t>({3}));
  EXPECT_EQ(server_control_messages_.goaway_reason, 42u);
  EXPECT_EQ(client_control_messages_.messages,
            std::vector<std::string>({expected_copy}));
  EXPECT_EQ(client_control_messages_.flags,
            std::vector<iree_net_control_data_flags_t>({5}));
  EXPECT_EQ(client_control_messages_.error_count, 0);
  EXPECT_EQ(server_control_messages_.error_count, 0);
  EXPECT_EQ(borrowed_send.status_code, IREE_STATUS_OK);
  EXPECT_EQ(copied_send.status_code, IREE_STATUS_OK);
  EXPECT_EQ(goaway_send.status_code, IREE_STATUS_OK);
}

TEST_F(TransportTest, CopiesLargeTransientPrefixWithoutSizeCliff) {
  EstablishConnection();
  iree_net_message_endpoint_t client_endpoint =
      OpenEndpoint(client_connection_, client_proactor_, kClientPolling);
  iree_net_message_endpoint_t server_endpoint =
      OpenEndpoint(server_connection_, server_proactor_, kServerPolling);

  server_messages_.current_poll_side = &current_poll_side_;
  server_messages_.expected_poll_side = kServerPolling;
  iree_net_message_endpoint_set_callbacks(server_endpoint,
                                          server_messages_.callbacks());
  client_messages_.current_poll_side = &current_poll_side_;
  client_messages_.expected_poll_side = kClientPolling;
  iree_net_message_endpoint_set_callbacks(client_endpoint,
                                          client_messages_.callbacks());
  IREE_ASSERT_OK(iree_net_message_endpoint_activate(client_endpoint));
  IREE_ASSERT_OK(iree_net_message_endpoint_activate(server_endpoint));

  std::string prefix(8 * 1024 + 1, 'p');
  const std::string expected = prefix;
  SendState send_state;
  send_state.current_poll_side = &current_poll_side_;
  send_state.expected_poll_side = kClientPolling;
  send_state.expected_bytes = prefix.size();
  iree_net_message_endpoint_send_params_t send_params = {
      /*.copied_prefix=*/
      iree_make_const_byte_span(prefix.data(), prefix.size()),
      /*.data=*/iree_async_span_list_empty(),
      /*.completion_callback=*/send_state.callback(),
  };
  IREE_ASSERT_OK(iree_net_message_endpoint_send(client_endpoint, &send_params));
  std::fill(prefix.begin(), prefix.end(), 'x');

  PollImmediate(client_proactor_, kClientPolling);
  PollUntil(server_proactor_, kServerPolling,
            [&] { return server_messages_.messages.size() == 1; });
  EXPECT_EQ(server_messages_.messages[0], expected);
  PollUntil(client_proactor_, kClientPolling,
            [&] { return send_state.callback_count == 1; });
  EXPECT_EQ(send_state.status_code, IREE_STATUS_OK);
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
