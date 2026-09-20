// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Notification-local subscriber and native monitor ownership for io_uring.

#ifndef IREE_ASYNC_PLATFORM_IO_URING_NOTIFICATION_H_
#define IREE_ASYNC_PLATFORM_IO_URING_NOTIFICATION_H_

#include "iree/async/notification.h"
#include "iree/async/operations/scheduling.h"
#include "iree/async/relay.h"
#include "iree/base/api.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

typedef struct iree_async_proactor_io_uring_t iree_async_proactor_io_uring_t;

// Distinguishes the private HANDLE_POLL from user-visible completions.
enum iree_async_io_uring_notification_operation_flag_bits_e {
  IREE_ASYNC_IO_URING_NOTIFICATION_OPERATION_MONITOR = 1u << 0,
};

// Creates an eventfd-backed io_uring notification. Synchronous waits use the
// epoch's futex separately from asynchronous native readiness.
iree_status_t iree_async_io_uring_notification_create(
    iree_async_proactor_io_uring_t* proactor,
    iree_async_notification_flags_t flags,
    iree_async_notification_t** out_notification);

// Creates a shared io_uring notification backed by cross-process state.
// Borrows the caller's native state and wake resources.
iree_status_t iree_async_io_uring_notification_create_shared(
    iree_async_proactor_io_uring_t* proactor,
    iree_async_notification_native_t* native,
    iree_async_notification_t** out_notification);

// Destroys an io_uring notification, closing privately owned native resources.
void iree_async_io_uring_notification_destroy(
    iree_async_proactor_io_uring_t* proactor,
    iree_async_notification_t* notification);

// Wakes asynchronous eventfd consumers and synchronous futex waiters.
// Called from the shared notification_signal() after epoch increment.
void iree_async_io_uring_notification_signal(
    iree_async_proactor_t* base_proactor,
    iree_async_notification_t* notification, int32_t wake_count);

// Synchronous epoch-futex wait, independent of proactor progress.
// Called from the shared notification_wait().
bool iree_async_io_uring_notification_wait(
    iree_async_proactor_t* base_proactor,
    iree_async_notification_t* notification, uint32_t wait_token,
    iree_timeout_t timeout);

// Publishes an already accepted wait. Captures its original epoch when needed;
// the caller owns the normal submit wake/dispatch handshake. Thread-safe.
void iree_async_io_uring_notification_submit_wait(
    iree_async_notification_wait_operation_t* wait);

// Requests cancellation without consuming native SQ capacity. Thread-safe.
void iree_async_io_uring_notification_cancel_wait(
    iree_async_notification_wait_operation_t* wait);

// Attaches/removes notification-source relays on the poll owner. Registration
// captures its epoch before returning. Unregistration completes only after
// the source no longer has native or software ownership of the relay.
void iree_async_io_uring_notification_register_relay(iree_async_relay_t* relay);
void iree_async_io_uring_notification_unregister_relay(
    iree_async_relay_t* relay);

// Services a bounded snapshot of notification intents on the poll owner.
// Returns true when native SQEs were staged and need submission.
bool iree_async_io_uring_notification_drain_pending(
    iree_async_proactor_io_uring_t* proactor);

// Retires queued intents before relay cleanup after ring close.
void iree_async_io_uring_notification_discard_pending(
    iree_async_proactor_io_uring_t* proactor);

// Removes a relay and retires its source monitor after synchronous ring close.
void iree_async_io_uring_notification_detach_relay_after_ring_close(
    iree_async_relay_t* relay);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_ASYNC_PLATFORM_IO_URING_NOTIFICATION_H_
