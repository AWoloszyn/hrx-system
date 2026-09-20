// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_ASYNC_NOTIFICATION_NATIVE_H_
#define IREE_ASYNC_NOTIFICATION_NATIVE_H_

#include "iree/async/event.h"
#include "iree/base/threading/notification.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Native resources transferred alongside a separately shared state word.
// Linux transfers one eventfd. Pipe-backed platforms transfer both pipe ends.
// Windows transfers separate auto-reset Events for async and sync observers.
#if defined(IREE_ASYNC_HAVE_EVENTFD)
#define IREE_ASYNC_NOTIFICATION_NATIVE_HANDLE_COUNT 1u
#else
#define IREE_ASYNC_NOTIFICATION_NATIVE_HANDLE_COUNT 2u
#endif

// Shared notification resources independent of a proactor or managed object.
// The container requires no allocation or reference count; its owner retains
// both the mapped state and native resources until all users have retired.
// Copying the container does not duplicate resource ownership.
//
// Each resource bundle has one receiving domain: one async polling owner and
// any synchronous callers sharing that domain's native container. Other
// imported containers can publish, but independent receiving domains require
// independent bundles. Native readiness coalesces and is not a permit count.
typedef struct iree_async_notification_native_t {
  // Borrowed, eight-byte-aligned state in a shared mapping. Never reset here.
  iree_notification_state_t* state;
  // Owned async doorbell, drained only by the receiving proactor.
  iree_async_event_native_t async_event;
#if defined(IREE_PLATFORM_WINDOWS)
  // Blocking callers never consume the async doorbell's readiness.
  struct {
    // Owned auto-reset Event, shared with remote publishers.
    iree_async_event_native_t event;
    // Local handoff generation; the low bit marks the one native waiter.
    // Followers wait on this word, which changes whenever that waiter exits.
    iree_atomic_uint64_t handoff;
  } synchronous;
#endif  // IREE_PLATFORM_WINDOWS
} iree_async_notification_native_t;

// Whether this build and host support native shared notifications. Availability
// requires lock-free 64-bit atomics and independent sync/async wake mechanisms.
// Apple builds additionally require a supporting SDK and macOS 14.4 or newer.
IREE_API_EXPORT bool iree_async_notification_native_is_supported(void);

// Creates native resources attached to already initialized |state|. New state
// is initialized separately with iree_notification_state_initialize before any
// participant can access it; attaching never resets shared state. Returns
// UNAVAILABLE before creating resources when native support is absent. Leaves
// |out_notification| empty on failure.
IREE_API_EXPORT iree_status_t iree_async_notification_native_initialize(
    iree_notification_state_t* state,
    iree_async_notification_native_t* out_notification);

// Attaches to existing |state| and consumes a complete owned resource bundle
// produced by export and native resource transfer. Clears and consumes every
// handle on success or failure. The mapping remains caller-owned.
IREE_API_EXPORT iree_status_t iree_async_notification_native_import(
    iree_notification_state_t* state,
    iree_async_primitive_t handles[IREE_ASYNC_NOTIFICATION_NATIVE_HANDLE_COUNT],
    iree_async_notification_native_t* out_notification);

// Exports borrowed handles for native resource transfer, not raw integer IPC.
// The owner retains them until the receiver has acknowledged ownership.
IREE_API_EXPORT void iree_async_notification_native_export(
    const iree_async_notification_native_t* notification,
    iree_async_primitive_t
        out_handles[IREE_ASYNC_NOTIFICATION_NATIVE_HANDLE_COUNT]);

// Closes native resources and clears the container without unmapping state.
// All publishers, blocking callers, and async native observers must have
// retired. An empty or zero-initialized container may be deinitialized.
IREE_API_EXPORT void iree_async_notification_native_deinitialize(
    iree_async_notification_native_t* notification);

// Advances the shared epoch and wakes observers without accessing a proactor.
// Publication is infallible and does not acknowledge peer progress. Native
// synchronous wake work is skipped when no blocking callers are enrolled.
// |wake_count| is a nonnegative hint; platforms may wake additional observers.
IREE_API_EXPORT void iree_async_notification_native_signal(
    iree_async_notification_native_t* notification, int32_t wake_count);

// Blocks until the shared epoch differs from |wait_token| or |timeout| expires.
// The caller captures its token before checking the condition it protects.
// Enrollment is internal and balanced on every return. Synchronous callers
// share the receiving domain's one native container; imported signal-only
// containers must not establish independent receiving domains on its resources.
IREE_API_EXPORT bool iree_async_notification_native_wait_for_token(
    iree_async_notification_native_t* notification, uint32_t wait_token,
    iree_timeout_t timeout);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_ASYNC_NOTIFICATION_NATIVE_H_
