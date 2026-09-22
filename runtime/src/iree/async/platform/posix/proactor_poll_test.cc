// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/async/platform/posix/proactor.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace {

TEST(PosixProactorPollTest, InterruptedWaitDoesNotExpireCallerDeadline) {
  iree_async_proactor_t* proactor = nullptr;
  IREE_ASSERT_OK(
      iree_async_proactor_create_posix(iree_async_proactor_options_default(),
                                       iree_allocator_system(), &proactor));

  // Inject the event-set's documented EINTR outcome at the native dependency
  // boundary. No signal timing or companion thread is needed to exercise the
  // real proactor's return path.
  auto* event_set = iree_async_proactor_posix_cast(proactor)->event_set;
  const auto* original_vtable = event_set->vtable;
  auto interrupted_vtable = *original_vtable;
  interrupted_vtable.wait =
      +[](iree_async_posix_event_set_t*, int timeout_ms,
          iree_host_size_t* ready_count, bool* timed_out) -> iree_status_t {
    EXPECT_EQ(timeout_ms, -1);
    *ready_count = 0;
    *timed_out = false;
    return iree_ok_status();
  };
  event_set->vtable = &interrupted_vtable;

  iree_host_size_t completed = 99;
  IREE_EXPECT_OK(
      iree_async_proactor_poll(proactor, iree_infinite_timeout(), &completed));
  EXPECT_EQ(completed, 0u);

  event_set->vtable = original_vtable;
  iree_async_proactor_release(proactor);
}

}  // namespace
