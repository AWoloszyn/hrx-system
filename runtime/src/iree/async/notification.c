// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/async/notification.h"

#include "iree/async/proactor.h"

IREE_API_EXPORT iree_status_t iree_async_notification_create(
    iree_async_proactor_t* proactor, iree_async_notification_flags_t flags,
    iree_async_notification_t** out_notification) {
  IREE_ASSERT_ARGUMENT(proactor);
  IREE_ASSERT_ARGUMENT(out_notification);
  *out_notification = NULL;
  IREE_TRACE_ZONE_BEGIN(z0);
  iree_status_t status =
      proactor->vtable->create_notification(proactor, flags, out_notification);
  IREE_TRACE_ZONE_END(z0);
  return status;
}

IREE_API_EXPORT iree_status_t iree_async_notification_create_shared(
    iree_async_proactor_t* proactor, iree_async_notification_native_t* native,
    iree_async_notification_t** out_notification) {
  IREE_ASSERT_ARGUMENT(proactor);
  IREE_ASSERT_ARGUMENT(out_notification);
  *out_notification = NULL;
  IREE_TRACE_ZONE_BEGIN(z0);
  iree_status_t status = proactor->vtable->create_notification_shared(
      proactor, native, out_notification);
  IREE_TRACE_ZONE_END(z0);
  return status;
}

IREE_API_EXPORT void iree_async_notification_retain(
    iree_async_notification_t* notification) {
  if (notification) {
    iree_atomic_ref_count_inc(&notification->ref_count);
  }
}

IREE_API_EXPORT void iree_async_notification_release(
    iree_async_notification_t* notification) {
  if (notification &&
      iree_atomic_ref_count_dec(&notification->ref_count) == 1) {
    notification->proactor->vtable->destroy_notification(notification->proactor,
                                                         notification);
  }
}

IREE_API_EXPORT uint32_t
iree_async_notification_begin_observe(iree_async_notification_t* notification) {
  if (!notification->shared_native) {
    iree_atomic_fetch_add(&notification->observer_count, 1,
                          iree_memory_order_acq_rel);
  }
  return iree_async_notification_query_epoch(notification);
}

IREE_API_EXPORT void iree_async_notification_end_observe(
    iree_async_notification_t* notification) {
  if (!notification->shared_native) {
    iree_atomic_fetch_sub(&notification->observer_count, 1,
                          iree_memory_order_acq_rel);
  }
}

IREE_API_EXPORT void iree_async_notification_signal(
    iree_async_notification_t* notification, int32_t wake_count) {
  IREE_TRACE_ZONE_BEGIN(z0);

  if (notification->shared_native) {
    iree_async_notification_native_signal(notification->shared_native,
                                          wake_count);
  } else {
    iree_atomic_fetch_add(&notification->epoch, 1, iree_memory_order_release);
    notification->proactor->vtable->notification_signal(
        notification->proactor, notification, wake_count);
  }

  IREE_TRACE_ZONE_END(z0);
}

IREE_API_EXPORT bool iree_async_notification_signal_if_observed(
    iree_async_notification_t* notification, int32_t wake_count) {
  // Always advance the epoch so a waiter that has observed the token and is
  // between condition re-check and platform wait cannot miss the release. The
  // expensive platform wake is conditional on a known observer for local
  // notifications. Shared peers have independent observer counts, so their
  // absence cannot be established from this handle.
  if (notification->shared_native) {
    iree_async_notification_native_signal(notification->shared_native,
                                          wake_count);
    return true;
  }
  iree_atomic_fetch_add(&notification->epoch, 1, iree_memory_order_release);
  if (iree_atomic_load(&notification->observer_count,
                       iree_memory_order_acquire) <= 0) {
    return false;
  }
  IREE_TRACE_ZONE_BEGIN(z0);
  notification->proactor->vtable->notification_signal(notification->proactor,
                                                      notification, wake_count);
  IREE_TRACE_ZONE_END(z0);
  return true;
}

IREE_API_EXPORT bool iree_async_notification_wait(
    iree_async_notification_t* notification, iree_timeout_t timeout) {
  const uint32_t wait_token =
      iree_async_notification_begin_observe(notification);
  const bool signaled =
      iree_async_notification_wait_for_token(notification, wait_token, timeout);
  iree_async_notification_end_observe(notification);
  return signaled;
}

IREE_API_EXPORT bool iree_async_notification_wait_for_token(
    iree_async_notification_t* notification, uint32_t wait_token,
    iree_timeout_t timeout) {
  IREE_TRACE_ZONE_BEGIN(z0);
  bool signaled =
      notification->shared_native
          ? iree_async_notification_native_wait_for_token(
                notification->shared_native, wait_token, timeout)
          : notification->proactor->vtable->notification_wait(
                notification->proactor, notification, wait_token, timeout);
  IREE_TRACE_ZONE_END(z0);
  return signaled;
}
