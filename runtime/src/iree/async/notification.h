// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_ASYNC_NOTIFICATION_H_
#define IREE_ASYNC_NOTIFICATION_H_

#include "iree/async/notification_native.h"
#include "iree/base/api.h"
#include "iree/base/internal/atomics.h"
#include "iree/base/threading/futex.h"
#include "iree/base/threading/notification.h"

// Compile-time selection for sync notification_wait() implementation.
// When futex is available, sync waiters use futex_wait() on the epoch atomic —
// no eventfd involvement, no drain race with the poll thread. When futex is
// unavailable (macOS), sync waiters use iree_notification_t (condvar-based),
// keeping the eventfd exclusively for the poll thread.
//
// This flag is potentially transient: after measurement across platforms it
// may either track the main IREE_RUNTIME_USE_FUTEX unconditionally or be
// removed entirely.
//
// Set IREE_ASYNC_POSIX_NOTIFICATION_WANT_FUTEX=0 to force the condvar path
// even on platforms that support futex (for benchmarking/testing).
#ifndef IREE_ASYNC_POSIX_NOTIFICATION_WANT_FUTEX
#define IREE_ASYNC_POSIX_NOTIFICATION_WANT_FUTEX 1
#endif  // IREE_ASYNC_POSIX_NOTIFICATION_WANT_FUTEX
#if defined(IREE_RUNTIME_USE_FUTEX) && IREE_ASYNC_POSIX_NOTIFICATION_WANT_FUTEX
#define IREE_ASYNC_POSIX_NOTIFICATION_USE_FUTEX 1
#endif  // IREE_RUNTIME_USE_FUTEX && IREE_ASYNC_POSIX_NOTIFICATION_WANT_FUTEX

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

typedef struct iree_async_proactor_t iree_async_proactor_t;
typedef struct iree_async_notification_wait_operation_t
    iree_async_notification_wait_operation_t;

//===----------------------------------------------------------------------===//
// Notification
//===----------------------------------------------------------------------===//

// Creation flags for notifications.
enum iree_async_notification_flag_bits_e {
  IREE_ASYNC_NOTIFICATION_FLAG_NONE = 0u,
};
typedef uint32_t iree_async_notification_flags_t;

// Lightweight notification primitive for proactor-integrated thread wakeup.
//
// Provides cross-thread signaling with epoch counting: multiple signals
// coalesce, and waiters observe signals that occurred after their wait was
// submitted. Unlike events (edge-triggered, one signal -> one completion),
// notifications are level-triggered.
//
// Semantics:
//   - signal(): Atomically increments the epoch and wakes waiters.
//     Thread-safe, may be called from any context including callbacks.
//   - wait (async): Completes when the epoch advances past the token captured
//     at submit time. Multiple signals between submit and poll coalesce.
//   - wait (sync): Blocks until the epoch advances or timeout expires.
//     For worker threads outside the proactor.
//
// The epoch counter is the source of truth for signal state. Native events are
// only wakeup mechanisms and never carry one permit per logical observer.
typedef struct iree_async_notification_t {
  // References held by callers and admitted asynchronous consumers.
  iree_atomic_ref_count_t ref_count;

  // The proactor this notification is bound to. Not retained.
  iree_async_proactor_t* proactor;

  // Local epoch and native private address-wait word. Unused when shared.
  iree_atomic_int32_t epoch;

  // Borrowed shared state and wake resources, or NULL for local notifications.
  // The owner outlives all accepted waits, relays, and publication calls.
  iree_async_notification_native_t* shared_native;

  // Number of active observe-check-wait scopes on this handle. This includes
  // synchronous waits, explicit observation scopes, and submitted async waits
  // that carry a caller-provided wait token. Shared notifications do not use
  // this local optimization; their blocking enrollment lives in shared state.
  iree_atomic_int32_t observer_count;

  // Platform-specific resources. Only the creating backend accesses its member.
  union {
    // io_uring backend (Linux).
    // Async consumers share a backend-private monitor of a coalescing eventfd.
    // Synchronous waits use the epoch futex without consuming eventfd
    // readiness.
    struct {
      // Owned for local notifications; borrowed from shared_native otherwise.
      iree_async_event_native_t event;
    } io_uring;

    // POSIX backend (Linux/macOS/BSD).
    // Uses eventfd (Linux) or pipe (macOS/BSD) for asynchronous readiness.
    struct {
      // Owned for local notifications; borrowed from shared_native otherwise.
      iree_async_event_native_t event;
      // Intrusive list of pending async wait operations (poll thread only).
      // Uses iree_async_operation_t::next for linkage.
      iree_async_notification_wait_operation_t* pending_waits;
      // Relays with this notification as their source (poll thread only).
      // Walked on notification fd readiness alongside pending_waits.
      // Uses iree_async_relay_t::platform.posix.notification_relay_next.
      struct iree_async_relay_t* relay_list;
#if !defined(IREE_ASYNC_POSIX_NOTIFICATION_USE_FUTEX)
      // Condvar-based wakeup for sync waiters. Signal posts here alongside
      // the eventfd write; sync waiters await here instead of poll()+read()
      // on the eventfd. This eliminates the drain race where a sync waiter
      // consumes the eventfd signal before the proactor poll loop sees it.
      iree_notification_t sync_notification;
#endif  // !IREE_ASYNC_POSIX_NOTIFICATION_USE_FUTEX
    } posix;

    // IOCP backend (Windows).
    // Local sync waiters use WaitOnAddress on the inline epoch. Shared sync
    // waiters use shared_native independently of async poll progress.
    struct {
      // Intrusive list of pending async wait operations (poll thread only).
      // Uses iree_async_operation_t::next for linkage.
      iree_async_notification_wait_operation_t* pending_waits;
      // Intrusive linkage for proactor's notifications_with_waits list.
      // Only valid while the owner-list flag is set.
      struct iree_async_notification_t* next_with_waits;
      // IOCP owner-list, native association, and withdrawal obligations.
      uint32_t state;
      // Intrusive list of relays with this notification as their source
      // (poll thread only). Walked alongside pending_waits during poll
      // to fire relay sinks when the epoch advances.
      // Uses iree_async_relay_t::platform.iocp.notification_relay_next.
      struct iree_async_relay_t* relay_list;
      // Eager reusable WCP handle, or the legacy threadpool registration.
      // Shared notifications only. WCP associations are consumer-owned;
      // legacy publisher callbacks are joined by destruction.
      uintptr_t wait_registration;
      // Sticky native association failure delivered to accepted consumers.
      iree_status_t failure;
    } iocp;
  } platform;
} iree_async_notification_t;

// Creates a new notification for cross-thread signaling with epoch semantics.
//
// Notifications are lightweight, waitable objects that provide level-triggered
// signaling: multiple signals coalesce, and waiters observe any signal that
// occurs after their wait was submitted. Use
// iree_async_notification_wait_operation_t for async waits, or
// iree_async_notification_wait() for synchronous blocking waits (worker
// threads).
//
// Availability:
//   generic | io_uring | IOCP | kqueue
//   yes     | yes      | yes  | yes
//
// Async waits and relays share native readiness and check the epoch before
// completing. Synchronous waits use platform address waits or blocking wake
// primitives independently of proactor progress.
//
// Returns:
//   IREE_STATUS_OK: Notification created successfully.
//   IREE_STATUS_RESOURCE_EXHAUSTED: System resource limit reached.
IREE_API_EXPORT iree_status_t iree_async_notification_create(
    iree_async_proactor_t* proactor, iree_async_notification_flags_t flags,
    iree_async_notification_t** out_notification);

// Creates the managed receiver for an initialized native shared notification.
//
// Borrows |native| and its resources without taking ownership. The caller keeps
// them alive through notification destruction and native observer retirement.
// Each native bundle has one managed receiving notification. Its local waits
// and relays share readiness; remote publishers use native_signal without
// creating a managed receiver or owning a proactor. Independent receiving
// domains require independent native bundles.
//
// Returns:
//   IREE_STATUS_OK: Notification created successfully.
//   IREE_STATUS_RESOURCE_EXHAUSTED: System resource limit reached.
IREE_API_EXPORT iree_status_t iree_async_notification_create_shared(
    iree_async_proactor_t* proactor, iree_async_notification_native_t* native,
    iree_async_notification_t** out_notification);

// Retains a reference to the notification.
IREE_API_EXPORT void iree_async_notification_retain(
    iree_async_notification_t* notification);

// Releases a reference to the notification.
// Destroys when the reference count reaches zero.
IREE_API_EXPORT void iree_async_notification_release(
    iree_async_notification_t* notification);

// Advances the epoch and wakes observers of the notification.
//
// Thread-safe, async-signal-safe. May be called from any context including
// completion callbacks, signal handlers, or other threads.
//
// |wake_count| is a native wake hint, not a limit on observers seeing the new
// epoch. Use 1 for a single native sleeper or INT32_MAX for broadcast. Async
// waits and relays sharing one native monitor observe the publication together.
//
// Implementation:
//   Atomically increments the notification's epoch, then wakes waiters via
//   the platform primitive (futex_wake, eventfd write, etc.).
//
// Each signal advances the epoch. Waiters observe whether the epoch changed,
// so multiple signals can coalesce into one wait completion.
IREE_API_EXPORT void iree_async_notification_signal(
    iree_async_notification_t* notification, int32_t wake_count);

// Signals an advisory notification while avoiding wake work when no observer is
// known to exist.
//
// The epoch is always advanced so a waiter that has already observed the token
// cannot miss a concurrent signal between its protected-condition re-check and
// its platform wait. Returns true when a platform wake was performed; returns
// false when there were no active observe-check-wait scopes and wake work was
// skipped.
//
// Shared notifications always perform the platform wake: observers on other
// handles or in other processes are not represented by the local count.
//
// This must only be used for protocols where notification waits are advisory
// wakeups over a separately checked condition. It is not a replacement for
// iree_async_notification_signal() when the signal itself is user-visible.
IREE_API_EXPORT bool iree_async_notification_signal_if_observed(
    iree_async_notification_t* notification, int32_t wake_count);

// Returns the current epoch of the notification.
//
// The epoch is incremented each time the notification is signaled. This can be
// used for polling-style observation of notification state without blocking.
// Typical usage:
//   1. Read epoch before an operation: observed = query_epoch(notification)
//   2. Perform operation that should trigger a signal
//   3. Poll until epoch advances: while (query_epoch(notification) == observed)
//
// Thread safety:
//   May be called from any thread concurrently with signal/wait operations.
//   The returned value represents a point-in-time snapshot.
static inline uint32_t iree_async_notification_query_epoch(
    const iree_async_notification_t* notification) {
  return notification->shared_native
             ? iree_notification_state_query_epoch(
                   notification->shared_native->state)
             : (uint32_t)iree_atomic_load(&notification->epoch,
                                          iree_memory_order_acquire);
}

// Begins an observe-check-wait protocol and returns the current epoch token.
//
// The returned token can be used with
// iree_async_notification_wait_for_token(). Callers must pair every successful
// begin with iree_async_notification_end_observe(), including when the
// protected condition is satisfied and no wait is armed.
//
// This explicit observation scope is what makes
// iree_async_notification_signal_if_observed() race-free: a producer that
// signals after the observer begins will either advance the epoch or the
// observer's protected-condition re-check will see the producer's state change.
IREE_API_EXPORT uint32_t
iree_async_notification_begin_observe(iree_async_notification_t* notification);

// Ends an observe-check-wait protocol begun by
// iree_async_notification_begin_observe().
IREE_API_EXPORT void iree_async_notification_end_observe(
    iree_async_notification_t* notification);

// Blocks the calling thread until the notification is signaled or timeout
// expires.
//
// This is the synchronous blocking API for worker threads that need to wait
// on a notification outside the proactor's poll loop. The proactor poll thread
// should use NOTIFICATION_WAIT operations instead.
//
// Returns:
//   true: Notification was signaled.
//   false: Timeout expired without signal.
//
// Thread safety:
//   May be called from any thread. Multiple threads may wait concurrently.
//   However, this function blocks the calling thread and should not be called
//   from the proactor's poll thread (it would deadlock).
IREE_API_EXPORT bool iree_async_notification_wait(
    iree_async_notification_t* notification, iree_timeout_t timeout);

// Blocks until |notification|'s epoch differs from |wait_token| or |timeout|
// expires.
//
// The caller must hold an active observation scope from
// iree_async_notification_begin_observe() while calling this. Most callers
// should use iree_async_notification_wait(); this lower-level form exists for
// observe-check-wait protocols that need to capture the epoch before checking a
// protected condition.
IREE_API_EXPORT bool iree_async_notification_wait_for_token(
    iree_async_notification_t* notification, uint32_t wait_token,
    iree_timeout_t timeout);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_ASYNC_NOTIFICATION_H_
