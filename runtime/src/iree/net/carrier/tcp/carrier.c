// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/carrier/tcp/carrier.h"

#include <string.h>

#include "iree/async/operations/net.h"
#include "iree/base/internal/math.h"
#include "iree/base/threading/mutex.h"

typedef struct iree_net_tcp_carrier_t iree_net_tcp_carrier_t;

// Send-slot ownership transitions are serialized by the carrier mutex. A
// submitted or cancelling slot belongs to the kernel; a completing slot belongs
// to the owning proactor callback.
typedef enum iree_net_tcp_send_slot_state_e {
  IREE_NET_TCP_SEND_SLOT_STATE_FREE = 0,
  IREE_NET_TCP_SEND_SLOT_STATE_RESERVED = 1,
  IREE_NET_TCP_SEND_SLOT_STATE_QUEUED = 2,
  IREE_NET_TCP_SEND_SLOT_STATE_SUBMITTED = 3,
  IREE_NET_TCP_SEND_SLOT_STATE_CANCELLING = 4,
  IREE_NET_TCP_SEND_SLOT_STATE_COMPLETING = 5,
  IREE_NET_TCP_SEND_SLOT_STATE_DETACHED = 6,
} iree_net_tcp_send_slot_state_t;

// One bounded logical send or direct-write reservation.
typedef struct iree_net_tcp_send_slot_t {
  // Socket operation; first for completion downcasting.
  iree_async_socket_send_operation_t operation;

  // Next slot in the send FIFO or a detached completion list.
  struct iree_net_tcp_send_slot_t* next;

  // Stable descriptors retained across queueing and partial sends.
  iree_async_span_t spans[IREE_ASYNC_SOCKET_SEND_MAX_BUFFERS];

  // Completion callback installed for ordinary and committed sends.
  iree_net_send_completion_callback_t completion_callback;

  // Carrier-owned payload allocated for a direct-write reservation.
  void* reservation_buffer;

  // Total logical payload length.
  iree_host_size_t total_length;

  // Payload bytes completed by prior socket operations.
  iree_host_size_t bytes_transferred;

  // Number of valid descriptors in |spans|.
  iree_host_size_t span_count;

  // Index of the first descriptor not fully sent.
  iree_host_size_t first_span;

  // Generation used to prevent stale reservation handle aliasing.
  uint32_t reservation_generation;

  // Current reservation handle, or zero outside RESERVED state.
  iree_net_carrier_send_handle_t reservation_handle;

  // Current ownership state.
  iree_net_tcp_send_slot_state_t state;

  // True while the slot owns one retain on each registered region.
  bool regions_retained;
} iree_net_tcp_send_slot_t;

typedef enum iree_net_tcp_receive_state_e {
  IREE_NET_TCP_RECEIVE_STATE_RETIRED = 0,
  IREE_NET_TCP_RECEIVE_STATE_SUBMITTED = 1,
  IREE_NET_TCP_RECEIVE_STATE_CANCELLING = 2,
  IREE_NET_TCP_RECEIVE_STATE_COMPLETING = 3,
  IREE_NET_TCP_RECEIVE_STATE_PAUSED = 4,
} iree_net_tcp_receive_state_t;

typedef enum iree_net_tcp_receive_lease_state_e {
  IREE_NET_TCP_RECEIVE_LEASE_STATE_RELEASED = 0,
  IREE_NET_TCP_RECEIVE_LEASE_STATE_PENDING = 1,
  IREE_NET_TCP_RECEIVE_LEASE_STATE_RETAINED = 2,
} iree_net_tcp_receive_lease_state_t;

typedef enum iree_net_tcp_carrier_flag_bits_e {
  IREE_NET_TCP_CARRIER_FLAG_NONE = 0u,

  // One send lane is submitted or owned by its completion callback.
  IREE_NET_TCP_CARRIER_FLAG_SEND_DISPATCH_ACTIVE = 1u << 0,
  IREE_NET_TCP_CARRIER_FLAG_SEND_SHUTDOWN_INITIATED = 1u << 1,
  IREE_NET_TCP_CARRIER_FLAG_SOCKET_WRITE_SHUTDOWN_ISSUED = 1u << 2,
} iree_net_tcp_carrier_flag_bits_t;
typedef uint32_t iree_net_tcp_carrier_flags_t;

// Per-buffer wrapper restoring receive progress when a lease returns.
typedef struct iree_net_tcp_receive_lease_context_t {
  // Carrier retained only while the consumer owns the wrapped lease.
  iree_net_tcp_carrier_t* carrier;

  // Original callback returning this buffer to its source.
  iree_async_buffer_recycle_callback_t recycle;

  // Current wrapper ownership state.
  iree_atomic_int32_t state;
} iree_net_tcp_receive_lease_context_t;

struct iree_net_tcp_carrier_t {
  // Base carrier; must be first for upcasting.
  iree_net_carrier_t base;

  // Serializes send admission, slot queues, and shutdown publication.
  iree_slim_mutex_t mutex;

  // Proactor owning socket operations. Retained.
  iree_async_proactor_t* proactor;

  // Connected TCP socket. Retained.
  iree_async_socket_t* socket;

  // Pool supplying receive buffers. Retained.
  iree_async_buffer_pool_t* receive_pool;

  // Contiguous bounded send slot storage.
  iree_net_tcp_send_slot_t* send_slots;

  // Number of entries in |send_slots|.
  uint32_t send_slot_count;

  // Number of ordinary sends and reservations currently owned.
  uint32_t send_slots_in_use;

  // First committed send waiting for socket submission.
  iree_net_tcp_send_slot_t* send_queue_head;

  // Last committed send waiting for socket submission.
  iree_net_tcp_send_slot_t* send_queue_tail;

  // Mutex-protected dispatch and shutdown state.
  iree_net_tcp_carrier_flags_t flags;

  // Single receive operation reused for the carrier lifetime.
  iree_async_socket_recv_pool_operation_t receive_operation;

  // Mutex-protected receive operation ownership state.
  iree_net_tcp_receive_state_t receive_state;

  // Incremented after every wrapped receive buffer is recycled.
  iree_atomic_int32_t returned_buffer_epoch;

  // Number of receive leases currently retained by consumers.
  iree_atomic_int32_t retained_receive_lease_count;

  // Per-buffer wrappers indexed by receive lease buffer index.
  iree_net_tcp_receive_lease_context_t* receive_lease_contexts;

  // Number of entries in |receive_lease_contexts|.
  iree_host_size_t receive_lease_context_count;

  // Callback invoked after every accepted operation drains.
  struct {
    // Function invoked after transition to DEACTIVATED.
    iree_net_carrier_deactivate_callback_fn_t fn;

    // Opaque value passed to |fn|.
    void* user_data;
  } deactivate_callback;
};

static iree_net_tcp_carrier_t* iree_net_tcp_carrier_cast(
    iree_net_carrier_t* base_carrier) {
  return (iree_net_tcp_carrier_t*)base_carrier;
}

static void iree_net_tcp_carrier_maybe_complete_deactivation(
    iree_net_tcp_carrier_t* carrier) {
  if (iree_atomic_load(&carrier->base.pending_operations,
                       iree_memory_order_acquire) != 0) {
    return;
  }
  if (!iree_net_carrier_try_transition_state(
          &carrier->base, IREE_NET_CARRIER_STATE_DRAINING,
          IREE_NET_CARRIER_STATE_DEACTIVATED)) {
    return;
  }

  iree_net_carrier_deactivate_callback_fn_t callback =
      carrier->deactivate_callback.fn;
  void* user_data = carrier->deactivate_callback.user_data;
  carrier->deactivate_callback.fn = NULL;
  carrier->deactivate_callback.user_data = NULL;
  callback(user_data);
}

// Retirement must be the caller's final carrier access.
static void iree_net_tcp_carrier_retire_pending_operation(
    iree_net_tcp_carrier_t* carrier) {
  if (iree_net_carrier_retire_pending_operation(&carrier->base)) {
    iree_net_tcp_carrier_maybe_complete_deactivation(carrier);
  }
}

static iree_status_t iree_net_tcp_validate_send_span(iree_async_span_t span) {
  if (span.length == 0) {
    return iree_ok_status();
  }
  if (!iree_async_span_is_cpu_accessible(span)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "TCP send span is not CPU-accessible");
  }
  if (!span.region) {
    if (span.offset == 0) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "TCP send span has null storage");
    }
    return iree_ok_status();
  }
  if (!iree_any_bit_set(span.region->access_flags,
                        IREE_ASYNC_BUFFER_ACCESS_FLAG_READ)) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "TCP send region does not permit reads");
  }
  if (span.offset > span.region->length ||
      span.length > span.region->length - span.offset) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "TCP send span range [%" PRIhsz ", %" PRIhsz
                            ") exceeds its registered region length %" PRIhsz,
                            span.offset, span.offset + span.length,
                            span.region->length);
  }
  return iree_ok_status();
}

static iree_net_tcp_send_slot_t* iree_net_tcp_find_free_send_slot_locked(
    iree_net_tcp_carrier_t* carrier) {
  for (uint32_t i = 0; i < carrier->send_slot_count; ++i) {
    if (carrier->send_slots[i].state == IREE_NET_TCP_SEND_SLOT_STATE_FREE) {
      return &carrier->send_slots[i];
    }
  }
  return NULL;
}

static void iree_net_tcp_enqueue_send_locked(iree_net_tcp_carrier_t* carrier,
                                             iree_net_tcp_send_slot_t* slot) {
  IREE_ASSERT(slot->state == IREE_NET_TCP_SEND_SLOT_STATE_QUEUED);
  slot->next = NULL;
  if (carrier->send_queue_tail) {
    carrier->send_queue_tail->next = slot;
  } else {
    carrier->send_queue_head = slot;
  }
  carrier->send_queue_tail = slot;
}

static iree_net_tcp_send_slot_t* iree_net_tcp_pop_send_locked(
    iree_net_tcp_carrier_t* carrier) {
  iree_net_tcp_send_slot_t* slot = carrier->send_queue_head;
  if (!slot) {
    return NULL;
  }
  carrier->send_queue_head = slot->next;
  if (!carrier->send_queue_head) {
    carrier->send_queue_tail = NULL;
  }
  slot->next = NULL;
  slot->state = IREE_NET_TCP_SEND_SLOT_STATE_SUBMITTED;
  return slot;
}

static iree_net_tcp_send_slot_t* iree_net_tcp_claim_send_dispatch_locked(
    iree_net_tcp_carrier_t* carrier) {
  if (iree_any_bit_set(carrier->flags,
                       IREE_NET_TCP_CARRIER_FLAG_SEND_DISPATCH_ACTIVE) ||
      !carrier->send_queue_head) {
    return NULL;
  }
  carrier->flags |= IREE_NET_TCP_CARRIER_FLAG_SEND_DISPATCH_ACTIVE;
  return iree_net_tcp_pop_send_locked(carrier);
}

static iree_net_tcp_send_slot_t* iree_net_tcp_find_submitted_send_locked(
    iree_net_tcp_carrier_t* carrier) {
  iree_net_tcp_send_slot_t* submitted_slot = NULL;
  for (uint32_t i = 0; i < carrier->send_slot_count; ++i) {
    iree_net_tcp_send_slot_t* slot = &carrier->send_slots[i];
    if (slot->state != IREE_NET_TCP_SEND_SLOT_STATE_SUBMITTED) {
      continue;
    }
    IREE_ASSERT(!submitted_slot,
                "TCP carrier submitted multiple socket sends concurrently");
    submitted_slot = slot;
  }
  return submitted_slot;
}

static void iree_net_tcp_send_slot_release_resources(
    iree_net_tcp_carrier_t* carrier, iree_net_tcp_send_slot_t* slot) {
  if (slot->regions_retained) {
    iree_async_span_list_release_regions(
        iree_async_span_list_make(slot->spans, slot->span_count));
    slot->regions_retained = false;
  }
  iree_allocator_free(carrier->base.host_allocator, slot->reservation_buffer);
  slot->reservation_buffer = NULL;
}

static void iree_net_tcp_recycle_send_slot_locked(
    iree_net_tcp_carrier_t* carrier, iree_net_tcp_send_slot_t* slot) {
  IREE_ASSERT(slot->state != IREE_NET_TCP_SEND_SLOT_STATE_FREE);
  IREE_ASSERT(carrier->send_slots_in_use > 0);
  slot->next = NULL;
  slot->completion_callback = (iree_net_send_completion_callback_t){0};
  slot->total_length = 0;
  slot->bytes_transferred = 0;
  slot->span_count = 0;
  slot->first_span = 0;
  slot->reservation_handle = 0;
  slot->state = IREE_NET_TCP_SEND_SLOT_STATE_FREE;
  --carrier->send_slots_in_use;
}

static iree_net_tcp_send_slot_t* iree_net_tcp_detach_send_queue_locked(
    iree_net_tcp_carrier_t* carrier, iree_net_tcp_send_slot_t** out_tail) {
  iree_net_tcp_send_slot_t* head = carrier->send_queue_head;
  iree_net_tcp_send_slot_t* tail = carrier->send_queue_tail;
  carrier->send_queue_head = NULL;
  carrier->send_queue_tail = NULL;

  for (iree_net_tcp_send_slot_t* slot = head; slot; slot = slot->next) {
    IREE_ASSERT(slot->state == IREE_NET_TCP_SEND_SLOT_STATE_QUEUED);
    slot->state = IREE_NET_TCP_SEND_SLOT_STATE_DETACHED;
  }
  if (out_tail) {
    *out_tail = tail;
  }
  return head;
}

// Detaches caller-synchronized reservations during explicit deactivation.
// Asynchronous failure leaves writable reservation storage with its caller.
static iree_net_tcp_send_slot_t* iree_net_tcp_detach_reservations_locked(
    iree_net_tcp_carrier_t* carrier) {
  iree_net_tcp_send_slot_t* head = NULL;
  iree_net_tcp_send_slot_t* tail = NULL;
  for (uint32_t i = 0; i < carrier->send_slot_count; ++i) {
    iree_net_tcp_send_slot_t* slot = &carrier->send_slots[i];
    if (slot->state != IREE_NET_TCP_SEND_SLOT_STATE_RESERVED) {
      continue;
    }
    slot->reservation_handle = 0;
    slot->state = IREE_NET_TCP_SEND_SLOT_STATE_DETACHED;
    slot->next = NULL;
    if (tail) {
      tail->next = slot;
    } else {
      head = slot;
    }
    tail = slot;
  }
  return head;
}

static iree_status_t iree_net_tcp_cancel_operation_locked(
    iree_net_tcp_carrier_t* carrier, iree_async_operation_t* operation) {
  iree_status_t status =
      iree_async_proactor_cancel(carrier->proactor, operation);
  if (iree_status_is_not_found(status)) {
    iree_status_free(status);
    return iree_ok_status();
  }
  return status;
}

static iree_status_t iree_net_tcp_request_receive_cancellation_locked(
    iree_net_tcp_carrier_t* carrier) {
  if (carrier->receive_state != IREE_NET_TCP_RECEIVE_STATE_SUBMITTED) {
    return iree_ok_status();
  }
  iree_status_t status = iree_net_tcp_cancel_operation_locked(
      carrier, &carrier->receive_operation.base);
  if (iree_status_is_ok(status)) {
    carrier->receive_state = IREE_NET_TCP_RECEIVE_STATE_CANCELLING;
  }
  return status;
}

static iree_status_t iree_net_tcp_request_send_cancellation_locked(
    iree_net_tcp_carrier_t* carrier) {
  iree_net_tcp_send_slot_t* submitted_slot =
      iree_net_tcp_find_submitted_send_locked(carrier);
  if (!submitted_slot) {
    return iree_ok_status();
  }
  iree_status_t status = iree_net_tcp_cancel_operation_locked(
      carrier, &submitted_slot->operation.base);
  if (iree_status_is_ok(status)) {
    submitted_slot->state = IREE_NET_TCP_SEND_SLOT_STATE_CANCELLING;
  }
  return status;
}

static bool iree_net_tcp_claim_write_shutdown_locked(
    iree_net_tcp_carrier_t* carrier) {
  if (!iree_any_bit_set(carrier->flags,
                        IREE_NET_TCP_CARRIER_FLAG_SEND_SHUTDOWN_INITIATED) ||
      iree_any_bit_set(
          carrier->flags,
          IREE_NET_TCP_CARRIER_FLAG_SOCKET_WRITE_SHUTDOWN_ISSUED) ||
      carrier->send_slots_in_use != 0 ||
      iree_net_carrier_state(&carrier->base) != IREE_NET_CARRIER_STATE_ACTIVE) {
    return false;
  }
  carrier->flags |= IREE_NET_TCP_CARRIER_FLAG_SOCKET_WRITE_SHUTDOWN_ISSUED;
  return true;
}

static void iree_net_tcp_complete_detached_sends(
    iree_net_tcp_carrier_t* carrier, iree_net_tcp_send_slot_t* slot,
    iree_status_code_t status_code) {
  while (slot) {
    IREE_ASSERT(slot->state == IREE_NET_TCP_SEND_SLOT_STATE_DETACHED);
    iree_net_tcp_send_slot_t* next = slot->next;
    const iree_net_send_completion_callback_t completion_callback =
        slot->completion_callback;
    const iree_host_size_t bytes_transferred = slot->bytes_transferred;
    iree_net_tcp_send_slot_release_resources(carrier, slot);

    iree_slim_mutex_lock(&carrier->mutex);
    iree_net_tcp_recycle_send_slot_locked(carrier, slot);
    iree_slim_mutex_unlock(&carrier->mutex);

    if (completion_callback.fn) {
      completion_callback.fn(
          completion_callback.user_data,
          iree_make_status(status_code,
                           "TCP send did not complete before transport stop"),
          bytes_transferred);
    }
    iree_net_tcp_carrier_retire_pending_operation(carrier);
    slot = next;
  }
}

static void iree_net_tcp_reject_detached_send(iree_net_tcp_carrier_t* carrier,
                                              iree_net_tcp_send_slot_t* slot) {
  IREE_ASSERT(slot->state == IREE_NET_TCP_SEND_SLOT_STATE_DETACHED);
  iree_net_tcp_send_slot_release_resources(carrier, slot);
  iree_slim_mutex_lock(&carrier->mutex);
  iree_net_tcp_recycle_send_slot_locked(carrier, slot);
  iree_slim_mutex_unlock(&carrier->mutex);
  iree_net_tcp_carrier_retire_pending_operation(carrier);
}

// Publishes terminal failure and retires a paused receive as its final carrier
// access. Callers retain a separate operation or lifetime reference.
static void iree_net_tcp_publish_failure(iree_net_tcp_carrier_t* carrier,
                                         iree_status_t status) {
  IREE_ASSERT(!iree_status_is_ok(status));
  iree_status_t receive_cancel_status = iree_ok_status();
  iree_status_t send_cancel_status = iree_ok_status();
  bool retire_paused_receive = false;

  iree_slim_mutex_lock(&carrier->mutex);
  carrier->flags |= IREE_NET_TCP_CARRIER_FLAG_SEND_SHUTDOWN_INITIATED |
                    IREE_NET_TCP_CARRIER_FLAG_SOCKET_WRITE_SHUTDOWN_ISSUED;
  // Accepted sends stay on their single dispatch lane so only its proactor
  // callback can deliver their terminal completions.
  if (carrier->receive_state == IREE_NET_TCP_RECEIVE_STATE_PAUSED) {
    carrier->receive_state = IREE_NET_TCP_RECEIVE_STATE_RETIRED;
    retire_paused_receive = true;
  } else {
    receive_cancel_status =
        iree_net_tcp_request_receive_cancellation_locked(carrier);
  }
  send_cancel_status = iree_net_tcp_request_send_cancellation_locked(carrier);
  iree_slim_mutex_unlock(&carrier->mutex);

  status = iree_status_join(status, receive_cancel_status);
  status = iree_status_join(status, send_cancel_status);
  status = iree_status_join(
      status, iree_async_socket_shutdown(carrier->socket,
                                         IREE_ASYNC_SOCKET_SHUTDOWN_BOTH));
  iree_net_carrier_report_terminal_error(&carrier->base, status);
  if (retire_paused_receive) {
    iree_net_tcp_carrier_retire_pending_operation(carrier);
  }
}

static void iree_net_tcp_fail_without_operation(iree_net_tcp_carrier_t* carrier,
                                                iree_status_t status) {
  iree_net_carrier_retain(&carrier->base);
  iree_net_tcp_publish_failure(carrier, status);
  iree_net_carrier_release(&carrier->base);
}

static void iree_net_tcp_issue_deferred_write_shutdown(
    iree_net_tcp_carrier_t* carrier) {
  iree_status_t status = iree_async_socket_shutdown(
      carrier->socket, IREE_ASYNC_SOCKET_SHUTDOWN_WRITE);
  if (!iree_status_is_ok(status)) {
    iree_net_tcp_fail_without_operation(carrier, status);
  }
}

static bool iree_net_tcp_advance_send_slot(iree_net_tcp_send_slot_t* slot,
                                           iree_host_size_t bytes_transferred) {
  iree_host_size_t remaining = bytes_transferred;
  while (remaining > 0 && slot->first_span < slot->span_count) {
    iree_async_span_t* span = &slot->spans[slot->first_span];
    if (remaining >= span->length) {
      remaining -= span->length;
      ++slot->first_span;
    } else {
      span->offset += remaining;
      span->length -= remaining;
      remaining = 0;
    }
  }
  if (remaining != 0) {
    return false;
  }
  slot->bytes_transferred += bytes_transferred;
  return true;
}

static void iree_net_tcp_process_send_result(
    iree_net_tcp_carrier_t* carrier, iree_net_tcp_send_slot_t* slot,
    iree_status_t status, iree_host_size_t bytes_sent,
    iree_async_completion_flags_t flags);

static void iree_net_tcp_socket_send_completed(
    void* user_data, iree_async_operation_t* operation, iree_status_t status,
    iree_async_completion_flags_t flags) {
  iree_net_tcp_carrier_t* carrier = (iree_net_tcp_carrier_t*)user_data;
  iree_net_tcp_send_slot_t* slot = (iree_net_tcp_send_slot_t*)operation;
  iree_slim_mutex_lock(&carrier->mutex);
  IREE_ASSERT(slot->state == IREE_NET_TCP_SEND_SLOT_STATE_SUBMITTED ||
                  slot->state == IREE_NET_TCP_SEND_SLOT_STATE_CANCELLING,
              "TCP send completed from state %d", (int)slot->state);
  slot->state = IREE_NET_TCP_SEND_SLOT_STATE_COMPLETING;
  const iree_host_size_t bytes_sent = slot->operation.bytes_sent;
  iree_slim_mutex_unlock(&carrier->mutex);
  iree_net_tcp_process_send_result(carrier, slot, status, bytes_sent, flags);
}

static iree_status_t iree_net_tcp_submit_send_slot_locked(
    iree_net_tcp_carrier_t* carrier, iree_net_tcp_send_slot_t* slot) {
  IREE_ASSERT(slot->state == IREE_NET_TCP_SEND_SLOT_STATE_SUBMITTED);
  IREE_ASSERT(slot->first_span < slot->span_count);
  iree_async_operation_zero(&slot->operation.base, sizeof(slot->operation));
  iree_async_operation_initialize(&slot->operation.base,
                                  IREE_ASYNC_OPERATION_TYPE_SOCKET_SEND,
                                  IREE_ASYNC_OPERATION_FLAG_NONE,
                                  iree_net_tcp_socket_send_completed, carrier);
  slot->operation.socket = carrier->socket;
  slot->operation.buffers = iree_async_span_list_make(
      &slot->spans[slot->first_span], slot->span_count - slot->first_span);
  slot->operation.send_flags = IREE_ASYNC_SOCKET_SEND_FLAG_NONE;
  return iree_async_proactor_submit_one(carrier->proactor,
                                        &slot->operation.base);
}

static iree_status_t iree_net_tcp_start_send_dispatch_locked(
    iree_net_tcp_carrier_t* carrier,
    iree_net_tcp_send_slot_t** out_rejected_slot) {
  *out_rejected_slot = NULL;
  iree_net_tcp_send_slot_t* slot =
      iree_net_tcp_claim_send_dispatch_locked(carrier);
  if (!slot) {
    return iree_ok_status();
  }
  iree_status_t status = iree_net_tcp_submit_send_slot_locked(carrier, slot);
  if (!iree_status_is_ok(status)) {
    slot->state = IREE_NET_TCP_SEND_SLOT_STATE_DETACHED;
    carrier->flags &= ~IREE_NET_TCP_CARRIER_FLAG_SEND_DISPATCH_ACTIVE;
    *out_rejected_slot = slot;
  }
  return status;
}

static void iree_net_tcp_process_send_result(
    iree_net_tcp_carrier_t* carrier, iree_net_tcp_send_slot_t* slot,
    iree_status_t status, iree_host_size_t bytes_sent,
    iree_async_completion_flags_t flags) {
  if (iree_any_bit_set(flags, IREE_ASYNC_COMPLETION_FLAG_CANCELLED) &&
      iree_status_is_ok(status)) {
    status = iree_make_status(IREE_STATUS_CANCELLED,
                              "TCP socket send was cancelled");
  }

  const iree_host_size_t remaining_length =
      slot->total_length - slot->bytes_transferred;
  if (iree_status_is_ok(status)) {
    if (bytes_sent == 0) {
      status = iree_make_status(IREE_STATUS_UNAVAILABLE,
                                "TCP socket send made no forward progress");
    } else if (bytes_sent > remaining_length ||
               !iree_net_tcp_advance_send_slot(slot, bytes_sent)) {
      status = iree_make_status(IREE_STATUS_INTERNAL,
                                "TCP socket reported %" PRIhsz
                                " bytes for a %" PRIhsz " byte remaining send",
                                bytes_sent, remaining_length);
    }
  }

  if (iree_status_is_ok(status) &&
      slot->bytes_transferred < slot->total_length) {
    bool resubmitted = false;
    iree_status_t submit_status = iree_ok_status();
    iree_slim_mutex_lock(&carrier->mutex);
    if (iree_net_carrier_state(&carrier->base) ==
            IREE_NET_CARRIER_STATE_ACTIVE &&
        !iree_net_carrier_has_terminal_error(&carrier->base)) {
      slot->state = IREE_NET_TCP_SEND_SLOT_STATE_SUBMITTED;
      submit_status = iree_net_tcp_submit_send_slot_locked(carrier, slot);
      if (iree_status_is_ok(submit_status)) {
        resubmitted = true;
      } else {
        slot->state = IREE_NET_TCP_SEND_SLOT_STATE_COMPLETING;
      }
    }
    iree_slim_mutex_unlock(&carrier->mutex);
    if (resubmitted) {
      iree_status_free(status);
      return;
    }
    iree_status_free(status);
    if (!iree_status_is_ok(submit_status)) {
      status = submit_status;
    } else {
      status = iree_net_carrier_clone_terminal_error(&carrier->base);
      if (iree_status_is_ok(status)) {
        status =
            iree_make_status(IREE_STATUS_CANCELLED,
                             "TCP send interrupted by carrier deactivation");
      }
    }
  }

  const iree_net_send_completion_callback_t completion_callback =
      slot->completion_callback;
  const iree_host_size_t total_bytes_transferred = slot->bytes_transferred;
  const bool send_succeeded = iree_status_is_ok(status);
  iree_net_tcp_send_slot_release_resources(carrier, slot);

  iree_slim_mutex_lock(&carrier->mutex);
  iree_net_tcp_recycle_send_slot_locked(carrier, slot);
  if (!send_succeeded) {
    carrier->flags |= IREE_NET_TCP_CARRIER_FLAG_SEND_SHUTDOWN_INITIATED;
  }
  iree_slim_mutex_unlock(&carrier->mutex);

  if (!send_succeeded &&
      iree_net_carrier_state(&carrier->base) == IREE_NET_CARRIER_STATE_ACTIVE &&
      !iree_net_carrier_has_terminal_error(&carrier->base)) {
    iree_net_tcp_publish_failure(carrier, iree_status_clone(status));
  }

  completion_callback.fn(completion_callback.user_data, status,
                         total_bytes_transferred);

  iree_net_tcp_send_slot_t* next_slot = NULL;
  iree_net_tcp_send_slot_t* detached_sends = NULL;
  iree_status_t next_submit_status = iree_ok_status();
  bool issue_write_shutdown = false;
  iree_slim_mutex_lock(&carrier->mutex);
  if (send_succeeded &&
      iree_net_carrier_state(&carrier->base) == IREE_NET_CARRIER_STATE_ACTIVE &&
      !iree_net_carrier_has_terminal_error(&carrier->base)) {
    next_slot = iree_net_tcp_pop_send_locked(carrier);
    if (next_slot) {
      next_submit_status =
          iree_net_tcp_submit_send_slot_locked(carrier, next_slot);
      if (!iree_status_is_ok(next_submit_status)) {
        next_slot->state = IREE_NET_TCP_SEND_SLOT_STATE_COMPLETING;
      }
    }
  } else {
    detached_sends =
        iree_net_tcp_detach_send_queue_locked(carrier, /*out_tail=*/NULL);
  }
  if (!next_slot) {
    carrier->flags &= ~IREE_NET_TCP_CARRIER_FLAG_SEND_DISPATCH_ACTIVE;
  }
  issue_write_shutdown = iree_net_tcp_claim_write_shutdown_locked(carrier);
  iree_slim_mutex_unlock(&carrier->mutex);

  if (detached_sends) {
    iree_status_t terminal_status =
        iree_net_carrier_clone_terminal_error(&carrier->base);
    const iree_status_code_t status_code =
        iree_status_is_ok(terminal_status) ? IREE_STATUS_CANCELLED
                                           : iree_status_code(terminal_status);
    iree_status_free(terminal_status);
    iree_net_tcp_complete_detached_sends(carrier, detached_sends, status_code);
  }
  if (next_slot && !iree_status_is_ok(next_submit_status)) {
    iree_net_tcp_process_send_result(carrier, next_slot, next_submit_status, 0,
                                     IREE_ASYNC_COMPLETION_FLAG_NONE);
  }
  if (issue_write_shutdown) {
    iree_net_tcp_issue_deferred_write_shutdown(carrier);
  }
  iree_net_tcp_carrier_retire_pending_operation(carrier);
}

static void iree_net_tcp_socket_receive_completed(
    void* user_data, iree_async_operation_t* operation, iree_status_t status,
    iree_async_completion_flags_t flags);

static iree_status_t iree_net_tcp_submit_receive_locked(
    iree_net_tcp_carrier_t* carrier) {
  iree_async_socket_recv_pool_operation_t* operation =
      &carrier->receive_operation;
  iree_async_operation_zero(&operation->base, sizeof(*operation));
  iree_async_operation_initialize(
      &operation->base, IREE_ASYNC_OPERATION_TYPE_SOCKET_RECV_POOL,
      IREE_ASYNC_OPERATION_FLAG_NONE, iree_net_tcp_socket_receive_completed,
      carrier);
  operation->socket = carrier->socket;
  operation->pool = carrier->receive_pool;
  return iree_async_proactor_submit_one(carrier->proactor, &operation->base);
}

static iree_status_t iree_net_tcp_continue_receive_locked(
    iree_net_tcp_carrier_t* carrier, bool* out_retire_receive) {
  *out_retire_receive = false;
  if (iree_net_carrier_state(&carrier->base) == IREE_NET_CARRIER_STATE_ACTIVE &&
      !iree_net_carrier_has_terminal_error(&carrier->base)) {
    carrier->receive_state = IREE_NET_TCP_RECEIVE_STATE_SUBMITTED;
    iree_status_t status = iree_net_tcp_submit_receive_locked(carrier);
    if (iree_status_is_ok(status)) {
      return status;
    }
    carrier->receive_state = IREE_NET_TCP_RECEIVE_STATE_RETIRED;
    *out_retire_receive = true;
    return status;
  }
  carrier->receive_state = IREE_NET_TCP_RECEIVE_STATE_RETIRED;
  *out_retire_receive = true;
  return iree_ok_status();
}

// Finishes a receive transition and must be the caller's final carrier access
// when |retire_receive| is true.
static void iree_net_tcp_finish_receive_transition(
    iree_net_tcp_carrier_t* carrier, iree_status_t status,
    bool retire_receive) {
  if (!iree_status_is_ok(status)) {
    iree_net_tcp_publish_failure(carrier, status);
  }
  if (retire_receive) {
    iree_net_tcp_carrier_retire_pending_operation(carrier);
  }
}

static void iree_net_tcp_resume_paused_receive(
    iree_net_tcp_carrier_t* carrier) {
  iree_status_t submit_status = iree_ok_status();
  bool retire_receive = false;
  iree_slim_mutex_lock(&carrier->mutex);
  if (carrier->receive_state == IREE_NET_TCP_RECEIVE_STATE_PAUSED) {
    submit_status =
        iree_net_tcp_continue_receive_locked(carrier, &retire_receive);
  }
  iree_slim_mutex_unlock(&carrier->mutex);
  iree_net_tcp_finish_receive_transition(carrier, submit_status,
                                         retire_receive);
}

static void iree_net_tcp_receive_lease_recycle(void* user_data,
                                               uint32_t buffer_index) {
  iree_net_tcp_receive_lease_context_t* context =
      (iree_net_tcp_receive_lease_context_t*)user_data;
  iree_net_tcp_carrier_t* carrier = context->carrier;
  const iree_async_buffer_recycle_callback_t recycle = context->recycle;
  const int32_t old_state = iree_atomic_exchange(
      &context->state, IREE_NET_TCP_RECEIVE_LEASE_STATE_RELEASED,
      iree_memory_order_acq_rel);
  IREE_ASSERT(old_state != IREE_NET_TCP_RECEIVE_LEASE_STATE_RELEASED,
              "TCP receive lease recycled more than once");

  recycle.fn(recycle.user_data, buffer_index);
  if (old_state == IREE_NET_TCP_RECEIVE_LEASE_STATE_RETAINED) {
    int32_t previous_count = iree_atomic_fetch_sub(
        &carrier->retained_receive_lease_count, 1, iree_memory_order_acq_rel);
    IREE_ASSERT(previous_count > 0,
                "TCP carrier lost a retained receive lease");
  }
  iree_atomic_fetch_add(&carrier->returned_buffer_epoch, 1,
                        iree_memory_order_release);
  iree_net_tcp_resume_paused_receive(carrier);

  if (old_state == IREE_NET_TCP_RECEIVE_LEASE_STATE_RETAINED) {
    iree_net_carrier_release(&carrier->base);
  }
}

static iree_net_tcp_receive_lease_context_t* iree_net_tcp_prepare_receive_lease(
    iree_net_tcp_carrier_t* carrier, iree_async_buffer_lease_t* lease) {
  IREE_ASSERT(lease->release.fn,
              "pool-backed TCP receive produced an unrecyclable lease");
  IREE_ASSERT(lease->buffer_index < carrier->receive_lease_context_count,
              "TCP receive lease buffer index is out of range");
  iree_net_tcp_receive_lease_context_t* context =
      &carrier->receive_lease_contexts[lease->buffer_index];
  IREE_ASSERT(iree_atomic_load(&context->state, iree_memory_order_acquire) ==
                  IREE_NET_TCP_RECEIVE_LEASE_STATE_RELEASED,
              "TCP receive buffer was reused before its prior lease returned");
  context->carrier = carrier;
  context->recycle = lease->release;
  iree_atomic_store(&context->state, IREE_NET_TCP_RECEIVE_LEASE_STATE_PENDING,
                    iree_memory_order_release);
  lease->release.fn = iree_net_tcp_receive_lease_recycle;
  lease->release.user_data = context;
  return context;
}

static void iree_net_tcp_retain_moved_receive_lease(
    iree_net_tcp_carrier_t* carrier,
    iree_net_tcp_receive_lease_context_t* context,
    iree_async_buffer_lease_t* callback_lease) {
  if (callback_lease->release.fn) {
    return;
  }

  iree_net_carrier_retain(&carrier->base);
  iree_atomic_fetch_add(&carrier->retained_receive_lease_count, 1,
                        iree_memory_order_acq_rel);
  int32_t expected_state = IREE_NET_TCP_RECEIVE_LEASE_STATE_PENDING;
  if (!iree_atomic_compare_exchange_strong(
          &context->state, &expected_state,
          IREE_NET_TCP_RECEIVE_LEASE_STATE_RETAINED, iree_memory_order_acq_rel,
          iree_memory_order_acquire)) {
    int32_t previous_count = iree_atomic_fetch_sub(
        &carrier->retained_receive_lease_count, 1, iree_memory_order_acq_rel);
    IREE_ASSERT(previous_count > 0,
                "TCP carrier lost a retained receive lease");
    iree_net_carrier_release(&carrier->base);
  }
}

// Publishes PAUSED while closing the lease-return lost-wakeup race.
static void iree_net_tcp_pause_receive(iree_net_tcp_carrier_t* carrier,
                                       int32_t observed_return_epoch) {
  iree_status_t status = iree_ok_status();
  bool retire_receive = false;
  iree_slim_mutex_lock(&carrier->mutex);
  IREE_ASSERT(carrier->receive_state == IREE_NET_TCP_RECEIVE_STATE_COMPLETING,
              "TCP receive paused from state %d", (int)carrier->receive_state);
  carrier->receive_state = IREE_NET_TCP_RECEIVE_STATE_PAUSED;
  const int32_t current_return_epoch = iree_atomic_load(
      &carrier->returned_buffer_epoch, iree_memory_order_acquire);
  if (iree_net_carrier_state(&carrier->base) != IREE_NET_CARRIER_STATE_ACTIVE ||
      iree_net_carrier_has_terminal_error(&carrier->base) ||
      current_return_epoch != observed_return_epoch) {
    status = iree_net_tcp_continue_receive_locked(carrier, &retire_receive);
  }
  iree_slim_mutex_unlock(&carrier->mutex);
  iree_net_tcp_finish_receive_transition(carrier, status, retire_receive);
}

static void iree_net_tcp_retire_completing_receive(
    iree_net_tcp_carrier_t* carrier) {
  iree_slim_mutex_lock(&carrier->mutex);
  IREE_ASSERT(carrier->receive_state == IREE_NET_TCP_RECEIVE_STATE_COMPLETING,
              "TCP receive retired from state %d", (int)carrier->receive_state);
  carrier->receive_state = IREE_NET_TCP_RECEIVE_STATE_RETIRED;
  iree_slim_mutex_unlock(&carrier->mutex);
  iree_net_tcp_carrier_retire_pending_operation(carrier);
}

static void iree_net_tcp_socket_receive_completed(
    void* user_data, iree_async_operation_t* operation, iree_status_t status,
    iree_async_completion_flags_t flags) {
  iree_net_tcp_carrier_t* carrier = (iree_net_tcp_carrier_t*)user_data;
  iree_async_socket_recv_pool_operation_t* receive_operation =
      (iree_async_socket_recv_pool_operation_t*)operation;

  iree_slim_mutex_lock(&carrier->mutex);
  IREE_ASSERT(
      carrier->receive_state == IREE_NET_TCP_RECEIVE_STATE_SUBMITTED ||
          carrier->receive_state == IREE_NET_TCP_RECEIVE_STATE_CANCELLING,
      "TCP receive completed from state %d", (int)carrier->receive_state);
  carrier->receive_state = IREE_NET_TCP_RECEIVE_STATE_COMPLETING;
  iree_slim_mutex_unlock(&carrier->mutex);

  if (iree_net_carrier_state(&carrier->base) != IREE_NET_CARRIER_STATE_ACTIVE) {
    iree_status_free(status);
    iree_async_buffer_lease_release(&receive_operation->lease);
    iree_net_tcp_retire_completing_receive(carrier);
    return;
  }

  if (iree_any_bit_set(flags, IREE_ASYNC_COMPLETION_FLAG_CANCELLED) &&
      iree_status_is_ok(status)) {
    status = iree_make_status(IREE_STATUS_CANCELLED,
                              "TCP socket receive was cancelled");
  }
  if (!iree_status_is_ok(status)) {
    iree_async_buffer_lease_release(&receive_operation->lease);
    iree_net_tcp_publish_failure(carrier, status);
    iree_net_tcp_retire_completing_receive(carrier);
    return;
  }
  iree_status_free(status);

  if (receive_operation->bytes_received == 0) {
    iree_async_buffer_lease_release(&receive_operation->lease);
    iree_status_t handler_status = carrier->base.handlers.on_receive(
        carrier->base.handlers.user_data, iree_async_span_empty(), NULL);
    if (!iree_status_is_ok(handler_status)) {
      iree_net_tcp_publish_failure(carrier, handler_status);
    }
    iree_net_tcp_retire_completing_receive(carrier);
    return;
  }

  IREE_ASSERT(
      receive_operation->bytes_received <= receive_operation->lease.span.length,
      "TCP receive exceeds its leased buffer");
  iree_async_span_t data = iree_async_span_make(
      receive_operation->lease.span.region,
      receive_operation->lease.span.offset, receive_operation->bytes_received);
  iree_net_tcp_receive_lease_context_t* lease_context =
      iree_net_tcp_prepare_receive_lease(carrier, &receive_operation->lease);
  iree_status_t handler_status = carrier->base.handlers.on_receive(
      carrier->base.handlers.user_data, data, &receive_operation->lease);
  iree_net_tcp_retain_moved_receive_lease(carrier, lease_context,
                                          &receive_operation->lease);
  iree_async_buffer_lease_release(&receive_operation->lease);

  if (!iree_status_is_ok(handler_status)) {
    iree_net_tcp_publish_failure(carrier, handler_status);
    iree_net_tcp_retire_completing_receive(carrier);
    return;
  }

  const int32_t return_epoch = iree_atomic_load(&carrier->returned_buffer_epoch,
                                                iree_memory_order_acquire);
  const int32_t retained_lease_count = iree_atomic_load(
      &carrier->retained_receive_lease_count, iree_memory_order_acquire);
  if ((iree_host_size_t)retained_lease_count >=
      carrier->receive_lease_context_count) {
    iree_net_tcp_pause_receive(carrier, return_epoch);
    return;
  }

  iree_status_t submit_status = iree_ok_status();
  bool retire_receive = false;
  iree_slim_mutex_lock(&carrier->mutex);
  IREE_ASSERT(carrier->receive_state == IREE_NET_TCP_RECEIVE_STATE_COMPLETING,
              "TCP receive resumed from state %d", (int)carrier->receive_state);
  submit_status =
      iree_net_tcp_continue_receive_locked(carrier, &retire_receive);
  iree_slim_mutex_unlock(&carrier->mutex);
  iree_net_tcp_finish_receive_transition(carrier, submit_status,
                                         retire_receive);
}

static void iree_net_tcp_carrier_destroy(iree_net_carrier_t* base_carrier) {
  iree_net_tcp_carrier_t* carrier = iree_net_tcp_carrier_cast(base_carrier);
  iree_allocator_t host_allocator = base_carrier->host_allocator;
  const iree_net_carrier_state_t state = iree_net_carrier_state(base_carrier);
  IREE_ASSERT(state == IREE_NET_CARRIER_STATE_CREATED ||
              state == IREE_NET_CARRIER_STATE_DEACTIVATED);
  IREE_ASSERT(iree_atomic_load(&base_carrier->pending_operations,
                               iree_memory_order_acquire) == 0);
  IREE_ASSERT(carrier->send_slots_in_use == 0);
  IREE_ASSERT(!carrier->send_queue_head);
  IREE_ASSERT(iree_atomic_load(&carrier->retained_receive_lease_count,
                               iree_memory_order_acquire) == 0);

  iree_async_socket_release(carrier->socket);
  iree_async_buffer_pool_release(carrier->receive_pool);
  iree_async_proactor_release(carrier->proactor);
  iree_slim_mutex_deinitialize(&carrier->mutex);
  iree_net_carrier_deinitialize(base_carrier);
  iree_allocator_free(host_allocator, carrier);
}

static iree_status_t iree_net_tcp_carrier_activate(
    iree_net_carrier_t* base_carrier) {
  iree_net_tcp_carrier_t* carrier = iree_net_tcp_carrier_cast(base_carrier);
  iree_status_t status = iree_ok_status();
  iree_slim_mutex_lock(&carrier->mutex);
  if (!iree_net_carrier_try_transition_state(base_carrier,
                                             IREE_NET_CARRIER_STATE_CREATED,
                                             IREE_NET_CARRIER_STATE_ACTIVE)) {
    status = iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                              "TCP carrier is not in CREATED state");
  } else {
    carrier->receive_state = IREE_NET_TCP_RECEIVE_STATE_SUBMITTED;
    iree_atomic_fetch_add(&base_carrier->pending_operations, 1,
                          iree_memory_order_acq_rel);
    status = iree_net_tcp_submit_receive_locked(carrier);
  }
  if (!iree_status_is_ok(status)) {
    if (carrier->receive_state == IREE_NET_TCP_RECEIVE_STATE_SUBMITTED) {
      carrier->receive_state = IREE_NET_TCP_RECEIVE_STATE_RETIRED;
      bool was_final = iree_net_carrier_retire_pending_operation(base_carrier);
      IREE_ASSERT(was_final, "failed TCP activation left pending operations");
      iree_net_carrier_set_state(base_carrier, IREE_NET_CARRIER_STATE_CREATED);
    }
  }
  iree_slim_mutex_unlock(&carrier->mutex);
  return status;
}

static void iree_net_tcp_carrier_deactivate(
    iree_net_carrier_t* base_carrier,
    iree_net_carrier_deactivate_callback_fn_t callback, void* user_data) {
  iree_net_tcp_carrier_t* carrier = iree_net_tcp_carrier_cast(base_carrier);
  iree_net_tcp_send_slot_t* detached_reservations = NULL;
  iree_status_t cleanup_status = iree_ok_status();
  bool retire_paused_receive = false;
  bool shutdown_socket = false;
  bool valid_request = false;

  iree_slim_mutex_lock(&carrier->mutex);
  const iree_net_carrier_state_t state = iree_net_carrier_state(base_carrier);
  if (state == IREE_NET_CARRIER_STATE_CREATED ||
      state == IREE_NET_CARRIER_STATE_ACTIVE) {
    valid_request = true;
    shutdown_socket = state == IREE_NET_CARRIER_STATE_ACTIVE;
    iree_atomic_fetch_add(&base_carrier->pending_operations, 1,
                          iree_memory_order_acq_rel);
    carrier->deactivate_callback.fn = callback;
    carrier->deactivate_callback.user_data = user_data;
    carrier->flags |= IREE_NET_TCP_CARRIER_FLAG_SEND_SHUTDOWN_INITIATED |
                      IREE_NET_TCP_CARRIER_FLAG_SOCKET_WRITE_SHUTDOWN_ISSUED;
    iree_net_carrier_set_state(base_carrier, IREE_NET_CARRIER_STATE_DRAINING);
    detached_reservations = iree_net_tcp_detach_reservations_locked(carrier);
    if (carrier->receive_state == IREE_NET_TCP_RECEIVE_STATE_PAUSED) {
      carrier->receive_state = IREE_NET_TCP_RECEIVE_STATE_RETIRED;
      retire_paused_receive = true;
    } else {
      cleanup_status =
          iree_net_tcp_request_receive_cancellation_locked(carrier);
    }
    cleanup_status = iree_status_join(
        cleanup_status, iree_net_tcp_request_send_cancellation_locked(carrier));
  }
  iree_slim_mutex_unlock(&carrier->mutex);

  IREE_ASSERT(valid_request, "TCP carrier deactivated more than once");
  if (!valid_request) {
    return;
  }

  if (shutdown_socket) {
    cleanup_status = iree_status_join(
        cleanup_status, iree_async_socket_shutdown(
                            carrier->socket, IREE_ASYNC_SOCKET_SHUTDOWN_BOTH));
  }
  if (!iree_status_is_ok(cleanup_status)) {
    iree_net_carrier_report_terminal_error(base_carrier, cleanup_status);
  }
  if (detached_reservations) {
    iree_net_tcp_complete_detached_sends(carrier, detached_reservations,
                                         IREE_STATUS_CANCELLED);
  }
  if (retire_paused_receive) {
    iree_net_tcp_carrier_retire_pending_operation(carrier);
  }
  iree_net_tcp_carrier_retire_pending_operation(carrier);
}

static iree_net_carrier_send_budget_t iree_net_tcp_carrier_query_send_budget(
    iree_net_carrier_t* base_carrier) {
  iree_net_tcp_carrier_t* carrier = iree_net_tcp_carrier_cast(base_carrier);
  iree_net_carrier_send_budget_t budget = {0};
  iree_slim_mutex_lock(&carrier->mutex);
  if (iree_net_carrier_state(base_carrier) == IREE_NET_CARRIER_STATE_ACTIVE &&
      !iree_any_bit_set(carrier->flags,
                        IREE_NET_TCP_CARRIER_FLAG_SEND_SHUTDOWN_INITIATED) &&
      !iree_net_carrier_has_terminal_error(base_carrier)) {
    budget.bytes = IREE_HOST_SIZE_MAX;
    budget.slots = carrier->send_slot_count - carrier->send_slots_in_use;
  }
  iree_slim_mutex_unlock(&carrier->mutex);
  return budget;
}

static iree_status_t iree_net_tcp_check_send_admission_locked(
    iree_net_tcp_carrier_t* carrier, iree_net_tcp_send_slot_t** out_slot) {
  *out_slot = NULL;
  if (iree_net_carrier_state(&carrier->base) != IREE_NET_CARRIER_STATE_ACTIVE) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "TCP carrier is not active");
  }
  if (iree_any_bit_set(carrier->flags,
                       IREE_NET_TCP_CARRIER_FLAG_SEND_SHUTDOWN_INITIATED)) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "TCP carrier send direction is shut down");
  }
  IREE_RETURN_IF_ERROR(iree_net_carrier_clone_terminal_error(&carrier->base));
  iree_net_tcp_send_slot_t* slot =
      iree_net_tcp_find_free_send_slot_locked(carrier);
  if (!slot) {
    return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                            "TCP send operation slots are exhausted");
  }
  *out_slot = slot;
  return iree_ok_status();
}

static iree_status_t iree_net_tcp_carrier_send(
    iree_net_carrier_t* base_carrier, const iree_net_send_params_t* params) {
  iree_net_tcp_carrier_t* carrier = iree_net_tcp_carrier_cast(base_carrier);
  if (iree_any_bit_set(params->flags, ~IREE_NET_SEND_FLAG_ZERO_COPY)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "TCP send has unknown flags 0x%08X", params->flags);
  }

  iree_host_size_t total_length = 0;
  for (iree_host_size_t i = 0; i < params->data.count; ++i) {
    IREE_RETURN_IF_ERROR(
        iree_net_tcp_validate_send_span(params->data.values[i]));
    total_length += params->data.values[i].length;
  }

  iree_net_tcp_send_slot_t* slot = NULL;
  iree_net_tcp_send_slot_t* rejected_slot = NULL;
  iree_status_t status = iree_ok_status();
  iree_slim_mutex_lock(&carrier->mutex);
  status = iree_net_tcp_check_send_admission_locked(carrier, &slot);
  if (iree_status_is_ok(status)) {
    slot->completion_callback = params->completion_callback;
    slot->total_length = total_length;
    slot->bytes_transferred = 0;
    slot->span_count = 0;
    slot->first_span = 0;
    for (iree_host_size_t i = 0; i < params->data.count; ++i) {
      if (params->data.values[i].length == 0) {
        continue;
      }
      slot->spans[slot->span_count++] = params->data.values[i];
    }
    iree_async_span_list_retain_regions(
        iree_async_span_list_make(slot->spans, slot->span_count));
    slot->regions_retained = true;
    slot->state = IREE_NET_TCP_SEND_SLOT_STATE_QUEUED;
    ++carrier->send_slots_in_use;
    iree_atomic_fetch_add(&base_carrier->pending_operations, 1,
                          iree_memory_order_acq_rel);
    iree_net_tcp_enqueue_send_locked(carrier, slot);
    status = iree_net_tcp_start_send_dispatch_locked(carrier, &rejected_slot);
  }
  iree_slim_mutex_unlock(&carrier->mutex);

  if (rejected_slot) {
    iree_net_tcp_reject_detached_send(carrier, rejected_slot);
  }
  return status;
}

static iree_net_carrier_send_handle_t iree_net_tcp_allocate_reservation_handle(
    iree_net_tcp_carrier_t* carrier, iree_net_tcp_send_slot_t* slot) {
  ++slot->reservation_generation;
  if (slot->reservation_generation == 0) {
    ++slot->reservation_generation;
  }
  const uint32_t slot_index = (uint32_t)(slot - carrier->send_slots);
  return ((uint64_t)slot->reservation_generation << 32) |
         ((uint64_t)slot_index + 1u);
}

static iree_status_t iree_net_tcp_carrier_begin_send(
    iree_net_carrier_t* base_carrier, iree_host_size_t size, void** out_ptr,
    iree_net_carrier_send_handle_t* out_handle) {
  iree_net_tcp_carrier_t* carrier = iree_net_tcp_carrier_cast(base_carrier);
  void* reservation_buffer = NULL;
  IREE_RETURN_IF_ERROR(iree_allocator_malloc(base_carrier->host_allocator, size,
                                             &reservation_buffer));

  iree_net_tcp_send_slot_t* slot = NULL;
  iree_status_t status = iree_ok_status();
  iree_slim_mutex_lock(&carrier->mutex);
  status = iree_net_tcp_check_send_admission_locked(carrier, &slot);
  if (iree_status_is_ok(status)) {
    slot->reservation_buffer = reservation_buffer;
    slot->total_length = size;
    slot->bytes_transferred = 0;
    slot->span_count = 1;
    slot->first_span = 0;
    slot->spans[0] = iree_async_span_from_ptr(reservation_buffer, size);
    slot->reservation_handle =
        iree_net_tcp_allocate_reservation_handle(carrier, slot);
    slot->state = IREE_NET_TCP_SEND_SLOT_STATE_RESERVED;
    ++carrier->send_slots_in_use;
    iree_atomic_fetch_add(&base_carrier->pending_operations, 1,
                          iree_memory_order_acq_rel);
    *out_ptr = reservation_buffer;
    *out_handle = slot->reservation_handle;
  }
  iree_slim_mutex_unlock(&carrier->mutex);

  if (!iree_status_is_ok(status)) {
    iree_allocator_free(base_carrier->host_allocator, reservation_buffer);
  }
  return status;
}

static iree_net_tcp_send_slot_t* iree_net_tcp_lookup_reservation_locked(
    iree_net_tcp_carrier_t* carrier, iree_net_carrier_send_handle_t handle) {
  const uint32_t encoded_index = (uint32_t)handle;
  if (encoded_index == 0 || encoded_index > carrier->send_slot_count) {
    return NULL;
  }
  iree_net_tcp_send_slot_t* slot = &carrier->send_slots[encoded_index - 1u];
  return slot->state == IREE_NET_TCP_SEND_SLOT_STATE_RESERVED &&
                 slot->reservation_handle == handle
             ? slot
             : NULL;
}

static iree_status_t iree_net_tcp_carrier_commit_send(
    iree_net_carrier_t* base_carrier, iree_net_carrier_send_handle_t handle,
    iree_net_send_completion_callback_t callback) {
  iree_net_tcp_carrier_t* carrier = iree_net_tcp_carrier_cast(base_carrier);
  iree_net_tcp_send_slot_t* rejected_slot = NULL;
  iree_status_t status = iree_ok_status();
  iree_slim_mutex_lock(&carrier->mutex);
  iree_net_tcp_send_slot_t* slot =
      iree_net_tcp_lookup_reservation_locked(carrier, handle);
  if (!slot) {
    status = iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                              "TCP send reservation is no longer valid");
  } else {
    slot->reservation_handle = 0;
    slot->completion_callback = callback;
    slot->state = IREE_NET_TCP_SEND_SLOT_STATE_QUEUED;
    iree_net_tcp_enqueue_send_locked(carrier, slot);
    status = iree_net_tcp_start_send_dispatch_locked(carrier, &rejected_slot);
  }
  iree_slim_mutex_unlock(&carrier->mutex);

  if (rejected_slot) {
    iree_net_tcp_reject_detached_send(carrier, rejected_slot);
  }
  return status;
}

static void iree_net_tcp_carrier_abort_send(
    iree_net_carrier_t* base_carrier, iree_net_carrier_send_handle_t handle) {
  iree_net_tcp_carrier_t* carrier = iree_net_tcp_carrier_cast(base_carrier);
  void* reservation_buffer = NULL;
  bool issue_write_shutdown = false;
  bool valid_reservation = false;
  iree_slim_mutex_lock(&carrier->mutex);
  iree_net_tcp_send_slot_t* slot =
      iree_net_tcp_lookup_reservation_locked(carrier, handle);
  if (slot) {
    valid_reservation = true;
    reservation_buffer = slot->reservation_buffer;
    slot->reservation_buffer = NULL;
    iree_net_tcp_recycle_send_slot_locked(carrier, slot);
    issue_write_shutdown = iree_net_tcp_claim_write_shutdown_locked(carrier);
  }
  iree_slim_mutex_unlock(&carrier->mutex);

  IREE_ASSERT(valid_reservation, "TCP send reservation is no longer valid");
  if (!valid_reservation) {
    return;
  }
  iree_allocator_free(base_carrier->host_allocator, reservation_buffer);
  if (issue_write_shutdown) {
    iree_net_tcp_issue_deferred_write_shutdown(carrier);
  }
  iree_net_tcp_carrier_retire_pending_operation(carrier);
}

static iree_status_t iree_net_tcp_carrier_shutdown(
    iree_net_carrier_t* base_carrier) {
  iree_net_tcp_carrier_t* carrier = iree_net_tcp_carrier_cast(base_carrier);
  bool issue_write_shutdown = false;
  iree_status_t status = iree_ok_status();
  iree_slim_mutex_lock(&carrier->mutex);
  if (iree_net_carrier_state(base_carrier) != IREE_NET_CARRIER_STATE_ACTIVE) {
    status = iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                              "TCP carrier is not active");
  } else if (iree_any_bit_set(
                 carrier->flags,
                 IREE_NET_TCP_CARRIER_FLAG_SEND_SHUTDOWN_INITIATED)) {
    status =
        iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                         "TCP carrier send direction is already shut down");
  } else {
    carrier->flags |= IREE_NET_TCP_CARRIER_FLAG_SEND_SHUTDOWN_INITIATED;
    issue_write_shutdown = iree_net_tcp_claim_write_shutdown_locked(carrier);
  }
  iree_slim_mutex_unlock(&carrier->mutex);

  if (issue_write_shutdown) {
    status = iree_async_socket_shutdown(carrier->socket,
                                        IREE_ASYNC_SOCKET_SHUTDOWN_WRITE);
    if (!iree_status_is_ok(status)) {
      iree_status_t caller_status = iree_status_clone(status);
      iree_net_tcp_fail_without_operation(carrier, status);
      status = caller_status;
    }
  }
  return status;
}

static const iree_net_carrier_vtable_t iree_net_tcp_carrier_vtable = {
    .destroy = iree_net_tcp_carrier_destroy,
    .activate = iree_net_tcp_carrier_activate,
    .deactivate = iree_net_tcp_carrier_deactivate,
    .query_send_budget = iree_net_tcp_carrier_query_send_budget,
    .send = iree_net_tcp_carrier_send,
    .begin_send = iree_net_tcp_carrier_begin_send,
    .commit_send = iree_net_tcp_carrier_commit_send,
    .abort_send = iree_net_tcp_carrier_abort_send,
    .shutdown = iree_net_tcp_carrier_shutdown,
};

IREE_API_EXPORT iree_status_t iree_net_tcp_carrier_create(
    iree_async_proactor_t* proactor, iree_async_socket_t* socket,
    iree_async_buffer_pool_t* receive_pool,
    const iree_net_tcp_carrier_options_t* options,
    iree_allocator_t host_allocator, iree_net_carrier_t** out_carrier) {
  IREE_ASSERT_ARGUMENT(proactor);
  IREE_ASSERT_ARGUMENT(socket);
  IREE_ASSERT_ARGUMENT(receive_pool);
  IREE_ASSERT_ARGUMENT(out_carrier);
  *out_carrier = NULL;

  iree_net_tcp_carrier_options_t default_options =
      iree_net_tcp_carrier_options_default();
  if (!options) {
    options = &default_options;
  }
  if (options->max_send_operations == 0 ||
      options->max_send_operations == UINT32_MAX) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "TCP max send operations must be in [1, UINT32_MAX)");
  }
  if (socket->proactor != proactor) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "TCP socket belongs to a different proactor");
  }
  if (socket->type != IREE_ASYNC_SOCKET_TYPE_TCP &&
      socket->type != IREE_ASYNC_SOCKET_TYPE_TCP6) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "TCP carrier requires a TCP socket");
  }
  if (iree_async_socket_query_state(socket) !=
      IREE_ASYNC_SOCKET_STATE_CONNECTED) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "TCP carrier requires a connected socket");
  }

  iree_async_region_t* receive_region =
      iree_async_buffer_pool_region(receive_pool);
  if (receive_region->proactor != proactor) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "TCP receive pool was registered with a different proactor");
  }
  if (!iree_any_bit_set(receive_region->access_flags,
                        IREE_ASYNC_BUFFER_ACCESS_FLAG_WRITE)) {
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "TCP receive pool does not permit writes");
  }
  const iree_host_size_t receive_buffer_count =
      iree_async_buffer_pool_capacity(receive_pool);
  if (receive_buffer_count == 0 || receive_buffer_count > INT32_MAX) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "TCP receive pool capacity must be in [1, INT32_MAX]");
  }

  iree_host_size_t total_size = 0;
  iree_host_size_t send_slots_offset = 0;
  iree_host_size_t receive_contexts_offset = 0;
  IREE_RETURN_IF_ERROR(IREE_STRUCT_LAYOUT(
      sizeof(iree_net_tcp_carrier_t), &total_size,
      IREE_STRUCT_FIELD_ALIGNED(
          options->max_send_operations, iree_net_tcp_send_slot_t,
          iree_alignof(iree_net_tcp_send_slot_t), &send_slots_offset),
      IREE_STRUCT_FIELD_ALIGNED(
          receive_buffer_count, iree_net_tcp_receive_lease_context_t,
          iree_alignof(iree_net_tcp_receive_lease_context_t),
          &receive_contexts_offset)));

  iree_net_tcp_carrier_t* carrier = NULL;
  IREE_RETURN_IF_ERROR(
      iree_allocator_malloc(host_allocator, total_size, (void**)&carrier));
  memset(carrier, 0, total_size);
  iree_slim_mutex_initialize(&carrier->mutex);
  carrier->proactor = proactor;
  iree_async_proactor_retain(proactor);
  carrier->socket = socket;
  iree_async_socket_retain(socket);
  carrier->receive_pool = receive_pool;
  iree_async_buffer_pool_retain(receive_pool);
  carrier->send_slots =
      (iree_net_tcp_send_slot_t*)((uint8_t*)carrier + send_slots_offset);
  carrier->send_slot_count = options->max_send_operations;
  carrier->receive_lease_contexts =
      (iree_net_tcp_receive_lease_context_t*)((uint8_t*)carrier +
                                              receive_contexts_offset);
  carrier->receive_lease_context_count = receive_buffer_count;
  carrier->receive_state = IREE_NET_TCP_RECEIVE_STATE_RETIRED;
  iree_atomic_store(&carrier->returned_buffer_epoch, 0,
                    iree_memory_order_relaxed);
  iree_atomic_store(&carrier->retained_receive_lease_count, 0,
                    iree_memory_order_relaxed);
  for (iree_host_size_t i = 0; i < receive_buffer_count; ++i) {
    iree_atomic_store(&carrier->receive_lease_contexts[i].state,
                      IREE_NET_TCP_RECEIVE_LEASE_STATE_RELEASED,
                      iree_memory_order_relaxed);
  }

  iree_net_carrier_capabilities_t capabilities =
      IREE_NET_CARRIER_CAPABILITY_RELIABLE |
      IREE_NET_CARRIER_CAPABILITY_ORDERED |
      IREE_NET_CARRIER_CAPABILITY_ZERO_COPY_RX;
  const iree_async_proactor_capabilities_t proactor_capabilities =
      iree_async_proactor_query_capabilities(proactor);
  if (iree_any_bit_set(socket->flags, IREE_ASYNC_SOCKET_FLAG_ZERO_COPY) &&
      iree_any_bit_set(proactor_capabilities,
                       IREE_ASYNC_PROACTOR_CAPABILITY_ZERO_COPY_SEND)) {
    capabilities |= IREE_NET_CARRIER_CAPABILITY_ZERO_COPY_TX;
  }
  iree_net_carrier_initialize(&iree_net_tcp_carrier_vtable, capabilities,
                              IREE_ASYNC_SOCKET_SEND_MAX_BUFFERS,
                              host_allocator, &carrier->base);
  *out_carrier = &carrier->base;
  return iree_ok_status();
}
