// Copyright 2026 The HRX Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef HRX_BINDING_HIP_ARRAY_LIFETIME_H_
#define HRX_BINDING_HIP_ARRAY_LIFETIME_H_

#include "iree/base/api.h"
#include "iree/base/threading/mutex.h"
#include "iree/base/threading/notification.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Coordinates active array users with the thread closing the public handle.
typedef struct iree_hip_array_lifetime_t {
  // Serializes closure and active-use accounting.
  iree_slim_mutex_t mutex;
  // Wakes the closing thread when the final active use is released.
  iree_notification_t idle_notification;
  // Number of callers that may still access the array storage.
  iree_host_size_t active_use_count;
  // True after the public handle has stopped accepting new users.
  bool is_closing;
} iree_hip_array_lifetime_t;

void iree_hip_array_lifetime_initialize(iree_hip_array_lifetime_t* lifetime);

// Requires closure to have begun and all active uses to have been released.
void iree_hip_array_lifetime_deinitialize(iree_hip_array_lifetime_t* lifetime);

// Acquires an active-use lease unless closure has begun or the count is full.
bool iree_hip_array_lifetime_try_acquire(iree_hip_array_lifetime_t* lifetime);

// Releases one active-use lease. This never destroys the owning array.
void iree_hip_array_lifetime_release(iree_hip_array_lifetime_t* lifetime);

// Prevents new active-use leases. The caller serializes this transition with
// public-handle lookup and calls it exactly once.
void iree_hip_array_lifetime_begin_close(iree_hip_array_lifetime_t* lifetime);

// Waits until every lease acquired before closure has been released.
void iree_hip_array_lifetime_await_idle(iree_hip_array_lifetime_t* lifetime);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // HRX_BINDING_HIP_ARRAY_LIFETIME_H_
