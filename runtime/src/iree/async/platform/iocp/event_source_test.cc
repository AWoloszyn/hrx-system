// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/async/platform/iocp/event_source.h"

#include "iree/async/event.h"
#include "iree/async/platform/iocp/proactor.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace {

static LONG NTAPI ReturnCancellationResult(HANDLE handle,
                                           BOOLEAN remove_packet) {
  EXPECT_TRUE(remove_packet);
  return *static_cast<LONG*>(handle);
}

TEST(IocpEventSourceTest, ClassifiesNativeWithdrawal) {
  iree_async_proactor_iocp_t proactor = {};
  proactor.nt_wait_api.NtCancelWaitCompletionPacket = ReturnCancellationResult;
  for (LONG result : {0L, 0x103L, static_cast<LONG>(0xC0000120L)}) {
    bool withdrawn = true;
    IREE_ASSERT_OK(iree_async_proactor_iocp_cancel_wait_packet(
        &proactor, reinterpret_cast<uintptr_t>(&result), &withdrawn));
    EXPECT_EQ(withdrawn, result == 0);
  }
  LONG failure = static_cast<LONG>(0xC000000DL);
  bool withdrawn = true;
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INTERNAL,
      iree_async_proactor_iocp_cancel_wait_packet(
          &proactor, reinterpret_cast<uintptr_t>(&failure), &withdrawn));
  EXPECT_FALSE(withdrawn);
}

TEST(IocpEventSourceTest, JoinsDispatchOfDequeuedPacket) {
  iree_async_proactor_t* proactor = nullptr;
  IREE_ASSERT_OK(
      iree_async_proactor_create_iocp(iree_async_proactor_options_default(),
                                      iree_allocator_system(), &proactor));
  auto* iocp = iree_async_proactor_iocp_cast(proactor);
  if (!iocp->nt_wait_api.available) {
    iree_async_proactor_release(proactor);
    GTEST_SKIP() << "wait completion packets unavailable";
  }
  iree_async_event_native_t event = {};
  IREE_ASSERT_OK(iree_async_event_native_initialize(&event));
  int ready_count = 0;
  iree_async_event_source_t* source = nullptr;
  IREE_ASSERT_OK(iree_async_proactor_register_event_source(
      proactor, event.wait_primitive,
      {+[](void* context, iree_async_event_source_t*,
           iree_async_poll_events_t) { ++*static_cast<int*>(context); },
       &ready_count},
      &source));
  iree_async_event_native_set(&event);

  // Hold one real native completion exactly as the poller's batch does. All
  // borrowed resources remain alive through its explicit terminal dispatch.
  OVERLAPPED_ENTRY entry = {};
  ULONG count = 0;
  do {
    ASSERT_TRUE(GetQueuedCompletionStatusEx(
        reinterpret_cast<HANDLE>(iocp->completion_port.handle), &entry, 1,
        &count, INFINITE, FALSE));
  } while (!count || entry.lpCompletionKey !=
                         IREE_ASYNC_IOCP_EVENT_SOURCE_COMPLETION_KEY);
  ASSERT_EQ(reinterpret_cast<void*>(entry.lpOverlapped), source);

  int unregistered_count = 0;
  iree_async_proactor_unregister_event_source(
      proactor, source,
      {+[](void* context) { ++*static_cast<int*>(context); },
       &unregistered_count});
  EXPECT_EQ(unregistered_count, 0);
  iree_async_iocp_event_source_dispatch(iocp, source);
  EXPECT_EQ(unregistered_count, 1);
  EXPECT_EQ(ready_count, 0);
  EXPECT_EQ(iocp->event_sources, nullptr);
  iree_async_event_native_deinitialize(&event);
  iree_async_proactor_release(proactor);
}

}  // namespace
