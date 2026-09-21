// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/async/cts/util/registry.h"
#include "iree/async/cts/util/test_base.h"
#include "iree/async/event.h"
#include "iree/async/proactor.h"

#if !defined(IREE_PLATFORM_WINDOWS)
#include <errno.h>
#include <unistd.h>
#endif

namespace iree::async::cts {
namespace {

struct EventSourceOwner {
  // Independently owned resources borrowed by the registered source.
  iree_async_event_native_t native = {};
  // Number of readiness callbacks consumed.
  int ready_count = 0;
  // Number of terminal callbacks returning native ownership.
  int unregistered_count = 0;

  EventSourceOwner() {
    IREE_CHECK_OK(iree_async_event_native_initialize(&native));
  }
  ~EventSourceOwner() { iree_async_event_native_deinitialize(&native); }

  static void Ready(void* context, iree_async_event_source_t*,
                    iree_async_poll_events_t events) {
    auto* owner = static_cast<EventSourceOwner*>(context);
    EXPECT_TRUE(iree_any_bit_set(events, IREE_ASYNC_POLL_EVENT_IN));
#if !defined(IREE_PLATFORM_WINDOWS)
    uint64_t value = 0;
    ssize_t result;
    do {
      result =
          read(owner->native.wait_primitive.value.fd, &value, sizeof(value));
    } while (result < 0 && errno == EINTR);
    EXPECT_TRUE(result > 0 || (result < 0 && errno == EAGAIN));
#endif
    ++owner->ready_count;
  }

  static void Unregistered(void* context) {
    auto* owner = static_cast<EventSourceOwner*>(context);
    iree_async_event_native_deinitialize(&owner->native);
    ++owner->unregistered_count;
  }
};

class EventSourceTest : public CtsTestBase<> {};

TEST_P(EventSourceTest, NullSourceCompletesInline) {
  int count = 0;
  iree_async_proactor_unregister_event_source(
      proactor_, nullptr,
      {+[](void* context) { ++*static_cast<int*>(context); }, &count});
  EXPECT_EQ(count, 1);
}

TEST_P(EventSourceTest, CompletionReturnsBorrowedNativeOwnership) {
  EventSourceOwner owner;
  iree_async_event_source_t* source = nullptr;
  IREE_ASSERT_OK(iree_async_proactor_register_event_source(
      proactor_, owner.native.wait_primitive, {EventSourceOwner::Ready, &owner},
      &source));
  for (int i = 0; i < 3; ++i) {
    iree_async_event_native_set(&owner.native);
    PollUntilCondition([&] { return owner.ready_count > i; });
  }
  iree_async_proactor_unregister_event_source(
      proactor_, source, {EventSourceOwner::Unregistered, &owner});
  PollUntilCondition([&] { return owner.unregistered_count != 0; });
  EXPECT_EQ(owner.unregistered_count, 1);
  EXPECT_EQ(owner.native.wait_primitive.type, IREE_ASYNC_PRIMITIVE_TYPE_NONE);
}

TEST_P(EventSourceTest, DestructionCompletesAdmittedUnregistration) {
  EventSourceOwner owner;
  iree_async_event_source_t* source = nullptr;
  IREE_ASSERT_OK(iree_async_proactor_register_event_source(
      proactor_, owner.native.wait_primitive, {EventSourceOwner::Ready, &owner},
      &source));
  // Establish native monitoring without requiring a producer to signal.
  iree_async_proactor_wake(proactor_);
  PollOneProgressEvent();
  iree_async_proactor_unregister_event_source(
      proactor_, source, {EventSourceOwner::Unregistered, &owner});
  iree_async_proactor_release(proactor_);
  proactor_ = nullptr;
  EXPECT_EQ(owner.unregistered_count, 1);
  EXPECT_EQ(owner.ready_count, 0);
}

#if defined(IREE_PLATFORM_WINDOWS)
CTS_REGISTER_TEST_SUITE_WITH_TAGS(EventSourceTest, {"wait_completion_packet"},
                                  {});
#else
CTS_REGISTER_TEST_SUITE(EventSourceTest);
#endif

}  // namespace
}  // namespace iree::async::cts
