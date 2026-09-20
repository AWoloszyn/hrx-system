// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/async/platform/io_uring/notification.h"

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

#include "iree/async/platform/io_uring/proactor.h"
#include "iree/async/platform/io_uring/relay.h"
#include "iree/async/proactor.h"
#include "iree/async/util/continuation.h"
#include "iree/base/threading/futex.h"
#include "iree/base/threading/mutex.h"

enum iree_async_io_uring_notification_state_flag_bits_e {
  IREE_ASYNC_IO_URING_NOTIFICATION_PENDING = 1u << 0,
  IREE_ASYNC_IO_URING_NOTIFICATION_POLL_IN_FLIGHT = 1u << 1,
  IREE_ASYNC_IO_URING_NOTIFICATION_RETIRING = 1u << 2,
};
typedef uint32_t iree_async_io_uring_notification_state_flags_t;

enum iree_async_io_uring_notification_wait_state_bits_e {
  IREE_ASYNC_IO_URING_NOTIFICATION_WAIT_CANCELLED = 1u << 0,
  IREE_ASYNC_IO_URING_NOTIFICATION_WAIT_DETACHED = 1u << 1,
  IREE_ASYNC_IO_URING_NOTIFICATION_WAIT_ATTACHED = 1u << 2,
};

typedef struct iree_async_io_uring_notification_t {
  // Shared notification identity and epoch publication.
  iree_async_notification_t base;
  // Serializes admission and cancellation with terminal list detachment.
  iree_slim_mutex_t mutex;
  // Coalesced owner-work entry, held until dispatch finishes classification.
  iree_atomic_slist_entry_t pending_entry;
  // Pending, native poll, and native cancellation ownership under |mutex|.
  iree_async_io_uring_notification_state_flags_t state;
  // Accepted waits, including last consumers held for native retirement.
  iree_async_operation_t* waits;
  // Source-local relay subscriptions, owned by the poll thread.
  iree_async_relay_t* relays;
  // Sole native consumer of the coalescing eventfd.
  iree_async_handle_poll_operation_t poll;
  // Joins cancellation-key retirement before poll identity reuse.
  iree_async_cancel_request_t cancel;
  // Sticky native monitor failure, propagated to present and future consumers.
  iree_status_t failure;
} iree_async_io_uring_notification_t;

static iree_async_io_uring_notification_t*
iree_async_io_uring_notification_cast(iree_async_notification_t* notification) {
  return (iree_async_io_uring_notification_t*)notification;
}

// The accepting consumer or an in-flight monitor keeps the entry alive. While
// the owner dispatches it, PENDING stays set even across unlocked callbacks.
static void iree_async_io_uring_notification_enqueue_locked(
    iree_async_io_uring_notification_t* notification) {
  if (!iree_any_bit_set(notification->state,
                        IREE_ASYNC_IO_URING_NOTIFICATION_PENDING)) {
    notification->state |= IREE_ASYNC_IO_URING_NOTIFICATION_PENDING;
    iree_async_proactor_io_uring_t* proactor =
        iree_async_proactor_io_uring_cast(notification->base.proactor);
    iree_atomic_slist_push(&proactor->pending_notifications,
                           &notification->pending_entry);
  }
}

static void iree_async_io_uring_notification_cancel_complete(void* user_data) {
  iree_async_io_uring_notification_t* notification = user_data;
  iree_slim_mutex_lock(&notification->mutex);
  // The key receipt may precede the target CQE. Keep the retirement cycle
  // active until both arrive, suppressing duplicate requests and key reuse.
  if (!iree_any_bit_set(notification->state,
                        IREE_ASYNC_IO_URING_NOTIFICATION_POLL_IN_FLIGHT)) {
    notification->state &= ~IREE_ASYNC_IO_URING_NOTIFICATION_RETIRING;
  }
  iree_async_io_uring_notification_enqueue_locked(notification);
  iree_slim_mutex_unlock(&notification->mutex);
}

static void iree_async_io_uring_notification_poll_complete(
    void* user_data, iree_async_operation_t* operation, iree_status_t status,
    iree_async_completion_flags_t flags) {
  iree_async_io_uring_notification_t* notification = user_data;
  iree_slim_mutex_lock(&notification->mutex);
  notification->state &= ~IREE_ASYNC_IO_URING_NOTIFICATION_POLL_IN_FLIGHT;
  if (iree_status_is_cancelled(status) &&
      iree_any_bit_set(notification->state,
                       IREE_ASYNC_IO_URING_NOTIFICATION_RETIRING)) {
    // This is the requested retirement, not a source failure.
    iree_status_free(status);
  } else {
    if (iree_status_is_ok(status) &&
        !iree_any_bit_set(notification->poll.result_events,
                          IREE_ASYNC_POLL_EVENT_IN)) {
      status = iree_make_status(IREE_STATUS_UNAVAILABLE,
                                "notification wake descriptor closed");
    }
    notification->failure = iree_status_join(notification->failure, status);
  }
  if (notification->cancel.phase == IREE_ASYNC_CANCEL_REQUEST_PHASE_IDLE) {
    notification->state &= ~IREE_ASYNC_IO_URING_NOTIFICATION_RETIRING;
  }
  iree_async_io_uring_notification_enqueue_locked(notification);
  iree_slim_mutex_unlock(&notification->mutex);
  // Withdrawing an unissued cancellation invokes its receipt inline.
  iree_async_proactor_cancel_request_target_retired(notification->base.proactor,
                                                    &notification->cancel);
}

static void iree_async_io_uring_notification_initialize(
    iree_async_proactor_io_uring_t* proactor,
    iree_async_io_uring_notification_t* notification) {
  iree_atomic_ref_count_init(&notification->base.ref_count);
  notification->base.proactor = &proactor->base;
  iree_slim_mutex_initialize(&notification->mutex);
  iree_async_cancel_callback_t callback = {
      .fn = iree_async_io_uring_notification_cancel_complete,
      .user_data = notification,
  };
  iree_async_cancel_request_initialize(callback, &notification->cancel);
}

//===----------------------------------------------------------------------===//
// Creation and destruction
//===----------------------------------------------------------------------===//

iree_status_t iree_async_io_uring_notification_create(
    iree_async_proactor_io_uring_t* proactor,
    iree_async_notification_flags_t flags,
    iree_async_notification_t** out_notification) {
  IREE_TRACE_ZONE_BEGIN(z0);
  IREE_ASSERT_ARGUMENT(out_notification);
  *out_notification = NULL;

  iree_allocator_t allocator = proactor->base.allocator;

  iree_async_io_uring_notification_t* storage = NULL;
  IREE_RETURN_AND_END_ZONE_IF_ERROR(
      z0, iree_allocator_malloc(allocator, sizeof(*storage), (void**)&storage));
  iree_async_io_uring_notification_initialize(proactor, storage);
  iree_async_notification_t* notification = &storage->base;
  iree_atomic_store(&notification->epoch, 0, iree_memory_order_release);

  // A single native consumer drains coalesced wakeups; the epoch, not the
  // counter value, determines which local subscribers have observed a signal.
  iree_status_t status = iree_async_event_native_initialize(
      &notification->platform.io_uring.event);

  if (iree_status_is_ok(status)) {
    *out_notification = notification;
  } else {
    iree_slim_mutex_deinitialize(&storage->mutex);
    iree_allocator_free(allocator, notification);
  }
  IREE_TRACE_ZONE_END(z0);
  return status;
}

iree_status_t iree_async_io_uring_notification_create_shared(
    iree_async_proactor_io_uring_t* proactor,
    iree_async_notification_native_t* native,
    iree_async_notification_t** out_notification) {
  IREE_TRACE_ZONE_BEGIN(z0);
  IREE_ASSERT_ARGUMENT(out_notification);
  *out_notification = NULL;

  iree_allocator_t allocator = proactor->base.allocator;

  iree_async_io_uring_notification_t* storage = NULL;
  IREE_RETURN_AND_END_ZONE_IF_ERROR(
      z0, iree_allocator_malloc(allocator, sizeof(*storage), (void**)&storage));
  iree_async_io_uring_notification_initialize(proactor, storage);
  iree_async_notification_t* notification = &storage->base;
  notification->shared_native = native;

  notification->platform.io_uring.event = native->async_event;

  *out_notification = notification;
  IREE_TRACE_ZONE_END(z0);
  return iree_ok_status();
}

void iree_async_io_uring_notification_destroy(
    iree_async_proactor_io_uring_t* proactor,
    iree_async_notification_t* notification) {
  IREE_TRACE_ZONE_BEGIN(z0);
  if (!notification) {
    IREE_TRACE_ZONE_END(z0);
    return;
  }

  iree_allocator_t allocator = proactor->base.allocator;
  iree_async_io_uring_notification_t* storage =
      iree_async_io_uring_notification_cast(notification);
  IREE_ASSERT(storage->state == 0 && !storage->waits && !storage->relays,
              "notification destroyed before consumer retirement");

  if (!notification->shared_native) {
    iree_async_event_native_deinitialize(
        &notification->platform.io_uring.event);
  }

  iree_status_free(storage->failure);
  iree_slim_mutex_deinitialize(&storage->mutex);
  iree_allocator_free(allocator, storage);
  IREE_TRACE_ZONE_END(z0);
}

//===----------------------------------------------------------------------===//
// Vtable implementations for signal and wait
//===----------------------------------------------------------------------===//

void iree_async_io_uring_notification_signal(
    iree_async_proactor_t* base_proactor,
    iree_async_notification_t* notification, int32_t wake_count) {
  iree_async_event_native_set(&notification->platform.io_uring.event);

#if defined(IREE_PLATFORM_HAS_FUTEX)
  // Synchronous waiters have their own native wake channel. They never consume
  // eventfd readiness belonging to asynchronous waits and relays.
  iree_futex_wake(&notification->epoch, wake_count);
#endif  // IREE_PLATFORM_HAS_FUTEX
}

bool iree_async_io_uring_notification_wait(
    iree_async_proactor_t* base_proactor,
    iree_async_notification_t* notification, uint32_t wait_token,
    iree_timeout_t timeout) {
  iree_time_t deadline_ns = iree_timeout_as_deadline_ns(timeout);
  while (iree_time_now() < deadline_ns) {
    uint32_t current_epoch = iree_async_notification_query_epoch(notification);
    if (current_epoch != wait_token) {
      return true;
    }

#if defined(IREE_PLATFORM_HAS_FUTEX)
    iree_status_code_t status_code =
        iree_futex_wait(&notification->epoch, wait_token, deadline_ns);
    if (status_code == IREE_STATUS_DEADLINE_EXCEEDED) {
      break;
    }
    IREE_ASSERT(status_code == IREE_STATUS_OK,
                "local notification wait contract violated");
#endif  // IREE_PLATFORM_HAS_FUTEX
  }

  uint32_t final_epoch = iree_async_notification_query_epoch(notification);
  return final_epoch != wait_token;
}

//===----------------------------------------------------------------------===//
// Local subscriber admission
//===----------------------------------------------------------------------===//

void iree_async_io_uring_notification_submit_wait(
    iree_async_notification_wait_operation_t* wait) {
  iree_async_io_uring_notification_t* notification =
      iree_async_io_uring_notification_cast(wait->notification);
  if (!iree_any_bit_set(wait->wait_flags,
                        IREE_ASYNC_NOTIFICATION_WAIT_FLAG_USE_WAIT_TOKEN)) {
    wait->wait_token = iree_async_notification_query_epoch(&notification->base);
  }
  iree_slim_mutex_lock(&notification->mutex);
  wait->base.next = notification->waits;
  notification->waits = &wait->base;
  iree_async_operation_set_internal_flags(
      &wait->base, IREE_ASYNC_IO_URING_NOTIFICATION_WAIT_ATTACHED);
  iree_async_io_uring_notification_enqueue_locked(notification);
  iree_slim_mutex_unlock(&notification->mutex);
}

void iree_async_io_uring_notification_cancel_wait(
    iree_async_notification_wait_operation_t* wait) {
  iree_async_io_uring_notification_t* notification =
      iree_async_io_uring_notification_cast(wait->notification);
  iree_async_proactor_t* proactor = notification->base.proactor;
  iree_slim_mutex_lock(&notification->mutex);
  uint32_t state = iree_async_operation_load_internal_flags(&wait->base);
  if (!iree_any_bit_set(state,
                        IREE_ASYNC_IO_URING_NOTIFICATION_WAIT_DETACHED)) {
    iree_async_operation_set_internal_flags(
        &wait->base, IREE_ASYNC_IO_URING_NOTIFICATION_WAIT_CANCELLED);
    // A deferred linked wait is owned by its predecessor until activation.
    // Record its cancellation without publishing an unowned monitor intent.
    if (iree_any_bit_set(state,
                         IREE_ASYNC_IO_URING_NOTIFICATION_WAIT_ATTACHED)) {
      iree_async_io_uring_notification_enqueue_locked(notification);
    }
  }
  iree_slim_mutex_unlock(&notification->mutex);
  iree_async_proactor_wake(proactor);
}

void iree_async_io_uring_notification_register_relay(
    iree_async_relay_t* relay) {
  iree_async_io_uring_notification_t* notification =
      iree_async_io_uring_notification_cast(relay->source.notification);
  relay->wait_epoch = iree_async_notification_query_epoch(&notification->base);
  relay->platform.io_uring.state = IREE_ASYNC_IO_URING_RELAY_STATE_ACTIVE;
  iree_slim_mutex_lock(&notification->mutex);
  relay->platform.io_uring.notification_relay_next = notification->relays;
  notification->relays = relay;
  iree_async_io_uring_notification_enqueue_locked(notification);
  iree_slim_mutex_unlock(&notification->mutex);
}

void iree_async_io_uring_notification_unregister_relay(
    iree_async_relay_t* relay) {
  iree_async_io_uring_notification_t* notification =
      iree_async_io_uring_notification_cast(relay->source.notification);
  iree_slim_mutex_lock(&notification->mutex);
  relay->platform.io_uring.state =
      IREE_ASYNC_IO_URING_RELAY_STATE_UNREGISTRATION_PENDING;
  iree_async_io_uring_notification_enqueue_locked(notification);
  iree_slim_mutex_unlock(&notification->mutex);
  iree_async_proactor_wake(relay->proactor);
}

//===----------------------------------------------------------------------===//
// Native monitor ownership
//===----------------------------------------------------------------------===//

// Only the poll owner stages the monitor. A full SQ is submitted while holding
// its lock, making capacity available without waiting for any completion.
static iree_status_t iree_async_io_uring_notification_arm_locked(
    iree_async_proactor_io_uring_t* proactor,
    iree_async_io_uring_notification_t* notification) {
  iree_io_uring_ring_sq_lock(&proactor->ring);
  iree_io_uring_sqe_t* sqe = iree_io_uring_ring_get_sqe(&proactor->ring);
  iree_status_t status = iree_ok_status();
  if (!sqe) {
    status = iree_io_uring_ring_submit_pending_locked(&proactor->ring);
    if (iree_status_is_ok(status)) {
      sqe = iree_io_uring_ring_get_sqe(&proactor->ring);
      if (!sqe) {
        status = iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                                  "SQ remained full after submission");
      }
    }
  }
  if (iree_status_is_ok(status)) {
    iree_async_handle_poll_operation_t* poll = &notification->poll;
    iree_async_operation_initialize(
        &poll->base, IREE_ASYNC_OPERATION_TYPE_HANDLE_POLL,
        IREE_ASYNC_OPERATION_FLAG_NONE,
        iree_async_io_uring_notification_poll_complete, notification);
    iree_async_operation_set_internal_flags(
        &poll->base, IREE_ASYNC_IO_URING_NOTIFICATION_OPERATION_MONITOR);
    poll->primitive = notification->base.platform.io_uring.event.wait_primitive;
    poll->events = IREE_ASYNC_POLL_EVENT_IN;
    poll->result_events = IREE_ASYNC_POLL_EVENT_NONE;
    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = IREE_IORING_OP_POLL_ADD;
    sqe->fd = poll->primitive.value.fd;
    sqe->poll32_events = POLLIN;
    sqe->user_data = (uint64_t)(uintptr_t)&poll->base;
    notification->state |= IREE_ASYNC_IO_URING_NOTIFICATION_POLL_IN_FLIGHT;
#if defined(IREE_SANITIZER_THREAD)
    iree_atomic_store(&poll->base.tsan_bridge, 1, iree_memory_order_release);
#endif  // IREE_SANITIZER_THREAD
  }
  iree_io_uring_ring_sq_unlock(&proactor->ring);
  return status;
}

// The single consumer resets an ordinary eventfd in one read. A signal racing
// after this read either advances the sampled epoch or leaves readiness for
// the next poll. No kernel READ or other local consumer competes with it.
static iree_status_t iree_async_io_uring_notification_drain_wake(
    iree_async_io_uring_notification_t* notification) {
  uint64_t value;
  ssize_t result;
  do {
    result =
        read(notification->base.platform.io_uring.event.wait_primitive.value.fd,
             &value, sizeof(value));
  } while (result < 0 && errno == EINTR);
  if (result == sizeof(value) || (result < 0 && errno == EAGAIN)) {
    return iree_ok_status();
  }
  return iree_make_status(
      result < 0 ? iree_status_code_from_errno(errno) : IREE_STATUS_DATA_LOSS,
      "notification wake descriptor read failed");
}

static bool iree_async_io_uring_notification_wait_is_ready(
    iree_async_notification_wait_operation_t* wait, uint32_t epoch,
    iree_status_code_t failure_code) {
  return failure_code != IREE_STATUS_OK || wait->wait_token != epoch ||
         iree_any_bit_set(iree_async_operation_load_internal_flags(&wait->base),
                          IREE_ASYNC_IO_URING_NOTIFICATION_WAIT_CANCELLED);
}

static bool iree_async_io_uring_notification_has_pending_consumers(
    iree_async_io_uring_notification_t* notification, uint32_t epoch) {
  if (!iree_status_is_ok(notification->failure)) {
    return false;
  }
  for (iree_async_operation_t* operation = notification->waits; operation;
       operation = operation->next) {
    if (!iree_async_io_uring_notification_wait_is_ready(
            (iree_async_notification_wait_operation_t*)operation, epoch,
            iree_status_code(notification->failure))) {
      return true;
    }
  }
  for (iree_async_relay_t* relay = notification->relays; relay;
       relay = relay->platform.io_uring.notification_relay_next) {
    if (relay->platform.io_uring.state ==
        IREE_ASYNC_IO_URING_RELAY_STATE_ACTIVE) {
      return true;
    }
  }
  return false;
}

// The relay error callback may submit work, so dispatch it outside the mutex.
// The API prohibits unregistering relays from that callback. PENDING stays
// claimed so concurrent wait admission is consumed by this dispatch pass.
static void iree_async_io_uring_notification_dispatch_relays_locked(
    iree_async_io_uring_notification_t* notification, uint32_t epoch) {
  for (iree_async_relay_t* relay = notification->relays; relay;
       relay = relay->platform.io_uring.notification_relay_next) {
    if (relay->platform.io_uring.state !=
        IREE_ASYNC_IO_URING_RELAY_STATE_ACTIVE) {
      continue;
    }
    if (iree_status_is_ok(notification->failure) &&
        relay->wait_epoch == epoch) {
      continue;
    }
    // Publish the observation before executing a sink that may signal again.
    relay->wait_epoch = epoch;
    iree_status_t status = iree_status_clone(notification->failure);
    iree_slim_mutex_unlock(&notification->mutex);
    iree_async_io_uring_relay_dispatch_notification(relay, status);
    iree_slim_mutex_lock(&notification->mutex);
  }
}

static void iree_async_io_uring_notification_complete_waits(
    iree_async_proactor_io_uring_t* proactor, iree_async_operation_t* ready,
    iree_status_t failure) {
  while (ready) {
    iree_async_operation_t* operation = ready;
    ready = operation->next;
    operation->next = NULL;
    iree_status_t status =
        iree_any_bit_set(iree_async_operation_load_internal_flags(operation),
                         IREE_ASYNC_IO_URING_NOTIFICATION_WAIT_CANCELLED)
            ? iree_status_from_code(IREE_STATUS_CANCELLED)
            : iree_status_clone(failure);
    iree_async_operation_t* continuation =
        iree_async_continuation_take(operation);
    iree_status_code_t code = iree_status_code(status);
    iree_async_proactor_io_uring_push_software_operation(proactor, operation,
                                                         status);
    if (continuation) {
      if (code == IREE_STATUS_OK) {
        iree_async_proactor_io_uring_dispatch_continuation_chain(proactor,
                                                                 continuation);
      } else {
        iree_async_proactor_io_uring_cancel_continuation_chain_to_mpsc(
            proactor, continuation);
      }
    }
  }
  iree_status_free(failure);
}

static bool iree_async_io_uring_notification_dispatch(
    iree_async_proactor_io_uring_t* proactor,
    iree_async_io_uring_notification_t* notification) {
  iree_async_operation_t* ready = NULL;
  iree_async_relay_t* retired_relays = NULL;
  bool armed = false;
  iree_slim_mutex_lock(&notification->mutex);
  uint32_t epoch = 0;
  bool has_pending_consumers = false;
  for (;;) {
    bool native_owned = iree_any_bit_set(
        notification->state, IREE_ASYNC_IO_URING_NOTIFICATION_POLL_IN_FLIGHT |
                                 IREE_ASYNC_IO_URING_NOTIFICATION_RETIRING);
    if (!native_owned && (notification->waits || notification->relays) &&
        iree_status_is_ok(notification->failure)) {
      notification->failure =
          iree_async_io_uring_notification_drain_wake(notification);
    }
    if (notification->waits || notification->relays) {
      epoch = iree_async_notification_query_epoch(&notification->base);
    }
    if (!native_owned) {
      iree_async_io_uring_notification_dispatch_relays_locked(notification,
                                                              epoch);
      // An unlocked relay callback may admit a wait with a newer token.
      // Compare it with a fresh epoch, never the earlier relay snapshot.
      if (notification->waits || notification->relays) {
        epoch = iree_async_notification_query_epoch(&notification->base);
      }
    }
    has_pending_consumers =
        iree_async_io_uring_notification_has_pending_consumers(notification,
                                                               epoch);
    if (!native_owned && has_pending_consumers) {
      notification->failure =
          iree_async_io_uring_notification_arm_locked(proactor, notification);
      if (!iree_status_is_ok(notification->failure)) {
        // Nothing was staged. Dispatch this native failure to all consumers.
        continue;
      }
      armed = true;
    }
    break;
  }

  if (!has_pending_consumers &&
      iree_any_bit_set(notification->state,
                       IREE_ASYNC_IO_URING_NOTIFICATION_POLL_IN_FLIGHT) &&
      !iree_any_bit_set(notification->state,
                        IREE_ASYNC_IO_URING_NOTIFICATION_RETIRING)) {
    notification->state |= IREE_ASYNC_IO_URING_NOTIFICATION_RETIRING;
    // The embedded, initialized, unlinked HANDLE_POLL satisfies the API's
    // infallible admission preconditions. Only native issuance can fail.
    IREE_CHECK_OK(iree_async_proactor_request_cancel(
        &proactor->base, &notification->poll.base, &notification->cancel));
  }

  // Other live consumers can retain the monitor. The last consumers cannot
  // return ownership until both the native poll and cancellation key retire.
  bool can_detach =
      has_pending_consumers ||
      !iree_any_bit_set(notification->state,
                        IREE_ASYNC_IO_URING_NOTIFICATION_POLL_IN_FLIGHT |
                            IREE_ASYNC_IO_URING_NOTIFICATION_RETIRING);
  if (can_detach) {
    iree_async_operation_t** link = &notification->waits;
    iree_async_operation_t** ready_tail = &ready;
    while (*link) {
      iree_async_operation_t* operation = *link;
      if (iree_async_io_uring_notification_wait_is_ready(
              (iree_async_notification_wait_operation_t*)operation, epoch,
              iree_status_code(notification->failure))) {
        *link = operation->next;
        operation->next = NULL;
        iree_async_operation_set_internal_flags(
            operation, IREE_ASYNC_IO_URING_NOTIFICATION_WAIT_DETACHED);
        *ready_tail = operation;
        ready_tail = &operation->next;
      } else {
        link = &operation->next;
      }
    }
    iree_async_relay_t** relay_link = &notification->relays;
    while (*relay_link) {
      iree_async_relay_t* relay = *relay_link;
      if (relay->platform.io_uring.state ==
          IREE_ASYNC_IO_URING_RELAY_STATE_ACTIVE) {
        relay_link = &relay->platform.io_uring.notification_relay_next;
        continue;
      }
      *relay_link = relay->platform.io_uring.notification_relay_next;
      relay->platform.io_uring.notification_relay_next = NULL;
      if (relay->platform.io_uring.state !=
              IREE_ASYNC_IO_URING_RELAY_STATE_FAULTED ||
          !iree_any_bit_set(relay->flags, IREE_ASYNC_RELAY_FLAG_PERSISTENT)) {
        relay->platform.io_uring.notification_relay_next = retired_relays;
        retired_relays = relay;
      }
    }
  }
  iree_status_t failure =
      ready ? iree_status_clone(notification->failure) : iree_ok_status();
  notification->state &= ~IREE_ASYNC_IO_URING_NOTIFICATION_PENDING;
  iree_slim_mutex_unlock(&notification->mutex);

  // No further access to the notification or its borrowed primitives. The
  // terminal callbacks below may release the last resource owners.
  iree_async_io_uring_notification_complete_waits(proactor, ready, failure);
  while (retired_relays) {
    iree_async_relay_t* relay = retired_relays;
    retired_relays = relay->platform.io_uring.notification_relay_next;
    relay->platform.io_uring.notification_relay_next = NULL;
    iree_async_io_uring_relay_cleanup(proactor, relay);
  }
  return armed;
}

bool iree_async_io_uring_notification_drain_pending(
    iree_async_proactor_io_uring_t* proactor) {
  iree_atomic_slist_entry_t* head = NULL;
  iree_atomic_slist_entry_t* tail = NULL;
  iree_atomic_slist_flush(&proactor->pending_notifications,
                          IREE_ATOMIC_SLIST_FLUSH_ORDER_APPROXIMATE_FIFO, &head,
                          &tail);
  bool armed = false;
  while (head) {
    iree_atomic_slist_entry_t* entry = head;
    head = entry->next;
    iree_async_io_uring_notification_t* notification =
        (iree_async_io_uring_notification_t*)((char*)entry -
                                              offsetof(
                                                  iree_async_io_uring_notification_t,
                                                  pending_entry));
    armed |= iree_async_io_uring_notification_dispatch(proactor, notification);
  }
  return armed;
}

void iree_async_io_uring_notification_discard_pending(
    iree_async_proactor_io_uring_t* proactor) {
  iree_atomic_slist_entry_t* head = NULL;
  iree_atomic_slist_entry_t* tail = NULL;
  iree_atomic_slist_flush(&proactor->pending_notifications,
                          IREE_ATOMIC_SLIST_FLUSH_ORDER_APPROXIMATE_FIFO, &head,
                          &tail);
  while (head) {
    iree_atomic_slist_entry_t* entry = head;
    head = entry->next;
    iree_async_io_uring_notification_t* notification =
        (iree_async_io_uring_notification_t*)((char*)entry -
                                              offsetof(
                                                  iree_async_io_uring_notification_t,
                                                  pending_entry));
    notification->state &= ~IREE_ASYNC_IO_URING_NOTIFICATION_PENDING;
  }
}

void iree_async_io_uring_notification_detach_relay_after_ring_close(
    iree_async_relay_t* relay) {
  iree_async_io_uring_notification_t* notification =
      iree_async_io_uring_notification_cast(relay->source.notification);
  if (notification->cancel.phase == IREE_ASYNC_CANCEL_REQUEST_PHASE_QUEUED) {
    iree_async_proactor_issue_cancel_request(notification->base.proactor,
                                             &notification->cancel);
  }
  notification->cancel.phase = IREE_ASYNC_CANCEL_REQUEST_PHASE_IDLE;
  notification->state &= ~(IREE_ASYNC_IO_URING_NOTIFICATION_POLL_IN_FLIGHT |
                           IREE_ASYNC_IO_URING_NOTIFICATION_RETIRING);
  iree_async_relay_t** link = &notification->relays;
  while (*link && *link != relay) {
    link = &(*link)->platform.io_uring.notification_relay_next;
  }
  if (*link) {
    *link = relay->platform.io_uring.notification_relay_next;
  }
  relay->platform.io_uring.notification_relay_next = NULL;
}
