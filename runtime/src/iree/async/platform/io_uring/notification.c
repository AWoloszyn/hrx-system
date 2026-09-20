// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/async/platform/io_uring/notification.h"

#include <errno.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "iree/async/platform/io_uring/proactor.h"
#include "iree/async/proactor.h"
#include "iree/base/threading/futex.h"

//===----------------------------------------------------------------------===//
// Creation and destruction
//===----------------------------------------------------------------------===//

iree_status_t iree_async_io_uring_notification_create(
    iree_async_proactor_io_uring_t* proactor,
    iree_async_notification_flags_t flags,
    iree_async_notification_t** out_notification) {
  IREE_TRACE_ZONE_BEGIN(z0);
  IREE_ASSERT_ARGUMENT(out_notification);
  *out_notification = NULL;

  iree_allocator_t allocator = proactor->base.allocator;

  iree_async_notification_t* notification = NULL;
  IREE_RETURN_AND_END_ZONE_IF_ERROR(
      z0, iree_allocator_malloc(allocator, sizeof(*notification),
                                (void**)&notification));
  memset(notification, 0, sizeof(*notification));

  iree_atomic_ref_count_init(&notification->ref_count);
  notification->proactor = &proactor->base;
  iree_atomic_store(&notification->epoch, 0, iree_memory_order_release);
  notification->epoch_ptr = &notification->epoch;
  notification->flags = IREE_ASYNC_NOTIFICATION_FLAG_NONE;

  // Use eventfd-backed notifications for now. io_uring FUTEX_WAIT has no
  // userspace-visible "wait is armed" edge, so a signal racing immediately
  // after relay registration can miss the in-kernel waiter. eventfd is
  // level-triggered and preserves that register-before-signal contract.
  iree_status_t status = iree_ok_status();
  notification->mode = IREE_ASYNC_NOTIFICATION_MODE_EVENT;
  // EFD_SEMAPHORE makes read() decrement by 1 instead of draining the
  // counter. This allows wake_count to control how many waiters are woken.
  int eventfd_result = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK | EFD_SEMAPHORE);
  if (eventfd_result >= 0) {
    iree_async_primitive_t event_primitive =
        iree_async_primitive_from_fd(eventfd_result);
    notification->platform.io_uring.primitive = event_primitive;
    notification->platform.io_uring.signal_primitive = event_primitive;
    notification->platform.io_uring.drain_buffer = 0;
  } else {
    status = iree_make_status(iree_status_code_from_errno(errno),
                              "eventfd creation failed (%d)", errno);
  }

  if (iree_status_is_ok(status)) {
    *out_notification = notification;
  } else {
    iree_allocator_free(allocator, notification);
  }
  IREE_TRACE_ZONE_END(z0);
  return status;
}

iree_status_t iree_async_io_uring_notification_create_shared(
    iree_async_proactor_io_uring_t* proactor,
    const iree_async_notification_shared_options_t* options,
    iree_async_notification_t** out_notification) {
  IREE_TRACE_ZONE_BEGIN(z0);
  IREE_ASSERT_ARGUMENT(options);
  IREE_ASSERT_ARGUMENT(options->epoch_address);
  IREE_ASSERT_ARGUMENT(out_notification);
  *out_notification = NULL;

  iree_allocator_t allocator = proactor->base.allocator;

  iree_async_notification_t* notification = NULL;
  IREE_RETURN_AND_END_ZONE_IF_ERROR(
      z0, iree_allocator_malloc(allocator, sizeof(*notification),
                                (void**)&notification));
  memset(notification, 0, sizeof(*notification));

  iree_atomic_ref_count_init(&notification->ref_count);
  notification->proactor = &proactor->base;
  notification->epoch_ptr = options->epoch_address;
  notification->flags = IREE_ASYNC_NOTIFICATION_FLAG_SHARED;

  notification->mode = IREE_ASYNC_NOTIFICATION_MODE_EVENT;
  // Use caller-provided primitives instead of creating our own eventfd.
  // For proxy notifications (peer wake): wake_primitive may be NONE (not
  // polled locally) while signal_primitive is the peer's eventfd.
  notification->platform.io_uring.primitive = options->wake_primitive;
  notification->platform.io_uring.signal_primitive = options->signal_primitive;
  notification->platform.io_uring.drain_buffer = 0;

  *out_notification = notification;
  IREE_TRACE_ZONE_END(z0);
  return iree_ok_status();
}

void iree_async_io_uring_notification_destroy(
    iree_async_proactor_io_uring_t* proactor,
    iree_async_notification_t* notification) {
  IREE_TRACE_ZONE_BEGIN(z0);
  if (!notification) {
    IREE_TRACE_ZONE_END(z0);
    return;
  }

  iree_allocator_t allocator = proactor->base.allocator;

  if (notification->mode == IREE_ASYNC_NOTIFICATION_MODE_EVENT &&
      !iree_any_bit_set(notification->flags,
                        IREE_ASYNC_NOTIFICATION_FLAG_SHARED)) {
    if (notification->platform.io_uring.primitive.value.fd >= 0) {
      close(notification->platform.io_uring.primitive.value.fd);
    }
  }

  iree_allocator_free(allocator, notification);
  IREE_TRACE_ZONE_END(z0);
}

//===----------------------------------------------------------------------===//
// Vtable implementations for signal and wait
//===----------------------------------------------------------------------===//

void iree_async_io_uring_notification_signal(
    iree_async_proactor_t* base_proactor,
    iree_async_notification_t* notification, int32_t wake_count) {
  // With EFD_SEMAPHORE, write(N) allows N read()s to succeed.
  // Write to signal_primitive — for local notifications this is the same
  // eventfd as primitive; for shared/proxy notifications it's the peer's fd.
  uint64_t value = (wake_count > 0) ? (uint64_t)wake_count : UINT32_MAX;
  ssize_t result;
  do {
    result = write(notification->platform.io_uring.signal_primitive.value.fd,
                   &value, sizeof(value));
  } while (result < 0 && errno == EINTR);
  // A full nonblocking eventfd is already readable. Other failures indicate
  // that the native primitive was released before the notification.
  IREE_ASSERT(result == sizeof(value) || (result < 0 && errno == EAGAIN),
              "eventfd write failed during notification signal: %zd (errno=%d)",
              result, errno);

#if defined(IREE_PLATFORM_HAS_FUTEX)
  // Synchronous waiters have their own native wake channel. They never consume
  // eventfd readiness belonging to asynchronous waits and relays.
  if (iree_any_bit_set(notification->flags,
                       IREE_ASYNC_NOTIFICATION_FLAG_SHARED)) {
    iree_futex_wake_shared(notification->epoch_ptr, wake_count);
  } else {
    iree_futex_wake(notification->epoch_ptr, wake_count);
  }
#endif  // IREE_PLATFORM_HAS_FUTEX
}

bool iree_async_io_uring_notification_wait(
    iree_async_proactor_t* base_proactor,
    iree_async_notification_t* notification, uint32_t wait_token,
    iree_timeout_t timeout) {
  iree_time_t deadline_ns = iree_timeout_as_deadline_ns(timeout);
  while (iree_time_now() < deadline_ns) {
    uint32_t current_epoch =
        iree_atomic_load(notification->epoch_ptr, iree_memory_order_acquire);
    if (current_epoch != wait_token) {
      return true;
    }

#if defined(IREE_PLATFORM_HAS_FUTEX)
    iree_status_code_t status_code =
        iree_any_bit_set(notification->flags,
                         IREE_ASYNC_NOTIFICATION_FLAG_SHARED)
            ? iree_futex_wait_shared(notification->epoch_ptr, wait_token,
                                     deadline_ns)
            : iree_futex_wait(notification->epoch_ptr, wait_token, deadline_ns);
    if (status_code == IREE_STATUS_DEADLINE_EXCEEDED) {
      break;
    }
#endif  // IREE_PLATFORM_HAS_FUTEX
  }

  uint32_t final_epoch =
      iree_atomic_load(notification->epoch_ptr, iree_memory_order_acquire);
  return final_epoch != wait_token;
}
