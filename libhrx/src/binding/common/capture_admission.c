// Copyright 2026 The HRX Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "common/capture_admission.h"

static const uint32_t iree_hal_streaming_capture_admission_transition_bit =
    UINT32_C(1) << 31;
static const uint32_t iree_hal_streaming_capture_admission_count_mask =
    (UINT32_C(1) << 31) - 1;

static bool iree_hal_streaming_capture_admission_is_open(void* user_data) {
  iree_hal_streaming_capture_admission_t* admission =
      (iree_hal_streaming_capture_admission_t*)user_data;
  const uint32_t state =
      iree_atomic_load(&admission->state, iree_memory_order_acquire);
  return (state & iree_hal_streaming_capture_admission_transition_bit) == 0;
}

static bool iree_hal_streaming_capture_admission_is_idle(void* user_data) {
  iree_hal_streaming_capture_admission_t* admission =
      (iree_hal_streaming_capture_admission_t*)user_data;
  const uint32_t state =
      iree_atomic_load(&admission->state, iree_memory_order_acquire);
  return (state & iree_hal_streaming_capture_admission_count_mask) == 0;
}

void iree_hal_streaming_capture_admission_initialize(
    iree_hal_streaming_capture_admission_t* out_admission) {
  iree_atomic_store(&out_admission->state, 0, iree_memory_order_relaxed);
  iree_notification_initialize(&out_admission->notification);
  iree_slim_mutex_initialize(&out_admission->transition_mutex);
}

void iree_hal_streaming_capture_admission_deinitialize(
    iree_hal_streaming_capture_admission_t* admission) {
  IREE_ASSERT_EQ(iree_atomic_load(&admission->state, iree_memory_order_relaxed),
                 0u);
  iree_slim_mutex_deinitialize(&admission->transition_mutex);
  iree_notification_deinitialize(&admission->notification);
}

iree_status_t iree_hal_streaming_capture_admission_enter(
    iree_hal_streaming_capture_admission_t* admission) {
  uint32_t state =
      iree_atomic_load(&admission->state, iree_memory_order_acquire);
  while (true) {
    if (IREE_UNLIKELY(state &
                      iree_hal_streaming_capture_admission_transition_bit)) {
      iree_notification_await(&admission->notification,
                              iree_hal_streaming_capture_admission_is_open,
                              admission, iree_infinite_timeout());
      state = iree_atomic_load(&admission->state, iree_memory_order_acquire);
      continue;
    }
    if (IREE_UNLIKELY(state ==
                      iree_hal_streaming_capture_admission_count_mask)) {
      return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                              "capture admission count overflow");
    }
    if (iree_atomic_compare_exchange_weak(&admission->state, &state, state + 1,
                                          iree_memory_order_acq_rel,
                                          iree_memory_order_acquire)) {
      return iree_ok_status();
    }
  }
}

void iree_hal_streaming_capture_admission_leave(
    iree_hal_streaming_capture_admission_t* admission) {
  const uint32_t previous_state =
      iree_atomic_fetch_sub(&admission->state, 1, iree_memory_order_acq_rel);
  IREE_ASSERT_GT(
      previous_state & iree_hal_streaming_capture_admission_count_mask, 0u);
  if (previous_state ==
      (iree_hal_streaming_capture_admission_transition_bit | UINT32_C(1))) {
    iree_notification_post(&admission->notification, IREE_ALL_WAITERS);
  }
}

void iree_hal_streaming_capture_admission_begin_transition(
    iree_hal_streaming_capture_admission_t* admission) {
  iree_slim_mutex_lock(&admission->transition_mutex);
  const uint32_t previous_state = iree_atomic_fetch_or(
      &admission->state, iree_hal_streaming_capture_admission_transition_bit,
      iree_memory_order_acq_rel);
  IREE_ASSERT_FALSE(previous_state &
                    iree_hal_streaming_capture_admission_transition_bit);
  if (previous_state & iree_hal_streaming_capture_admission_count_mask) {
    iree_notification_await(&admission->notification,
                            iree_hal_streaming_capture_admission_is_idle,
                            admission, iree_infinite_timeout());
  }
}

void iree_hal_streaming_capture_admission_end_transition(
    iree_hal_streaming_capture_admission_t* admission) {
  const uint32_t previous_state = iree_atomic_fetch_and(
      &admission->state, iree_hal_streaming_capture_admission_count_mask,
      iree_memory_order_acq_rel);
  IREE_ASSERT_EQ(previous_state,
                 iree_hal_streaming_capture_admission_transition_bit);
  iree_notification_post(&admission->notification, IREE_ALL_WAITERS);
  iree_slim_mutex_unlock(&admission->transition_mutex);
}
