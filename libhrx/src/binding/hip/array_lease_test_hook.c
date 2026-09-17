// Copyright 2026 The HRX Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "binding/hip/api.h"
#include "iree/base/threading/call_once.h"
#include "iree/base/threading/mutex.h"

#if !defined(HRX_ENABLE_ARRAY_LEASE_TEST_HOOK)
#error "array lease test hook source is test-only"
#endif

static iree_once_flag iree_hip_array_lease_observer_mutex_once =
    IREE_ONCE_FLAG_INIT;
static iree_slim_mutex_t iree_hip_array_lease_observer_mutex;
static hipHostFn_t iree_hip_array_lease_observer = NULL;
static void* iree_hip_array_lease_observer_user_data = NULL;

static void iree_hip_array_lease_observer_mutex_initialize(void) {
  iree_slim_mutex_initialize(&iree_hip_array_lease_observer_mutex);
}

HIPAPI hipError_t hipHRXSetArrayLeaseObserverForTesting(hipHostFn_t observer,
                                                        void* user_data) {
  iree_call_once(&iree_hip_array_lease_observer_mutex_once,
                 iree_hip_array_lease_observer_mutex_initialize);
  iree_slim_mutex_lock(&iree_hip_array_lease_observer_mutex);
  iree_hip_array_lease_observer_user_data = user_data;
  iree_hip_array_lease_observer = observer;
  iree_slim_mutex_unlock(&iree_hip_array_lease_observer_mutex);
  return hipSuccess;
}

void iree_hip_notify_array_lease_observer(void) {
  iree_call_once(&iree_hip_array_lease_observer_mutex_once,
                 iree_hip_array_lease_observer_mutex_initialize);
  iree_slim_mutex_lock(&iree_hip_array_lease_observer_mutex);
  hipHostFn_t observer = iree_hip_array_lease_observer;
  void* user_data = iree_hip_array_lease_observer_user_data;
  iree_slim_mutex_unlock(&iree_hip_array_lease_observer_mutex);
  if (observer) observer(user_data);
}
