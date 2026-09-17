// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/carrier/tcp/factory.h"

#include <array>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "iree/async/address.h"
#include "iree/async/proactor_platform.h"
#include "iree/net/connection.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace iree {
namespace {

struct ConnectState {
  int callback_count = 0;
  std::vector<iree_status_code_t> status_codes;
  std::vector<iree_net_connection_t*> connections;

  static void OnConnect(void* user_data, iree_status_t status,
                        iree_net_connection_t* connection) {
    auto* self = static_cast<ConnectState*>(user_data);
    ++self->callback_count;
    self->status_codes.push_back(iree_status_code(status));
    self->connections.push_back(connection);
    iree_status_free(status);
  }

  iree_net_transport_connect_callback_t callback() {
    return {/*.fn=*/OnConnect, /*.user_data=*/this};
  }
};

struct StopState {
  int* sequence = nullptr;
  int completed_sequence = 0;
  bool completed = false;

  static void OnStopped(void* user_data) {
    auto* self = static_cast<StopState*>(user_data);
    if (self->sequence) {
      self->completed_sequence = ++*self->sequence;
    }
    self->completed = true;
  }

  iree_net_listener_stopped_callback_t callback() {
    return {/*.fn=*/OnStopped, /*.user_data=*/this};
  }
};

struct AcceptState {
  iree_net_listener_t* listener = nullptr;
  StopState* stop_state = nullptr;
  int stop_after_accept_count = 0;
  int* sequence = nullptr;
  int last_accept_sequence = 0;
  int callback_count = 0;
  iree_status_code_t stop_status_code = IREE_STATUS_UNKNOWN;
  std::vector<iree_status_code_t> status_codes;
  std::vector<iree_net_connection_t*> connections;

  static void OnAccept(void* user_data, iree_status_t status,
                       iree_net_connection_t* connection) {
    auto* self = static_cast<AcceptState*>(user_data);
    ++self->callback_count;
    if (self->sequence) {
      self->last_accept_sequence = ++*self->sequence;
    }
    self->status_codes.push_back(iree_status_code(status));
    self->connections.push_back(connection);
    iree_status_free(status);

    if (self->stop_after_accept_count == self->callback_count) {
      EXPECT_NE(self->listener, nullptr);
      EXPECT_NE(self->stop_state, nullptr);
      iree_status_t stop_status =
          iree_net_listener_stop(self->listener, self->stop_state->callback());
      self->stop_status_code = iree_status_code(stop_status);
      iree_status_free(stop_status);
      EXPECT_FALSE(self->stop_state->completed);
    }
  }

  iree_net_listener_accept_callback_t callback() {
    return {/*.fn=*/OnAccept, /*.user_data=*/this};
  }
};

struct DeactivateState {
  bool completed = false;

  static void OnDeactivated(void* user_data) {
    static_cast<DeactivateState*>(user_data)->completed = true;
  }

  iree_net_connection_deactivate_callback_t callback() {
    return {/*.fn=*/OnDeactivated, /*.user_data=*/this};
  }
};

struct ConcurrentStopState {
  std::mutex mutex;
  std::condition_variable condition;
  iree_net_listener_t* listener = nullptr;
  iree_net_connection_t* connection = nullptr;
  iree_status_code_t accept_status_code = IREE_STATUS_UNKNOWN;
  iree_status_code_t stop_status_code = IREE_STATUS_UNKNOWN;
  bool accept_entered = false;
  bool stop_returned = false;
  bool stopped = false;

  static void OnAccept(void* user_data, iree_status_t status,
                       iree_net_connection_t* connection) {
    auto* self = static_cast<ConcurrentStopState*>(user_data);
    std::unique_lock<std::mutex> lock(self->mutex);
    self->accept_status_code = iree_status_code(status);
    self->connection = connection;
    iree_status_free(status);
    self->accept_entered = true;
    self->condition.notify_all();
    self->condition.wait(lock, [&] { return self->stop_returned; });
  }

  static void OnStopped(void* user_data) {
    auto* self = static_cast<ConcurrentStopState*>(user_data);
    std::lock_guard<std::mutex> lock(self->mutex);
    self->stopped = true;
    self->condition.notify_all();
  }

  iree_net_listener_accept_callback_t accept_callback() {
    return {/*.fn=*/OnAccept, /*.user_data=*/this};
  }

  iree_net_listener_stopped_callback_t stopped_callback() {
    return {/*.fn=*/OnStopped, /*.user_data=*/this};
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

class TcpFactoryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    IREE_ASSERT_OK(iree_async_proactor_create_platform(
        iree_async_proactor_options_default(), iree_allocator_system(),
        &proactor_));
    IREE_ASSERT_OK(iree_net_tcp_factory_create(
        /*options=*/nullptr, iree_allocator_system(), &factory_));
  }

  void TearDown() override {
    StopAndFreeListener();
    for (iree_net_connection_t*& connection : connections_) {
      DeactivateAndRelease(connection);
    }
    iree_net_transport_factory_release(factory_);
    iree_async_proactor_release(proactor_);
  }

  void Poll() {
    IREE_ASSERT_OK(iree_async_proactor_poll(proactor_, iree_infinite_timeout(),
                                            /*out_completed_count=*/nullptr));
  }

  void PollUntil(const std::function<bool()>& condition) {
    while (!condition()) {
      Poll();
    }
  }

  std::string CreateListener(
      iree_net_listener_accept_callback_t accept_callback,
      iree_allocator_t host_allocator = iree_allocator_system()) {
    IREE_EXPECT_OK(iree_net_transport_factory_create_listener(
        factory_, IREE_SV("127.0.0.1:0"), proactor_,
        /*receive_pool=*/nullptr, accept_callback, host_allocator, &listener_));
    if (!listener_) {
      return {};
    }

    std::array<char, IREE_ASYNC_ADDRESS_MAX_FORMAT_LENGTH> storage = {};
    iree_string_view_t address = iree_string_view_empty();
    IREE_EXPECT_OK(iree_net_listener_query_bound_address(
        listener_, storage.size(), storage.data(), &address));
    return std::string(address.data, address.size);
  }

  std::string CreateListener(
      AcceptState* accept_state,
      iree_allocator_t host_allocator = iree_allocator_system()) {
    std::string address =
        CreateListener(accept_state->callback(), host_allocator);
    accept_state->listener = listener_;
    return address;
  }

  void StopAndFreeListener() {
    if (!listener_) {
      return;
    }
    StopState stop_state;
    IREE_ASSERT_OK(iree_net_listener_stop(listener_, stop_state.callback()));
    PollUntil([&] { return stop_state.completed; });
    iree_net_listener_free(listener_);
    listener_ = nullptr;
  }

  void DeactivateAndRelease(iree_net_connection_t*& connection) {
    if (!connection) {
      return;
    }
    DeactivateState state;
    iree_net_connection_deactivate(connection, state.callback());
    if (!state.completed) {
      PollUntil([&] { return state.completed; });
    }
    iree_net_connection_release(connection);
    connection = nullptr;
  }

  iree_async_proactor_t* proactor_ = nullptr;
  iree_net_transport_factory_t* factory_ = nullptr;
  iree_net_listener_t* listener_ = nullptr;
  std::vector<iree_net_connection_t*> connections_;
};

TEST_F(TcpFactoryTest, ValidatesOptions) {
  iree_net_tcp_factory_options_t options =
      iree_net_tcp_factory_options_default();
  iree_net_transport_factory_t* invalid_factory = nullptr;

  options.connection_options.max_endpoint_count = 0;
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      iree_net_tcp_factory_create(&options, iree_allocator_system(),
                                  &invalid_factory));
  EXPECT_EQ(invalid_factory, nullptr);

  options = iree_net_tcp_factory_options_default();
  options.receive_buffer_size = 0;
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      iree_net_tcp_factory_create(&options, iree_allocator_system(),
                                  &invalid_factory));
  EXPECT_EQ(invalid_factory, nullptr);

  options = iree_net_tcp_factory_options_default();
  options.receive_buffer_count = 3;
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      iree_net_tcp_factory_create(&options, iree_allocator_system(),
                                  &invalid_factory));
  EXPECT_EQ(invalid_factory, nullptr);

  options = iree_net_tcp_factory_options_default();
  options.receive_buffer_count = IREE_NET_TCP_MAX_RECEIVE_BUFFER_COUNT * 2u;
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_OUT_OF_RANGE,
      iree_net_tcp_factory_create(&options, iree_allocator_system(),
                                  &invalid_factory));
  EXPECT_EQ(invalid_factory, nullptr);

  options = iree_net_tcp_factory_options_default();
  options.receive_buffer_size = IREE_HOST_SIZE_MAX;
  options.receive_buffer_count = 2;
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_OUT_OF_RANGE,
      iree_net_tcp_factory_create(&options, iree_allocator_system(),
                                  &invalid_factory));
  EXPECT_EQ(invalid_factory, nullptr);
}

TEST_F(TcpFactoryTest, RejectsInvalidAddressesSynchronously) {
  ConnectState connect_state;
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      iree_net_transport_factory_connect(
          factory_, IREE_SV("localhost:1234"), proactor_,
          /*receive_pool=*/nullptr, connect_state.callback()));
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      iree_net_transport_factory_connect(
          factory_, IREE_SV("127.0.0.1:99999"), proactor_,
          /*receive_pool=*/nullptr, connect_state.callback()));
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      iree_net_transport_factory_connect(
          factory_, IREE_SV("unix:tcp-is-not-unix"), proactor_,
          /*receive_pool=*/nullptr, connect_state.callback()));
  EXPECT_EQ(connect_state.callback_count, 0);

  AcceptState accept_state;
  iree_net_listener_t* invalid_listener = nullptr;
  IREE_EXPECT_STATUS_IS(IREE_STATUS_INVALID_ARGUMENT,
                        iree_net_transport_factory_create_listener(
                            factory_, iree_string_view_empty(), proactor_,
                            /*receive_pool=*/nullptr, accept_state.callback(),
                            iree_allocator_system(), &invalid_listener));
  EXPECT_EQ(invalid_listener, nullptr);
}

TEST_F(TcpFactoryTest, ReportsEphemeralBoundAddress) {
  AcceptState accept_state;
  std::string address = CreateListener(&accept_state);
  EXPECT_FALSE(address.empty());

  iree_async_address_t parsed_address;
  IREE_EXPECT_OK(iree_async_address_from_string(
      iree_make_string_view(address.data(), address.size()), &parsed_address));

  char tiny_storage[1];
  iree_string_view_t tiny_address = iree_string_view_empty();
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_OUT_OF_RANGE,
      iree_net_listener_query_bound_address(listener_, sizeof(tiny_storage),
                                            tiny_storage, &tiny_address));
  EXPECT_TRUE(iree_string_view_is_empty(tiny_address));
}

TEST_F(TcpFactoryTest, RepeatedAcceptAndStopFromCallback) {
  int sequence = 0;
  StopState stop_state;
  stop_state.sequence = &sequence;
  AcceptState accept_state;
  accept_state.stop_state = &stop_state;
  accept_state.stop_after_accept_count = 2;
  accept_state.sequence = &sequence;
  std::string address = CreateListener(&accept_state);

  ConnectState connect_state;
  iree_string_view_t address_view =
      iree_make_string_view(address.data(), address.size());
  IREE_ASSERT_OK(iree_net_transport_factory_connect(
      factory_, address_view, proactor_, /*receive_pool=*/nullptr,
      connect_state.callback()));
  IREE_ASSERT_OK(iree_net_transport_factory_connect(
      factory_, address_view, proactor_, /*receive_pool=*/nullptr,
      connect_state.callback()));

  PollUntil([&] {
    return connect_state.callback_count == 2 &&
           accept_state.callback_count == 2 && stop_state.completed;
  });
  EXPECT_EQ(accept_state.stop_status_code, IREE_STATUS_OK);
  EXPECT_LT(accept_state.last_accept_sequence, stop_state.completed_sequence);
  ASSERT_EQ(connect_state.connections.size(), 2u);
  ASSERT_EQ(accept_state.connections.size(), 2u);
  for (iree_status_code_t status_code : connect_state.status_codes) {
    EXPECT_EQ(status_code, IREE_STATUS_OK);
  }
  for (iree_status_code_t status_code : accept_state.status_codes) {
    EXPECT_EQ(status_code, IREE_STATUS_OK);
  }
  for (iree_net_connection_t* connection : connect_state.connections) {
    ASSERT_NE(connection, nullptr);
    connections_.push_back(connection);
  }
  for (iree_net_connection_t* connection : accept_state.connections) {
    ASSERT_NE(connection, nullptr);
    connections_.push_back(connection);
  }

  iree_net_listener_free(listener_);
  listener_ = nullptr;
}

TEST_F(TcpFactoryTest, StopRacesWithAcceptCallbackAcrossThreads) {
  ConcurrentStopState stop_state;
  std::string address = CreateListener(stop_state.accept_callback());
  stop_state.listener = listener_;

  ConnectState connect_state;
  IREE_ASSERT_OK(iree_net_transport_factory_connect(
      factory_, iree_make_string_view(address.data(), address.size()),
      proactor_, /*receive_pool=*/nullptr, connect_state.callback()));

  std::thread stop_thread([&] {
    {
      std::unique_lock<std::mutex> lock(stop_state.mutex);
      stop_state.condition.wait(lock,
                                [&] { return stop_state.accept_entered; });
    }
    iree_status_t status = iree_net_listener_stop(
        stop_state.listener, stop_state.stopped_callback());
    {
      std::lock_guard<std::mutex> lock(stop_state.mutex);
      stop_state.stop_status_code = iree_status_code(status);
      iree_status_free(status);
      stop_state.stop_returned = true;
    }
    stop_state.condition.notify_all();
  });

  PollUntil([&] {
    std::lock_guard<std::mutex> lock(stop_state.mutex);
    return stop_state.accept_entered;
  });
  stop_thread.join();
  PollUntil([&] {
    std::lock_guard<std::mutex> lock(stop_state.mutex);
    return stop_state.stopped && connect_state.callback_count == 1;
  });

  EXPECT_EQ(stop_state.accept_status_code, IREE_STATUS_OK);
  EXPECT_EQ(stop_state.stop_status_code, IREE_STATUS_OK);
  ASSERT_NE(stop_state.connection, nullptr);
  ASSERT_EQ(connect_state.status_codes.size(), 1u);
  EXPECT_EQ(connect_state.status_codes[0], IREE_STATUS_OK);
  ASSERT_EQ(connect_state.connections.size(), 1u);
  ASSERT_NE(connect_state.connections[0], nullptr);
  connections_.push_back(stop_state.connection);
  connections_.push_back(connect_state.connections[0]);

  iree_net_listener_free(listener_);
  listener_ = nullptr;
}

TEST_F(TcpFactoryTest, ConnectAllocationFailureIsSynchronous) {
  iree_net_transport_factory_release(factory_);
  ControlledAllocator allocator;
  IREE_ASSERT_OK(iree_net_tcp_factory_create(
      /*options=*/nullptr, allocator.value(), &factory_));
  allocator.allocations_before_failure = 0;

  ConnectState connect_state;
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_RESOURCE_EXHAUSTED,
      iree_net_transport_factory_connect(
          factory_, IREE_SV("127.0.0.1:1"), proactor_,
          /*receive_pool=*/nullptr, connect_state.callback()));
  EXPECT_EQ(connect_state.callback_count, 0);
  iree_net_transport_factory_release(factory_);
  factory_ = nullptr;
}

TEST_F(TcpFactoryTest, AcceptedConnectionAllocationFailureLeavesListenerLive) {
  ControlledAllocator listener_allocator;
  AcceptState accept_state;
  std::string address =
      CreateListener(&accept_state, listener_allocator.value());
  listener_allocator.allocations_before_failure = 0;

  ConnectState connect_state;
  IREE_ASSERT_OK(iree_net_transport_factory_connect(
      factory_, iree_make_string_view(address.data(), address.size()),
      proactor_, /*receive_pool=*/nullptr, connect_state.callback()));
  PollUntil([&] {
    return connect_state.callback_count == 1 &&
           accept_state.callback_count == 1;
  });
  ASSERT_EQ(connect_state.status_codes.size(), 1u);
  EXPECT_EQ(connect_state.status_codes[0], IREE_STATUS_OK);
  ASSERT_EQ(connect_state.connections.size(), 1u);
  ASSERT_NE(connect_state.connections[0], nullptr);
  connections_.push_back(connect_state.connections[0]);
  ASSERT_EQ(accept_state.status_codes.size(), 1u);
  EXPECT_EQ(accept_state.status_codes[0], IREE_STATUS_RESOURCE_EXHAUSTED);
  EXPECT_EQ(accept_state.connections[0], nullptr);

  StopAndFreeListener();
}

}  // namespace
}  // namespace iree
