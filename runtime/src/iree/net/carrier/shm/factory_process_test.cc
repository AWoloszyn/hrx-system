// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <stdio.h>
#include <stdlib.h>

#include <algorithm>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "iree/async/notification_native.h"
#include "iree/async/proactor_platform.h"
#include "iree/base/alignment.h"
#include "iree/net/carrier/shm/factory.h"
#include "iree/net/connection.h"
#include "iree/testing/coordinated_test.h"
#include "iree/testing/gtest.h"

#if defined(IREE_PLATFORM_WINDOWS)
#include <windows.h>
#else
#include <errno.h>
#include <unistd.h>
#if defined(IREE_PLATFORM_APPLE)
#include <sys/event.h>
#else
#include <poll.h>
#include <sys/syscall.h>
#endif
#endif

namespace {

// Role failures must fail the child process, not just a gtest object in the
// launcher. The outer coordinated harness owns the hang timeout.
void Check(bool condition, const char* expression, int line) {
  if (!condition) {
    fprintf(stderr, "SHM role failed at line %d: %s\n", line, expression);
    abort();
  }
}
#define ROLE_CHECK(expression) Check((expression), #expression, __LINE__)

void CheckStatus(iree_status_t status) {
  if (!iree_status_is_ok(status)) {
    iree_status_fprint(stderr, status);
    iree_status_free(status);
    abort();
  }
}

std::string Address(const char* directory) {
#if defined(IREE_PLATFORM_WINDOWS)
  std::string path = directory;
  return path.substr(path.find_last_of("\\/") + 1) + "-shm";
#else
  return std::string(directory) + "/shm.sock";
#endif
}

constexpr size_t kLengths[] = {64, 4089, 4090, 4097, 8192, 16389, 1024 * 1024};
constexpr size_t kPrefixLength = 65537;

struct Messages {
  // Copied values used for protocol coordination and initial content checks.
  std::vector<std::string> values;
  // Retained byte views into leases, checked again after process/owner exit.
  std::vector<iree_const_byte_span_t> views;
  // Moved receive ownership, including native slots and assembled messages.
  std::vector<iree_async_buffer_lease_t> leases;
  // First terminal failure count from peer process departure.
  int error_count = 0;

  static iree_status_t Receive(void* user_data, iree_const_byte_span_t message,
                               iree_async_buffer_lease_t* lease) {
    auto& self = *static_cast<Messages*>(user_data);
    ROLE_CHECK(lease != nullptr);
    self.values.emplace_back(reinterpret_cast<const char*>(message.data),
                             message.data_length);
    self.views.push_back(message);
    self.leases.push_back(*lease);
    *lease = {};
    return iree_ok_status();
  }

  static void Error(void* user_data, iree_status_t status) {
    auto& self = *static_cast<Messages*>(user_data);
    ROLE_CHECK(!iree_status_is_ok(status));
    ROLE_CHECK(++self.error_count == 1);
    iree_status_free(status);
  }
};

struct Peer {
  // Owned poll executor for the real native factory and message endpoint.
  iree_async_proactor_t* proactor = nullptr;
  // Owned connection produced by OFFER/ACCEPT/READY.
  iree_net_connection_t* connection = nullptr;
  // Protocol control and retained-payload endpoints.
  iree_net_message_endpoint_t endpoints[2] = {};
  // Receive ownership independent of the connection and executor.
  Messages messages[2];

  void PollUntil(const std::function<bool()>& condition) {
    while (!condition()) {
      CheckStatus(
          iree_async_proactor_poll(proactor, iree_infinite_timeout(), nullptr));
    }
  }

  static void Connected(void* user_data, iree_status_t status,
                        iree_net_connection_t* connection) {
    auto& self = *static_cast<Peer*>(user_data);
    CheckStatus(status);
    ROLE_CHECK(self.connection == nullptr);
    self.connection = connection;
  }

  void OpenEndpoints() {
    for (int i = 0; i < 2; ++i) {
      CheckStatus(iree_net_connection_open_endpoint(
          connection, {+[](void* user_data, iree_status_t status,
                           iree_net_message_endpoint_t endpoint) {
                         CheckStatus(status);
                         *static_cast<iree_net_message_endpoint_t*>(user_data) =
                             endpoint;
                       },
                       &endpoints[i]}));
      PollUntil([&] { return endpoints[i].self != nullptr; });
      iree_net_message_endpoint_set_callbacks(
          endpoints[i], {Messages::Receive, Messages::Error, &messages[i]});
      CheckStatus(iree_net_message_endpoint_activate(endpoints[i]));
    }
  }

  void Send(int ordinal, std::string& source, std::string prefix = {}) {
    struct Completion {
      // Completion synchronizing source reuse after the final receipt.
      bool done = false;
      // Borrowed source overwritten as soon as the transport returns ownership.
      std::string* source;
    } completion{false, &source};
    auto span = iree_async_span_from_ptr(source.data(), source.size());
    iree_net_message_endpoint_send_params_t params = {
        iree_net_send_prefix_from_bytes(
            iree_make_const_byte_span(prefix.data(), prefix.size())),
        {&span, 1},
        {+[](void* user_data, iree_status_t status, iree_host_size_t) {
           auto& completion = *static_cast<Completion*>(user_data);
           CheckStatus(status);
           ROLE_CHECK(!completion.done);
           std::fill(completion.source->begin(), completion.source->end(), 'x');
           completion.done = true;
         },
         &completion}};
    CheckStatus(iree_net_message_endpoint_send(endpoints[ordinal], &params));
    std::fill(prefix.begin(), prefix.end(), 'x');
    PollUntil([&] { return completion.done; });
  }

  void Drain() {
    bool drained = false;
    iree_net_connection_deactivate(
        connection,
        {+[](void* user_data) { *static_cast<bool*>(user_data) = true; },
         &drained});
    PollUntil([&] { return drained; });
    iree_net_connection_release(connection);
    connection = nullptr;
    iree_async_proactor_release(proactor);
    proactor = nullptr;
  }
};

iree_net_transport_factory_t* CreateFactory(Peer& peer) {
  CheckStatus(iree_async_proactor_create_platform(
      iree_async_proactor_options_default(), iree_allocator_system(),
      &peer.proactor));
  auto options = iree_net_shm_factory_options_default();
  options.region = {2, 4, 4097};
  options.max_pending_connections = 2;
  iree_net_transport_factory_t* factory = nullptr;
  CheckStatus(
      iree_net_shm_factory_create(&options, iree_allocator_system(), &factory));
  return factory;
}

int Server(int, char**, const char* directory) {
  Peer peer;
  auto* factory = CreateFactory(peer);
  std::string address = Address(directory);
  iree_net_listener_t* listener = nullptr;
  CheckStatus(iree_net_transport_factory_create_listener(
      factory, iree_make_string_view(address.data(), address.size()),
      peer.proactor, nullptr, {Peer::Connected, &peer}, iree_allocator_system(),
      &listener));
  iree_net_transport_factory_release(factory);
  iree_coordinated_test_signal_ready(directory);
  peer.PollUntil([&] { return peer.connection != nullptr; });
  bool stopped = false;
  CheckStatus(iree_net_listener_stop(
      listener,
      {+[](void* user_data) { *static_cast<bool*>(user_data) = true; },
       &stopped}));
  peer.PollUntil([&] { return stopped; });
  iree_net_listener_free(listener);
  peer.OpenEndpoints();
#if defined(IREE_PLATFORM_WINDOWS)
  uint64_t process_id = GetCurrentProcessId();
#else
  uint64_t process_id = getpid();
#endif
  std::string identity(sizeof(process_id), '\0');
  iree_unaligned_store_le_u64(identity.data(), process_id);
  peer.Send(0, identity);
  for (size_t i = 0; i < IREE_ARRAYSIZE(kLengths); ++i) {
    std::string source(kLengths[i], static_cast<char>('a' + i));
    peer.Send(1, source);
  }
  std::string source(4097, 'z');
  peer.Send(1, source, std::string(kPrefixLength, 'p'));
  std::string done = "all-published";
  peer.Send(0, done);
  peer.PollUntil([&] { return !peer.messages[0].values.empty(); });
  ROLE_CHECK(peer.messages[0].values[0] == "exit");
  // Model process loss, not orderly connection deactivation. The kernel closes
  // all exporter resources; the independent receiver still owns moved leases.
  _Exit(0);
}

int Client(int, char**, const char* directory) {
  Peer peer;
  auto* factory = CreateFactory(peer);
  std::string address = Address(directory);
  iree_net_transport_connect_operation_t operation;
  iree_net_transport_connect_operation_initialize(&operation);
  CheckStatus(iree_net_transport_factory_connect(
      factory, iree_make_string_view(address.data(), address.size()),
      peer.proactor, nullptr, {Peer::Connected, &peer}, &operation));
  iree_net_transport_factory_release(factory);
  peer.PollUntil([&] { return peer.connection != nullptr; });
  iree_net_transport_connect_operation_deinitialize(&operation);
  peer.OpenEndpoints();
  peer.PollUntil([&] { return peer.messages[0].values.size() == 2; });
  ROLE_CHECK(peer.messages[0].values[1] == "all-published");
  ROLE_CHECK(peer.messages[1].values.size() == IREE_ARRAYSIZE(kLengths) + 1);
  ROLE_CHECK(peer.messages[0].values[0].size() == sizeof(uint64_t));
  uint64_t process_id =
      iree_unaligned_load_le_u64(peer.messages[0].values[0].data());
  // This identity is only a test exit witness, never resource-import authority.
  // Register before ACK permits the exporter to leave, avoiding PID reuse.
#if defined(IREE_PLATFORM_WINDOWS)
  HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)process_id);
  ROLE_CHECK(process != nullptr);
#elif defined(IREE_PLATFORM_APPLE)
  int process = kqueue();
  ROLE_CHECK(process >= 0);
  struct kevent change;
  EV_SET(&change, process_id, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0,
         nullptr);
  ROLE_CHECK(kevent(process, &change, 1, nullptr, 0, nullptr) == 0);
#else
  int process = (int)syscall(SYS_pidfd_open, (pid_t)process_id, 0);
  ROLE_CHECK(process >= 0);
#endif

  std::string exit_message = "exit";
  auto span =
      iree_async_span_from_ptr(exit_message.data(), exit_message.size());
  bool sent = false;
  iree_net_message_endpoint_send_params_t params = {
      {},
      {&span, 1},
      {+[](void* user_data, iree_status_t status, iree_host_size_t) {
         // Peer EOF can win observation of an already-published receipt.
         ROLE_CHECK(iree_status_is_ok(status) ||
                    iree_status_is_out_of_range(status) ||
                    iree_status_is_unavailable(status));
         iree_status_free(status);
         *static_cast<bool*>(user_data) = true;
       },
       &sent}};
  CheckStatus(iree_net_message_endpoint_send(peer.endpoints[0], &params));
  peer.PollUntil([&] {
    return sent && peer.messages[0].error_count && peer.messages[1].error_count;
  });
#if defined(IREE_PLATFORM_WINDOWS)
  ROLE_CHECK(WaitForSingleObject(process, INFINITE) == WAIT_OBJECT_0);
  ROLE_CHECK(CloseHandle(process));
#elif defined(IREE_PLATFORM_APPLE)
  struct kevent event;
  ROLE_CHECK(kevent(process, nullptr, 0, &event, 1, nullptr) == 1);
  ROLE_CHECK((event.fflags & NOTE_EXIT) != 0);
  ROLE_CHECK(close(process) == 0);
#else
  pollfd event = {process, POLLIN, 0};
  int result;
  do {
    result = poll(&event, 1, -1);
  } while (result < 0 && errno == EINTR);
  ROLE_CHECK(result == 1 && (event.revents & POLLIN));
  ROLE_CHECK(close(process) == 0);
#endif
  peer.Drain();
  for (size_t i = 0; i < peer.messages[1].views.size(); ++i) {
    std::string expected =
        i < IREE_ARRAYSIZE(kLengths)
            ? std::string(kLengths[i], static_cast<char>('a' + i))
            : std::string(kPrefixLength, 'p') + std::string(4097, 'z');
    auto view = peer.messages[1].views[i];
    ROLE_CHECK(std::string(reinterpret_cast<const char*>(view.data),
                           view.data_length) == expected);
  }
  std::vector<std::thread> returns;
  for (auto& messages : peer.messages) {
    for (auto& lease : messages.leases) {
      returns.emplace_back(
          [&lease] { iree_async_buffer_lease_release(&lease); });
    }
  }
  for (auto& thread : returns) {
    thread.join();
  }
  return 0;
}

const iree_test_role_t kRoles[] = {
    {"server", Server, true},
    {"client", Client, false},
};
const iree_coordinated_test_config_t kConfig = {kRoles, IREE_ARRAYSIZE(kRoles)};
IREE_COORDINATED_TEST_REGISTER(kConfig);

TEST(ShmFactoryProcessTest, RetainedMessagesSurviveExporterProcessLoss) {
  if (!iree_async_notification_native_is_supported()) {
    GTEST_SKIP();
  }
  EXPECT_EQ(iree_coordinated_test_run(iree_coordinated_test_argc(),
                                      iree_coordinated_test_argv(), &kConfig),
            0);
}

}  // namespace
