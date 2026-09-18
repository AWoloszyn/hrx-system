// Copyright 2026 The HRX Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "binding/hip/array_lifetime.h"

#include <stdint.h>

void iree_hip_array_lifetime_initialize(iree_hip_array_lifetime_t* lifetime) {
  IREE_ASSERT_ARGUMENT(lifetime);
  iree_slim_mutex_initialize(&lifetime->mutex);
  iree_notification_initialize(&lifetime->idle_notification);
  lifetime->active_use_count = 0;
  lifetime->is_closing = false;
}

void iree_hip_array_lifetime_deinitialize(iree_hip_array_lifetime_t* lifetime) {
  IREE_ASSERT_ARGUMENT(lifetime);
  IREE_ASSERT_TRUE(lifetime->is_closing);
  IREE_ASSERT_EQ(lifetime->active_use_count, 0);
  iree_notification_deinitialize(&lifetime->idle_notification);
  iree_slim_mutex_deinitialize(&lifetime->mutex);
}

bool iree_hip_array_lifetime_try_acquire(iree_hip_array_lifetime_t* lifetime) {
  IREE_ASSERT_ARGUMENT(lifetime);
  iree_slim_mutex_lock(&lifetime->mutex);
  const bool acquired =
      !lifetime->is_closing && lifetime->active_use_count != SIZE_MAX;
  if (acquired) {
    ++lifetime->active_use_count;
  }
  iree_slim_mutex_unlock(&lifetime->mutex);
  return acquired;
}

void iree_hip_array_lifetime_release(iree_hip_array_lifetime_t* lifetime) {
  IREE_ASSERT_ARGUMENT(lifetime);
  iree_slim_mutex_lock(&lifetime->mutex);
  IREE_ASSERT_GT(lifetime->active_use_count, 0);
  --lifetime->active_use_count;
  if (lifetime->active_use_count == 0 && lifetime->is_closing) {
    // Post before dropping the mutex. An awakened waiter must acquire this
    // mutex to observe zero, ensuring this post has returned before teardown.
    iree_notification_post(&lifetime->idle_notification, IREE_ALL_WAITERS);
  }
  iree_slim_mutex_unlock(&lifetime->mutex);
}

void iree_hip_array_lifetime_begin_close(iree_hip_array_lifetime_t* lifetime) {
  IREE_ASSERT_ARGUMENT(lifetime);
  iree_slim_mutex_lock(&lifetime->mutex);
  IREE_ASSERT_FALSE(lifetime->is_closing);
  lifetime->is_closing = true;
  iree_slim_mutex_unlock(&lifetime->mutex);
}

static bool iree_hip_array_lifetime_is_idle(void* user_data) {
  iree_hip_array_lifetime_t* lifetime = (iree_hip_array_lifetime_t*)user_data;
  iree_slim_mutex_lock(&lifetime->mutex);
  const bool is_idle = lifetime->active_use_count == 0;
  iree_slim_mutex_unlock(&lifetime->mutex);
  return is_idle;
}

void iree_hip_array_lifetime_await_idle(iree_hip_array_lifetime_t* lifetime) {
  IREE_ASSERT_ARGUMENT(lifetime);
  IREE_ASSERT_TRUE(lifetime->is_closing);
  iree_notification_await(&lifetime->idle_notification,
                          iree_hip_array_lifetime_is_idle, lifetime,
                          iree_infinite_timeout());
}
