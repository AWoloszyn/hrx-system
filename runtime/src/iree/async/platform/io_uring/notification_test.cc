// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <unistd.h>

#include <cerrno>

#include "iree/async/api.h"
#include "iree/async/platform/io_uring/api.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace {

// Only the selected valid descriptor on the test/poll thread is intercepted.
thread_local int failed_read_descriptor = -1;
thread_local int failed_write_descriptor = -1;
thread_local int read_failures = 0;
thread_local int write_failures = 0;

}  // namespace

extern "C" ssize_t __real_read(int descriptor, void* data, size_t length);
extern "C" ssize_t __wrap_read(int descriptor, void* data, size_t length) {
  if (descriptor == failed_read_descriptor && read_failures == 0) {
    ++read_failures;
    errno = EIO;
    return -1;
  }
  return __real_read(descriptor, data, length);
}

extern "C" ssize_t __real_write(int descriptor, const void* data,
                                size_t length);
extern "C" ssize_t __wrap_write(int descriptor, const void* data,
                                size_t length) {
  if (descriptor == failed_write_descriptor && write_failures == 0) {
    ++write_failures;
    errno = EIO;
    return -1;
  }
  return __real_write(descriptor, data, length);
}

namespace {

class IoUringNotificationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    read_failures = 0;
    write_failures = 0;
    iree_status_t status = iree_async_proactor_create_io_uring(
        iree_async_proactor_options_default(), iree_allocator_system(),
        &proactor_);
    if (iree_status_is_unavailable(status)) {
      iree_status_free(status);
      GTEST_SKIP() << "io_uring is unavailable";
    }
    IREE_ASSERT_OK(status);
    iree_notification_state_initialize(&shared_state_);
    IREE_ASSERT_OK(
        iree_async_notification_native_initialize(&shared_state_, &native_));
    IREE_ASSERT_OK(
        iree_async_notification_create_shared(proactor_, &native_, &source_));
    IREE_ASSERT_OK(iree_async_notification_create(
        proactor_, IREE_ASYNC_NOTIFICATION_FLAG_NONE, &sink_));
    IREE_ASSERT_OK(iree_async_event_native_initialize(&primitive_sink_));
  }

  void TearDown() override {
    failed_read_descriptor = -1;
    failed_write_descriptor = -1;
    int expected_unregistrations = 0;
    for (auto* relay : relays_) {
      if (!relay) {
        continue;
      }
      ++expected_unregistrations;
      iree_async_proactor_unregister_relay(
          proactor_, relay,
          {+[](void* context) { ++*static_cast<int*>(context); },
           &unregistrations_});
    }
    while (unregistrations_ != expected_unregistrations) {
      IREE_ASSERT_OK(iree_async_proactor_poll(
          proactor_, iree_infinite_timeout(), nullptr));
    }
    iree_async_event_native_deinitialize(&primitive_sink_);
    iree_async_notification_release(sink_);
    iree_async_notification_release(source_);
    iree_async_proactor_release(proactor_);
    iree_async_notification_native_deinitialize(&native_);
  }

  void Admit(int index, iree_async_relay_sink_t sink) {
    IREE_ASSERT_OK(iree_async_proactor_register_relay(
        proactor_, iree_async_relay_source_from_notification(source_), sink,
        IREE_ASYNC_RELAY_FLAG_PERSISTENT, {Fault, this}, &relays_[index]));
  }

  void Admit(int index) {
    Admit(index, iree_async_relay_sink_signal_notification(sink_, 1));
  }

  static void Fault(void* context, iree_async_relay_t* relay,
                    iree_status_t status) {
    auto* self = static_cast<IoUringNotificationTest*>(context);
    IREE_EXPECT_STATUS_IS(iree_status_code_from_errno(EIO), status);
    for (int i = 0; i < 3; ++i) {
      if (self->relays_[i] == relay) {
        ++self->fault_counts_[i];
      }
    }
    ++self->fault_count_;
    if (self->fault_count_ == 1 && self->admit_from_fault_) {
      self->Admit(2);
    }
  }

  void PollImmediate() {
    iree_status_t status =
        iree_async_proactor_poll(proactor_, iree_immediate_timeout(), nullptr);
    if (iree_status_is_deadline_exceeded(status)) {
      iree_status_free(status);
    } else {
      IREE_ASSERT_OK(status);
    }
  }

  void CheckSourceFaults() {
    failed_read_descriptor =
        source_->platform.io_uring.event.wait_primitive.value.fd;
    // Fault delivery is software work. A finite set of nonblocking turns
    // verifies that the accepted batch remains scheduled without another wake.
    for (int i = 0; i < 4; ++i) {
      PollImmediate();
    }
    EXPECT_EQ(read_failures, 1);
    EXPECT_EQ(fault_count_, 3);
    for (int count : fault_counts_) {
      EXPECT_EQ(count, 1);
    }
    EXPECT_EQ(iree_async_notification_query_epoch(sink_), 0u);
  }

  // Real poll owner, retained until every explicit unregistration completes.
  iree_async_proactor_t* proactor_ = nullptr;
  // Caller-owned shared state, retained independently of the managed source.
  iree_notification_state_t shared_state_ = {};
  // Valid native resources retained through all accepted consumers.
  iree_async_notification_native_t native_ = {};
  // Managed source retained through fault reporting and unregistration.
  iree_async_notification_t* source_ = nullptr;
  // Notification sink of the callback-admitted replacement.
  iree_async_notification_t* sink_ = nullptr;
  // Valid primitive sink for dependency write failure coverage.
  iree_async_event_native_t primitive_sink_ = {};
  // Persistent caller-visible handles, including the replacement.
  iree_async_relay_t* relays_[3] = {};
  // Fault deliveries indexed by admitted handle.
  int fault_counts_[3] = {};
  // Total fault deliveries, used to admit the replacement only once.
  int fault_count_ = 0;
  // Explicit terminal unregistration completions.
  int unregistrations_ = 0;
  // Whether the first fault callback registers the third consumer.
  bool admit_from_fault_ = false;
};

TEST_F(IoUringNotificationTest, SourceFailureFaultsEveryAdmittedRelayOnce) {
  Admit(0);
  Admit(1);
  Admit(2);
  CheckSourceFaults();
}

TEST_F(IoUringNotificationTest, SourceFailurePreservesFaultCallbackAdmission) {
  admit_from_fault_ = true;
  Admit(0);
  Admit(1);
  CheckSourceFaults();
}

TEST_F(IoUringNotificationTest, SinkFailurePreservesFaultCallbackAdmission) {
  admit_from_fault_ = true;
  auto sink = iree_async_relay_sink_signal_primitive(
      primitive_sink_.signal_primitive, 7);
  Admit(0, sink);
  Admit(1, sink);
  failed_write_descriptor = primitive_sink_.signal_primitive.value.fd;
  iree_async_notification_signal(source_, IREE_ALL_WAITERS);
  while (!fault_count_) {
    IREE_ASSERT_OK(
        iree_async_proactor_poll(proactor_, iree_infinite_timeout(), nullptr));
  }
  EXPECT_EQ(write_failures, 1);
  EXPECT_EQ(fault_count_, 1);
  EXPECT_NE(relays_[2], nullptr);
  EXPECT_EQ(iree_async_notification_query_epoch(sink_), 0u);
  uint64_t value = 0;
  ASSERT_EQ(
      read(primitive_sink_.wait_primitive.value.fd, &value, sizeof(value)),
      sizeof(value));
  EXPECT_EQ(value, 7u);

  for (uint32_t round = 1; round <= 4; ++round) {
    iree_async_notification_signal(source_, IREE_ALL_WAITERS);
    while (iree_async_notification_query_epoch(sink_) < round) {
      IREE_ASSERT_OK(iree_async_proactor_poll(
          proactor_, iree_infinite_timeout(), nullptr));
    }
    EXPECT_EQ(iree_async_notification_query_epoch(sink_), round);
    EXPECT_EQ(fault_count_, 1);
  }
}

}  // namespace
