// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/async/notification_native.h"

#include "iree/base/threading/futex.h"

#if defined(IREE_PLATFORM_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(IREE_PLATFORM_APPLE)
#if __has_include(<os/os_sync_wait_on_address.h>)
#include <errno.h>
#include <os/os_sync_wait_on_address.h>
#define IREE_ASYNC_HAVE_APPLE_SHARED_WAIT 1
#endif
#endif  // IREE_PLATFORM_*

IREE_API_EXPORT bool iree_async_notification_native_is_supported(void) {
  if (!iree_atomic_int64_is_lock_free()) {
    return false;
  }
#if defined(IREE_PLATFORM_WINDOWS) ||                                    \
    ((defined(IREE_PLATFORM_LINUX) || defined(IREE_PLATFORM_ANDROID)) && \
     defined(IREE_PLATFORM_HAS_FUTEX))
  return true;
#elif defined(IREE_ASYNC_HAVE_APPLE_SHARED_WAIT)
  if (__builtin_available(macOS 14.4, iOS 17.4, tvOS 17.4, watchOS 10.4, *)) {
    return true;
  }
  return false;
#else
  return false;
#endif  // native shared waits
}

static iree_status_t iree_async_notification_native_validate_state(
    iree_notification_state_t* state) {
  if (!state || (uintptr_t)state % sizeof(uint64_t)) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "shared notification state must be eight-byte aligned");
  }
  if (!iree_async_notification_native_is_supported()) {
    return iree_make_status(IREE_STATUS_UNAVAILABLE,
                            "native shared notification waits are unavailable");
  }
  return iree_ok_status();
}

IREE_API_EXPORT void iree_async_notification_native_deinitialize(
    iree_async_notification_native_t* notification) {
#if defined(IREE_PLATFORM_WINDOWS)
  iree_async_event_native_deinitialize(&notification->synchronous.event);
#endif
  iree_async_event_native_deinitialize(&notification->async_event);
  memset(notification, 0, sizeof(*notification));
}

IREE_API_EXPORT iree_status_t iree_async_notification_native_initialize(
    iree_notification_state_t* state,
    iree_async_notification_native_t* out_notification) {
  memset(out_notification, 0, sizeof(*out_notification));
  IREE_RETURN_IF_ERROR(iree_async_notification_native_validate_state(state));
  iree_status_t status =
      iree_async_event_native_initialize(&out_notification->async_event);
#if defined(IREE_PLATFORM_WINDOWS)
  if (iree_status_is_ok(status)) {
    status = iree_async_event_native_initialize(
        &out_notification->synchronous.event);
  }
#endif
  if (iree_status_is_ok(status)) {
    out_notification->state = state;
  } else {
    iree_async_notification_native_deinitialize(out_notification);
  }
  return status;
}

IREE_API_EXPORT iree_status_t iree_async_notification_native_import(
    iree_notification_state_t* state,
    iree_async_primitive_t handles[IREE_ASYNC_NOTIFICATION_NATIVE_HANDLE_COUNT],
    iree_async_notification_native_t* out_notification) {
  memset(out_notification, 0, sizeof(*out_notification));
  iree_status_t status = iree_async_notification_native_validate_state(state);
  for (uint32_t i = 0; i < IREE_ASYNC_NOTIFICATION_NATIVE_HANDLE_COUNT &&
                       iree_status_is_ok(status);
       ++i) {
#if defined(IREE_PLATFORM_WINDOWS)
    bool valid = handles[i].type == IREE_ASYNC_PRIMITIVE_TYPE_WIN32_HANDLE &&
                 handles[i].value.win32_handle != 0;
#elif defined(IREE_ASYNC_HAVE_FD)
    bool valid = handles[i].type == IREE_ASYNC_PRIMITIVE_TYPE_FD &&
                 handles[i].value.fd >= 0;
#else
    bool valid = false;
#endif
    if (!valid) {
      status = iree_make_status(
          IREE_STATUS_INVALID_ARGUMENT,
          "shared notification has an invalid native resource");
    }
  }
  if (iree_status_is_ok(status)) {
    out_notification->state = state;
    out_notification->async_event.wait_primitive = handles[0];
#if defined(IREE_ASYNC_HAVE_EVENTFD) || defined(IREE_PLATFORM_WINDOWS)
    out_notification->async_event.signal_primitive = handles[0];
#else
    out_notification->async_event.signal_primitive = handles[1];
#endif
#if defined(IREE_PLATFORM_WINDOWS)
    out_notification->synchronous.event.wait_primitive = handles[1];
    out_notification->synchronous.event.signal_primitive = handles[1];
#endif
  }
  for (uint32_t i = 0; i < IREE_ASYNC_NOTIFICATION_NATIVE_HANDLE_COUNT; ++i) {
    if (!iree_status_is_ok(status)) {
      iree_async_primitive_close(&handles[i]);
    }
    handles[i] = iree_async_primitive_none();
  }
  return status;
}

IREE_API_EXPORT void iree_async_notification_native_export(
    const iree_async_notification_native_t* notification,
    iree_async_primitive_t
        out_handles[IREE_ASYNC_NOTIFICATION_NATIVE_HANDLE_COUNT]) {
  out_handles[0] = notification->async_event.wait_primitive;
#if defined(IREE_PLATFORM_WINDOWS)
  out_handles[1] = notification->synchronous.event.wait_primitive;
#elif !defined(IREE_ASYNC_HAVE_EVENTFD)
  out_handles[1] = notification->async_event.signal_primitive;
#endif
}

// Creation/import establishes availability once. The public Apple functions
// remain weak-linked for older deployment targets; no per-publication version
// check or process-global function table is required.
#if defined(IREE_ASYNC_HAVE_APPLE_SHARED_WAIT)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunguarded-availability-new"
#endif

IREE_API_EXPORT void iree_async_notification_native_signal(
    iree_async_notification_native_t* notification, int32_t wake_count) {
  uint32_t waiter_count = iree_notification_state_post(notification->state);
  iree_async_event_native_set(&notification->async_event);
  if (!waiter_count || !wake_count) {
    return;
  }
#if defined(IREE_PLATFORM_WINDOWS)
  iree_async_event_native_set(&notification->synchronous.event);
#elif (defined(IREE_PLATFORM_LINUX) || defined(IREE_PLATFORM_ANDROID)) && \
    defined(IREE_PLATFORM_HAS_FUTEX)
  iree_futex_wake_shared(
      iree_notification_state_epoch_address(notification->state), wake_count);
#elif defined(IREE_ASYNC_HAVE_APPLE_SHARED_WAIT)
  void* address = iree_notification_state_epoch_address(notification->state);
  int result =
      wake_count == 1
          ? os_sync_wake_by_address_any(address, sizeof(uint32_t),
                                        OS_SYNC_WAKE_BY_ADDRESS_SHARED)
          : os_sync_wake_by_address_all(address, sizeof(uint32_t),
                                        OS_SYNC_WAKE_BY_ADDRESS_SHARED);
  // Enrollment may observe publication before entering the native wait.
  IREE_ASSERT(result == 0 || errno == ENOENT,
              "shared notification wake contract violated");
#else
  IREE_ASSERT_UNREACHABLE("native shared waits are unavailable");
#endif
}

#if defined(IREE_PLATFORM_WINDOWS)

static void iree_async_notification_native_wait(
    iree_async_notification_native_t* notification, uint32_t wait_token,
    iree_time_t deadline_ns) {
  for (;;) {
    // Capture handoff before the epoch so an owner exiting between this check
    // and WaitOnAddress cannot leave a follower sleeping on a newer generation.
    uint64_t handoff = iree_atomic_load(&notification->synchronous.handoff,
                                        iree_memory_order_acquire);
    if (iree_notification_state_query_epoch(notification->state) !=
        wait_token) {
      break;
    }
    uint32_t timeout_ms = iree_absolute_deadline_to_timeout_ms(deadline_ns);
    if (!timeout_ms) {
      break;
    }
    if (!(handoff & 1u)) {
      if (!iree_atomic_compare_exchange_weak(
              &notification->synchronous.handoff, &handoff, handoff | 1u,
              iree_memory_order_acq_rel, iree_memory_order_acquire)) {
        continue;
      }
      if (iree_notification_state_query_epoch(notification->state) ==
          wait_token) {
        DWORD result =
            WaitForSingleObject((HANDLE)notification->synchronous.event
                                    .wait_primitive.value.win32_handle,
                                timeout_ms);
        IREE_ASSERT(result == WAIT_OBJECT_0 || result == WAIT_TIMEOUT,
                    "shared notification wait contract violated");
      }
      // Both completion and timeout relinquish native waiting to local
      // followers.
      iree_atomic_fetch_add(&notification->synchronous.handoff, 1,
                            iree_memory_order_release);
      WakeByAddressAll(&notification->synchronous.handoff);
    } else {
      BOOL result = WaitOnAddress(&notification->synchronous.handoff, &handoff,
                                  sizeof(handoff), timeout_ms);
      IREE_ASSERT(result || GetLastError() == ERROR_TIMEOUT,
                  "shared notification handoff contract violated");
    }
  }
}

#else

static void iree_async_notification_native_wait(
    iree_async_notification_native_t* notification, uint32_t wait_token,
    iree_time_t deadline_ns) {
  while (iree_notification_state_query_epoch(notification->state) ==
         wait_token) {
    if (deadline_ns != IREE_TIME_INFINITE_FUTURE &&
        iree_time_now() >= deadline_ns) {
      break;
    }
    void* address = iree_notification_state_epoch_address(notification->state);
#if (defined(IREE_PLATFORM_LINUX) || defined(IREE_PLATFORM_ANDROID)) && \
    defined(IREE_PLATFORM_HAS_FUTEX)
    iree_status_code_t result =
        iree_futex_wait_shared(address, wait_token, deadline_ns);
    if (result == IREE_STATUS_DEADLINE_EXCEEDED) {
      break;
    }
    IREE_ASSERT(result == IREE_STATUS_OK,
                "shared notification wait contract violated");
#elif defined(IREE_ASYNC_HAVE_APPLE_SHARED_WAIT)
    int result;
    if (deadline_ns == IREE_TIME_INFINITE_FUTURE) {
      result = os_sync_wait_on_address(address, wait_token, sizeof(uint32_t),
                                       OS_SYNC_WAIT_ON_ADDRESS_SHARED);
    } else {
      iree_duration_t remaining_ns = deadline_ns - iree_time_now();
      if (remaining_ns <= 0) {
        break;
      }
      result = os_sync_wait_on_address_with_timeout(
          address, wait_token, sizeof(uint32_t), OS_SYNC_WAIT_ON_ADDRESS_SHARED,
          OS_CLOCK_MACH_ABSOLUTE_TIME, (uint64_t)remaining_ns);
    }
    if (result < 0 && errno == ETIMEDOUT) {
      break;
    }
    // These native early returns require rechecking the epoch before retrying.
    IREE_ASSERT(
        result >= 0 || errno == EINTR || errno == ENOMEM || errno == EFAULT,
        "shared notification wait contract violated");
#else
    (void)address;
    IREE_ASSERT_UNREACHABLE("native shared waits are unavailable");
    break;
#endif
  }
}

#endif  // IREE_PLATFORM_WINDOWS

#if defined(IREE_ASYNC_HAVE_APPLE_SHARED_WAIT)
#pragma clang diagnostic pop
#endif

IREE_API_EXPORT bool iree_async_notification_native_wait_for_token(
    iree_async_notification_native_t* notification, uint32_t wait_token,
    iree_timeout_t timeout) {
  if (iree_notification_state_query_epoch(notification->state) != wait_token) {
    return true;
  }
  iree_notification_state_prepare_wait(notification->state);
  iree_async_notification_native_wait(notification, wait_token,
                                      iree_timeout_as_deadline_ns(timeout));
  iree_notification_state_cancel_wait(notification->state);
  return iree_notification_state_query_epoch(notification->state) != wait_token;
}
