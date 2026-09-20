// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <unistd.h>

#include <cerrno>
#include <cstdint>

#include "iree/async/api.h"
#include "iree/async/platform/posix/api.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace {

// Interpose only the selected descriptor on the calling test/poll thread.
// The backend remains the real linked implementation; later writes reach the
// native eventfd. Other proactor and test-runner writes are unaffected.
thread_local int interrupted_fd = -1;
thread_local int intercepted_write_count = 0;
thread_local int intercepted_error = EINTR;

}  // namespace

extern "C" ssize_t __real_write(int fd, const void* data, size_t length);
extern "C" ssize_t __wrap_write(int fd, const void* data, size_t length) {
  if (fd == interrupted_fd && ++intercepted_write_count == 1) {
    errno = intercepted_error;
    return -1;
  }
  return __real_write(fd, data, length);
}

namespace {

class NativeEventWakeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    intercepted_write_count = 0;
    intercepted_error = EINTR;
    IREE_ASSERT_OK(iree_async_event_native_initialize(&event_));
  }

  void TearDown() override {
    interrupted_fd = -1;
    iree_async_event_native_deinitialize(&event_);
  }

  // Native resources retained through publication and readiness observation.
  iree_async_event_native_t event_ = {};
};

TEST_F(NativeEventWakeTest, InterruptedPublicationRetainsReadiness) {
  interrupted_fd = event_.signal_primitive.value.fd;
  iree_async_event_native_set(&event_);
  EXPECT_EQ(intercepted_write_count, 2);
  uint64_t count = 0;
  EXPECT_EQ(read(event_.wait_primitive.value.fd, &count, sizeof(count)),
            sizeof(count));
  EXPECT_EQ(count, 1u);
}

TEST_F(NativeEventWakeTest, InterruptedPublicationPreservesCoalescing) {
  uint64_t saturated_count = UINT64_MAX - 1;
  ASSERT_EQ(write(event_.signal_primitive.value.fd, &saturated_count,
                  sizeof(saturated_count)),
            sizeof(saturated_count));
  interrupted_fd = event_.signal_primitive.value.fd;
  iree_async_event_native_set(&event_);
  EXPECT_EQ(intercepted_write_count, 2);
  uint64_t count = 0;
  EXPECT_EQ(read(event_.wait_primitive.value.fd, &count, sizeof(count)),
            sizeof(count));
  EXPECT_EQ(count, saturated_count);
}

class NotificationWakeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    intercepted_write_count = 0;
    intercepted_error = EINTR;
    IREE_ASSERT_OK(
        iree_async_proactor_create_posix(iree_async_proactor_options_default(),
                                         iree_allocator_system(), &proactor_));
    IREE_ASSERT_OK(iree_async_notification_create(
        proactor_, IREE_ASYNC_NOTIFICATION_FLAG_NONE, &notification_));
  }

  void TearDown() override {
    interrupted_fd = -1;
    iree_async_notification_release(notification_);
    iree_async_proactor_release(proactor_);
  }

  // Poll owner for the real POSIX backend.
  iree_async_proactor_t* proactor_ = nullptr;
  // Source notification retained through each wake or relay observation.
  iree_async_notification_t* notification_ = nullptr;
};

TEST_F(NotificationWakeTest, InterruptedSignalPublishesOnce) {
  interrupted_fd =
      notification_->platform.posix.event.signal_primitive.value.fd;
  uint32_t epoch = iree_async_notification_query_epoch(notification_);
  iree_async_notification_signal(notification_, IREE_ALL_WAITERS);
  EXPECT_EQ(intercepted_write_count, 2);
  EXPECT_EQ(iree_async_notification_query_epoch(notification_), epoch + 1);
  uint64_t count = 0;
  EXPECT_EQ(read(interrupted_fd, &count, sizeof(count)), sizeof(count));
  EXPECT_EQ(count, 1u);
}

TEST_F(NotificationWakeTest, InterruptedSignalPreservesCoalescing) {
  int fd = notification_->platform.posix.event.signal_primitive.value.fd;
  uint64_t saturated_count = UINT64_MAX - 1;
  ASSERT_EQ(write(fd, &saturated_count, sizeof(saturated_count)),
            sizeof(saturated_count));
  interrupted_fd = fd;
  uint32_t epoch = iree_async_notification_query_epoch(notification_);
  iree_async_notification_signal(notification_, IREE_ALL_WAITERS);
  EXPECT_EQ(intercepted_write_count, 2);
  EXPECT_EQ(iree_async_notification_query_epoch(notification_), epoch + 1);
  uint64_t count = 0;
  EXPECT_EQ(read(fd, &count, sizeof(count)), sizeof(count));
  EXPECT_EQ(count, saturated_count);
}

TEST_F(NotificationWakeTest, InterruptedRelayPreservesPayload) {
  iree_async_event_native_t sink = {};
  IREE_ASSERT_OK(iree_async_event_native_initialize(&sink));
  iree_status_code_t error_code = IREE_STATUS_OK;
  iree_async_relay_error_callback_t on_error = {
      +[](void* user_data, iree_async_relay_t*, iree_status_t status) {
        *static_cast<iree_status_code_t*>(user_data) = iree_status_code(status);
        iree_status_free(status);
      },
      &error_code,
  };
  iree_async_relay_t* relay = nullptr;
  iree_status_t status = iree_async_proactor_register_relay(
      proactor_, iree_async_relay_source_from_notification(notification_),
      iree_async_relay_sink_signal_primitive(sink.signal_primitive, 7),
      IREE_ASYNC_RELAY_FLAG_PERSISTENT, on_error, &relay);
  if (iree_status_is_ok(status)) {
    interrupted_fd = sink.signal_primitive.value.fd;
    iree_async_notification_signal(notification_, IREE_ALL_WAITERS);
    uint64_t count = 0;
    ssize_t length = 0;
    do {
      status =
          iree_async_proactor_poll(proactor_, iree_infinite_timeout(), nullptr);
      length = read(sink.wait_primitive.value.fd, &count, sizeof(count));
    } while (iree_status_is_ok(status) && error_code == IREE_STATUS_OK &&
             length < 0 && errno == EAGAIN);
    EXPECT_EQ(error_code, IREE_STATUS_OK);
    EXPECT_EQ(intercepted_write_count, 2);
    EXPECT_EQ(length, sizeof(count));
    EXPECT_EQ(count, 7u);
    iree_async_proactor_unregister_relay(
        proactor_, relay, iree_async_relay_unregistered_callback_none());
  }
  interrupted_fd = -1;
  iree_async_event_native_deinitialize(&sink);
  IREE_ASSERT_OK(status);
}

TEST_F(NotificationWakeTest, FaultCallbackAdmissionSurvivesOtherConsumers) {
  iree_async_event_native_t primitive_sink = {};
  IREE_ASSERT_OK(iree_async_event_native_initialize(&primitive_sink));
  struct State {
    // Poll owner for callback-driven admission.
    iree_async_proactor_t* proactor;
    // Retained source used by all relay generations.
    iree_async_notification_t* source;
    // Replacement relay sink retained through terminal unregistration.
    iree_async_notification_t* sink = nullptr;
    // Persistent replacement admitted by the fault callback.
    iree_async_relay_t* replacement = nullptr;
    // Number of observed terminal faults.
    int faults = 0;
    static void Fault(void* context, iree_async_relay_t*,
                      iree_status_t status) {
      auto* state = static_cast<State*>(context);
      IREE_EXPECT_STATUS_IS(iree_status_code_from_errno(EIO), status);
      ++state->faults;
      IREE_ASSERT_OK(iree_async_proactor_register_relay(
          state->proactor,
          iree_async_relay_source_from_notification(state->source),
          iree_async_relay_sink_signal_notification(state->sink, 1),
          IREE_ASYNC_RELAY_FLAG_PERSISTENT,
          iree_async_relay_error_callback_none(), &state->replacement));
    }
  } state = {proactor_, notification_};
  IREE_ASSERT_OK(iree_async_notification_create(
      proactor_, IREE_ASYNC_NOTIFICATION_FLAG_NONE, &state.sink));
  // Both native resources stay valid. Only the first dependency write reports
  // failure; the other one-shot consumer still delivers its payload normally.
  iree_async_relay_t* relay = nullptr;
  for (int i = 0; i < 2; ++i) {
    IREE_ASSERT_OK(iree_async_proactor_register_relay(
        proactor_, iree_async_relay_source_from_notification(notification_),
        iree_async_relay_sink_signal_primitive(primitive_sink.signal_primitive,
                                               7),
        IREE_ASYNC_RELAY_FLAG_NONE, {State::Fault, &state}, &relay));
  }
  intercepted_error = EIO;
  interrupted_fd = primitive_sink.signal_primitive.value.fd;
  iree_async_notification_signal(notification_, IREE_ALL_WAITERS);
  while (!state.faults) {
    IREE_ASSERT_OK(
        iree_async_proactor_poll(proactor_, iree_infinite_timeout(), nullptr));
  }
  EXPECT_EQ(state.faults, 1);
  EXPECT_EQ(intercepted_write_count, 2);
  EXPECT_EQ(iree_async_notification_query_epoch(state.sink), 0u);
  uint64_t value = 0;
  EXPECT_EQ(read(primitive_sink.wait_primitive.value.fd, &value, sizeof(value)),
            sizeof(value));
  EXPECT_EQ(value, 7u);
  for (uint32_t epoch = 1; epoch <= 4; ++epoch) {
    iree_async_notification_signal(notification_, IREE_ALL_WAITERS);
    while (iree_async_notification_query_epoch(state.sink) < epoch) {
      IREE_ASSERT_OK(iree_async_proactor_poll(
          proactor_, iree_infinite_timeout(), nullptr));
    }
    EXPECT_EQ(iree_async_notification_query_epoch(state.sink), epoch);
  }
  iree_async_proactor_unregister_relay(
      proactor_, state.replacement,
      iree_async_relay_unregistered_callback_none());
  iree_async_notification_release(state.sink);
  interrupted_fd = -1;
  iree_async_event_native_deinitialize(&primitive_sink);
}

}  // namespace
