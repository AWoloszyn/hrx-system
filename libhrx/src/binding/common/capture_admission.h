// Copyright 2026 The HRX Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LIBHRX_SRC_BINDING_COMMON_CAPTURE_ADMISSION_H_
#define LIBHRX_SRC_BINDING_COMMON_CAPTURE_ADMISSION_H_

#include "iree/base/api.h"
#include "iree/base/internal/atomics.h"
#include "iree/base/threading/mutex.h"
#include "iree/base/threading/notification.h"

#ifdef __cplusplus
extern "C" {
#endif

// Coordinates ordinary stream operation admission with capture transitions.
//
// Operations admitted together may order and submit concurrently. A capture
// transition closes admission, waits for every admitted operation to leave,
// and then has exclusive access until it reopens admission. This makes an
// operation's capture disposition and queue acceptance one transaction without
// serializing independent streams for the duration of submission.
typedef struct iree_hal_streaming_capture_admission_t {
  // Transition flag and number of admitted operations packed into one atomic
  // value so closing admission cannot race a new operation entering it.
  iree_atomic_uint32_t state;

  // Wakes transitions after the last admitted operation leaves and wakes
  // operations after a transition reopens admission.
  iree_notification_t notification;

  // Serializes the capture transitions that hold exclusive admission.
  iree_slim_mutex_t transition_mutex;
} iree_hal_streaming_capture_admission_t;

// Initializes |out_admission| with ordinary operation admission open.
void iree_hal_streaming_capture_admission_initialize(
    iree_hal_streaming_capture_admission_t* out_admission);

// Deinitializes an idle admission object. No transition may be active and no
// thread may be waiting on the object.
void iree_hal_streaming_capture_admission_deinitialize(
    iree_hal_streaming_capture_admission_t* admission);

// Admits one ordinary operation, waiting while a capture transition is active.
// The operation must leave admission after its capture disposition and queue
// acceptance have been decided.
iree_status_t iree_hal_streaming_capture_admission_enter(
    iree_hal_streaming_capture_admission_t* admission);

// Releases one previously admitted ordinary operation.
void iree_hal_streaming_capture_admission_leave(
    iree_hal_streaming_capture_admission_t* admission);

// Closes ordinary admission and waits for admitted operations to leave. The
// caller has exclusive transition ownership until end_transition is called.
void iree_hal_streaming_capture_admission_begin_transition(
    iree_hal_streaming_capture_admission_t* admission);

// Ends exclusive transition ownership and reopens ordinary admission.
void iree_hal_streaming_capture_admission_end_transition(
    iree_hal_streaming_capture_admission_t* admission);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LIBHRX_SRC_BINDING_COMMON_CAPTURE_ADMISSION_H_
