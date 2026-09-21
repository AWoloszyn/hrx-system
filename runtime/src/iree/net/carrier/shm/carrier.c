// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/carrier/shm/carrier.h"

#include "iree/async/operations/scheduling.h"
#include "iree/base/threading/mutex.h"
#include "iree/net/carrier/shm/storage.h"

// Bounds one poll callback, independently of message and resident buffer sizes.
#define IREE_NET_SHM_PROGRESS_BATCH_SIZE 64u

typedef enum iree_net_shm_send_phase_e {
  IREE_NET_SHM_SEND_PHASE_PREPARING = 0,
  IREE_NET_SHM_SEND_PHASE_READY,
  IREE_NET_SHM_SEND_PHASE_PUBLISHED,
} iree_net_shm_send_phase_t;

typedef struct iree_net_shm_send_t {
  // Admission phase, published under the carrier mutex after prefix writing.
  iree_net_shm_send_phase_t phase;
  // Source descriptors retained until this record completes. Entry zero is the
  // generated prefix, which may be empty.
  iree_async_span_t spans[IREE_NET_SHM_MAX_SEND_SPANS + 1];
  // Number of initialized span descriptors.
  iree_host_size_t span_count;
  // Current source span, advanced only by the polling owner.
  iree_host_size_t span_index;
  // Bytes already copied from the current span.
  iree_host_size_t span_offset;
  // Total source bytes, including the generated prefix.
  iree_host_size_t total_length;
  // Source bytes already copied to shared slots.
  iree_host_size_t copied_length;
  // Final shared byte position, valid once PUBLISHED with an OK status.
  uint64_t end_position;
  // Oversized generated-prefix allocation, or NULL for preallocated storage.
  void* owned_prefix;
  // Owned prefix failure transferred to the eventual completion callback.
  iree_status_t status;
  // Required callback returning source ownership on the polling thread.
  iree_net_send_completion_callback_t callback;
} iree_net_shm_send_t;

typedef enum iree_net_shm_admission_flag_bits_e {
  IREE_NET_SHM_ADMISSION_FLAG_DISPATCH_PENDING = 1u << 0,
  IREE_NET_SHM_ADMISSION_FLAG_SEND_SHUTDOWN = 1u << 1,
} iree_net_shm_admission_flag_bits_t;
typedef uint32_t iree_net_shm_admission_flags_t;

typedef enum iree_net_shm_progress_flag_bits_e {
  IREE_NET_SHM_PROGRESS_FLAG_WAIT_PENDING = 1u << 0,
  IREE_NET_SHM_PROGRESS_FLAG_WAIT_CANCELLED = 1u << 1,
  IREE_NET_SHM_PROGRESS_FLAG_SEND_EOF = 1u << 2,
  IREE_NET_SHM_PROGRESS_FLAG_RECEIVE_EOF = 1u << 3,
  IREE_NET_SHM_PROGRESS_FLAG_RECEIVER_CLOSED = 1u << 4,
} iree_net_shm_progress_flag_bits_t;
typedef uint32_t iree_net_shm_progress_flags_t;

typedef struct iree_net_shm_carrier_t {
  // Base carrier; must be first for upcasting.
  iree_net_carrier_t base;
  // Polling executor retained through carrier destruction.
  iree_async_proactor_t* proactor;
  // Detached mapping and wake resources retained by this carrier.
  iree_net_shm_storage_t* storage;
  // Shared local notification retained by all endpoint carriers.
  iree_async_notification_t* notification;
  // Outgoing shared direction; borrowed from storage.
  iree_net_shm_direction_t* outgoing;
  // Incoming shared direction and detached lease context; borrowed.
  iree_net_shm_storage_endpoint_t* receive;
  // Serializes concurrent admission, prefix publication, and lifecycle changes.
  iree_slim_mutex_t mutex;
  // Mutex-protected send admission and dispatch ownership.
  struct {
    // Preallocated circular record array.
    iree_net_shm_send_t* records;
    // Maximum simultaneous accepted sends.
    uint32_t capacity;
    // Oldest accepted record index.
    uint32_t head;
    // Accepted records, including concurrent prefix writers.
    uint32_t count;
    // Leading records fully published or locally failed.
    uint32_t published_count;
    // Coalesced dispatch and send-shutdown state.
    iree_net_shm_admission_flags_t flags;
    // Inline prefix slices indexed by record ordinal.
    uint8_t* prefixes;
    // Useful preallocated bytes per prefix slice.
    iree_host_size_t prefix_capacity;
    // Aligned byte stride between prefix slices.
    iree_host_size_t prefix_stride;
  } admission;
  // Poll-owner-only progress; caller threads never access these fields.
  struct {
    // Wait ownership, directional EOF, and local receiver closure.
    iree_net_shm_progress_flags_t flags;
    // Cumulative published bytes, wrapping modulo 2^64.
    uint64_t sent_position;
    // Cumulative consumed bytes, wrapping modulo 2^64.
    uint64_t received_position;
    // Reusable logical notification wait; native cancellation is owned by the
    // notification implementation, not by the carrier.
    iree_async_notification_wait_operation_t wait;
  } progress;
  // Reusable coalesced poll handoff. Callback entry returns its storage before
  // concurrent admission can initialize the next execution.
  iree_async_nop_operation_t dispatch;
  // Mutex-protected deactivation callback, consumed after accepted work drains.
  struct {
    // Required completion for an accepted deactivation.
    iree_net_carrier_deactivate_callback_fn_t fn;
    // Borrowed through that completion.
    void* user_data;
  } deactivate;
} iree_net_shm_carrier_t;

static iree_net_shm_carrier_t* iree_net_shm_carrier_cast(
    iree_net_carrier_t* carrier) {
  return (iree_net_shm_carrier_t*)carrier;
}

static bool iree_net_shm_carrier_is_running(iree_net_shm_carrier_t* carrier) {
  return iree_net_carrier_state(&carrier->base) ==
             IREE_NET_CARRIER_STATE_ACTIVE &&
         !iree_net_carrier_has_terminal_error(&carrier->base);
}

// Callback pointers are installed at construction. Reinitialization consumes
// only storage whose prior execution has already entered its terminal callback.
static void iree_net_shm_schedule_locked(iree_net_shm_carrier_t* carrier) {
  if (iree_any_bit_set(carrier->admission.flags,
                       IREE_NET_SHM_ADMISSION_FLAG_DISPATCH_PENDING)) {
    return;
  }
  carrier->admission.flags |= IREE_NET_SHM_ADMISSION_FLAG_DISPATCH_PENDING;
  iree_async_operation_t* operation = &carrier->dispatch.base;
  iree_async_operation_initialize(operation, IREE_ASYNC_OPERATION_TYPE_NOP, 0,
                                  operation->completion_fn, carrier);
  iree_atomic_fetch_add(&carrier->base.pending_operations, 1,
                        iree_memory_order_relaxed);
  IREE_CHECK_OK(iree_async_proactor_submit_one(carrier->proactor, operation));
}

void iree_net_shm_carrier_fail(iree_net_carrier_t* base_carrier,
                               iree_status_t status) {
  iree_net_shm_carrier_t* carrier = iree_net_shm_carrier_cast(base_carrier);
  iree_slim_mutex_lock(&carrier->mutex);
  iree_net_carrier_state_t state = iree_net_carrier_state(base_carrier);
  if (state == IREE_NET_CARRIER_STATE_CREATED &&
      !iree_net_carrier_has_terminal_error(base_carrier)) {
    // Serialize with activation even before handlers exist. Activation then
    // returns this failure transactionally, without admitting receive work.
    iree_atomic_store(&base_carrier->terminal_status, (intptr_t)status,
                      iree_memory_order_release);
    status = iree_ok_status();
  }
  iree_slim_mutex_unlock(&carrier->mutex);
  if (state == IREE_NET_CARRIER_STATE_CREATED ||
      state == IREE_NET_CARRIER_STATE_DEACTIVATED) {
    iree_status_free(status);
    return;
  }
  iree_net_carrier_report_terminal_error(base_carrier, status);
  iree_slim_mutex_lock(&carrier->mutex);
  iree_net_shm_schedule_locked(carrier);
  iree_slim_mutex_unlock(&carrier->mutex);
}

static iree_status_t iree_net_shm_validate_span(iree_async_span_t span) {
  if (!span.length) {
    return iree_ok_status();
  }
  if (!iree_async_span_is_cpu_accessible(span) ||
      (!span.region && !span.offset)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "SHM send requires CPU-readable storage");
  }
  if (span.region) {
    if (!iree_any_bit_set(span.region->access_flags,
                          IREE_ASYNC_BUFFER_ACCESS_FLAG_READ)) {
      return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                              "SHM send region does not permit reads");
    }
    if (span.offset > span.region->length ||
        span.length > span.region->length - span.offset) {
      return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                              "SHM send exceeds its registered region");
    }
  }
  return iree_ok_status();
}

// Source records and shared descriptors have independent lifetimes. Consumed
// receipts complete sources even when application-owned leases retain slots.
static void iree_net_shm_complete_sends(iree_net_shm_carrier_t* carrier) {
  uint64_t consumed = iree_atomic_load(carrier->outgoing->consumed_position,
                                       iree_memory_order_acquire);
  for (;;) {
    iree_slim_mutex_lock(&carrier->mutex);
    iree_net_shm_send_t* send =
        &carrier->admission.records[carrier->admission.head];
    bool stopped = !iree_net_shm_carrier_is_running(carrier);
    bool ready = carrier->admission.count &&
                 send->phase != IREE_NET_SHM_SEND_PHASE_PREPARING;
    if (ready && !stopped) {
      // Unacknowledged resident bytes plus one bounded progress batch are far
      // below 2^63, so modular subtraction also handles receipt wraparound.
      ready = send->phase == IREE_NET_SHM_SEND_PHASE_PUBLISHED &&
              (!iree_status_is_ok(send->status) ||
               consumed - send->end_position < (UINT64_C(1) << 63));
    }
    iree_slim_mutex_unlock(&carrier->mutex);
    if (!ready) {
      break;
    }

    iree_status_t status = send->status;
    send->status = iree_ok_status();
    if (iree_status_is_ok(status) && stopped) {
      status = iree_net_carrier_clone_terminal_error(&carrier->base);
      if (iree_status_is_ok(status)) {
        status = iree_make_status(IREE_STATUS_CANCELLED,
                                  "SHM send cancelled during deactivation");
      }
    }
    iree_net_send_completion_callback_t callback = send->callback;
    iree_host_size_t transferred = send->copied_length;
    iree_async_span_list_release_regions(
        iree_async_span_list_make(send->spans, send->span_count));
    iree_allocator_free(carrier->base.host_allocator, send->owned_prefix);
    send->owned_prefix = NULL;

    iree_slim_mutex_lock(&carrier->mutex);
    carrier->admission.head =
        (carrier->admission.head + 1) % carrier->admission.capacity;
    --carrier->admission.count;
    if (carrier->admission.published_count) {
      --carrier->admission.published_count;
    }
    iree_slim_mutex_unlock(&carrier->mutex);
    // Return capacity before the callback so it can submit its next send.
    callback.fn(callback.user_data, status, transferred);
    // The progress callback itself still owns an operation through this loop.
    iree_net_carrier_retire_pending_operation(&carrier->base);
  }
}

static void iree_net_shm_copy_chunk(iree_net_shm_send_t* send, uint8_t* target,
                                    iree_host_size_t length) {
  while (length) {
    iree_async_span_t span = send->spans[send->span_index];
    iree_host_size_t available = span.length - send->span_offset;
    iree_host_size_t count = iree_min(length, available);
    if (count) {
      memcpy(target, iree_async_span_ptr(span) + send->span_offset, count);
      target += count;
      length -= count;
      send->copied_length += count;
      send->span_offset += count;
    }
    if (send->span_offset == span.length) {
      ++send->span_index;
      send->span_offset = 0;
    }
  }
}

static void iree_net_shm_publish(iree_net_shm_carrier_t* carrier,
                                 bool* out_wake, bool* out_more) {
  iree_net_shm_direction_t* direction = carrier->outgoing;
  uint32_t count = 0;
  while (count < IREE_NET_SHM_PROGRESS_BATCH_SIZE &&
         iree_net_shm_carrier_is_running(carrier)) {
    iree_slim_mutex_lock(&carrier->mutex);
    iree_net_shm_send_t* send = NULL;
    if (carrier->admission.published_count < carrier->admission.count) {
      uint32_t index =
          (carrier->admission.head + carrier->admission.published_count) %
          carrier->admission.capacity;
      send = &carrier->admission.records[index];
      if (send->phase == IREE_NET_SHM_SEND_PHASE_PREPARING) {
        send = NULL;
      }
    }
    bool send_eof =
        !send &&
        carrier->admission.published_count == carrier->admission.count &&
        iree_any_bit_set(carrier->admission.flags,
                         IREE_NET_SHM_ADMISSION_FLAG_SEND_SHUTDOWN) &&
        !iree_any_bit_set(carrier->progress.flags,
                          IREE_NET_SHM_PROGRESS_FLAG_SEND_EOF);
    iree_slim_mutex_unlock(&carrier->mutex);
    if (!send) {
      if (send_eof) {
        iree_net_shm_descriptor_t descriptor = {
            .slot = IREE_ATOMIC_FREELIST_EMPTY,
            .end_position = carrier->progress.sent_position,
        };
        if (iree_mpsc_queue_write(&direction->descriptors, &descriptor,
                                  sizeof(descriptor))) {
          carrier->progress.flags |= IREE_NET_SHM_PROGRESS_FLAG_SEND_EOF;
          *out_wake = true;
        }
      }
      break;
    }

    if (iree_status_is_ok(send->status)) {
      uint16_t slot = 0;
      if (!iree_atomic_freelist_try_pop(direction->free_slots, direction->links,
                                        &slot)) {
        break;
      }
      iree_host_size_t length =
          iree_min(send->total_length - send->copied_length,
                   carrier->storage->layout.options.slot_capacity);
      iree_net_shm_copy_chunk(
          send,
          direction->payload + slot * carrier->storage->layout.slot_stride,
          length);
      carrier->progress.sent_position += length;
      iree_net_shm_descriptor_t descriptor = {
          .slot = slot,
          .length = (uint32_t)length,
          .end_position = carrier->progress.sent_position,
      };
      // Geometry provides room for every exclusively owned slot plus wrap
      // padding. This is a local producer invariant, not backpressure.
      bool written = iree_mpsc_queue_write(&direction->descriptors, &descriptor,
                                           sizeof(descriptor));
      IREE_ASSERT(written,
                  "SHM descriptor capacity must cover its payload slots");
      (void)written;
      *out_wake = true;
    }
    ++count;
    if (!iree_status_is_ok(send->status) ||
        send->copied_length == send->total_length) {
      send->end_position = carrier->progress.sent_position;
      iree_slim_mutex_lock(&carrier->mutex);
      send->phase = IREE_NET_SHM_SEND_PHASE_PUBLISHED;
      ++carrier->admission.published_count;
      iree_slim_mutex_unlock(&carrier->mutex);
    }
  }
  *out_more |= count == IREE_NET_SHM_PROGRESS_BATCH_SIZE;
}

static iree_status_t iree_net_shm_receive(iree_net_shm_carrier_t* carrier,
                                          bool* out_wake, bool* out_more) {
  if (iree_any_bit_set(carrier->progress.flags,
                       IREE_NET_SHM_PROGRESS_FLAG_RECEIVE_EOF)) {
    return iree_ok_status();
  }
  iree_net_shm_direction_t* direction = carrier->receive->incoming;
  iree_status_t status = iree_ok_status();
  uint32_t count = 0;
  while (count < IREE_NET_SHM_PROGRESS_BATCH_SIZE &&
         iree_status_is_ok(status) &&
         iree_net_shm_carrier_is_running(carrier)) {
    iree_host_size_t descriptor_length = 0;
    const void* data =
        iree_mpsc_queue_peek(&direction->descriptors, &descriptor_length);
    if (!data) {
      break;
    }
    if (descriptor_length != sizeof(iree_net_shm_descriptor_t)) {
      status = iree_make_status(IREE_STATUS_DATA_LOSS,
                                "SHM payload descriptor has an invalid size");
      break;
    }
    iree_net_shm_descriptor_t descriptor;
    memcpy(&descriptor, data, sizeof(descriptor));
    if (descriptor.end_position !=
            carrier->progress.received_position + descriptor.length ||
        (descriptor.length == 0
             ? descriptor.slot != IREE_ATOMIC_FREELIST_EMPTY
             : (descriptor.slot >=
                    carrier->storage->layout.options.slot_count ||
                descriptor.length >
                    carrier->storage->layout.options.slot_capacity))) {
      status =
          iree_make_status(IREE_STATUS_DATA_LOSS,
                           "SHM payload descriptor disagrees with its stream");
      break;
    }
    // The descriptor is copied before consuming its queue entry. A payload slot
    // is independently owned through the callback or a moved native lease.
    iree_mpsc_queue_consume(&direction->descriptors);
    ++count;
    *out_wake = true;
    if (!descriptor.length) {
      carrier->progress.flags |= IREE_NET_SHM_PROGRESS_FLAG_RECEIVE_EOF;
      status = carrier->base.handlers.on_receive(
          carrier->base.handlers.user_data, iree_async_span_empty(), NULL);
      break;
    }
    iree_async_buffer_lease_t lease = {0};
    bool leased = iree_net_shm_storage_try_lease(
        carrier->receive, (uint16_t)descriptor.slot, descriptor.length, &lease);
    iree_async_span_t span = iree_async_span_from_ptr(
        direction->payload +
            descriptor.slot * carrier->storage->layout.slot_stride,
        descriptor.length);
    status = carrier->base.handlers.on_receive(carrier->base.handlers.user_data,
                                               span, leased ? &lease : NULL);
    if (lease.release.fn) {
      iree_net_shm_storage_recycle_lease(&lease);
    } else if (!leased) {
      iree_atomic_freelist_push(direction->free_slots, direction->links,
                                (uint16_t)descriptor.slot);
    }
    carrier->progress.received_position = descriptor.end_position;
    iree_atomic_store(direction->consumed_position, descriptor.end_position,
                      iree_memory_order_release);
  }
  *out_more |= count == IREE_NET_SHM_PROGRESS_BATCH_SIZE;
  return status;
}

static void iree_net_shm_stop_receive(iree_net_shm_carrier_t* carrier,
                                      bool* out_wake) {
  if (!iree_any_bit_set(carrier->progress.flags,
                        IREE_NET_SHM_PROGRESS_FLAG_RECEIVER_CLOSED)) {
    carrier->progress.flags |= IREE_NET_SHM_PROGRESS_FLAG_RECEIVER_CLOSED;
    iree_atomic_store(carrier->receive->incoming->receiver_closed, 1,
                      iree_memory_order_release);
    *out_wake = true;
  }
  if (iree_any_bit_set(carrier->progress.flags,
                       IREE_NET_SHM_PROGRESS_FLAG_WAIT_PENDING) &&
      !iree_any_bit_set(carrier->progress.flags,
                        IREE_NET_SHM_PROGRESS_FLAG_WAIT_CANCELLED)) {
    carrier->progress.flags |= IREE_NET_SHM_PROGRESS_FLAG_WAIT_CANCELLED;
    // Logical notification cancellation only marks poll-owner work. Its
    // terminal callback joins any native monitor cancellation internally.
    IREE_CHECK_OK(iree_async_proactor_cancel(carrier->proactor,
                                             &carrier->progress.wait.base));
  }
}

static iree_status_t iree_net_shm_wait(iree_net_shm_carrier_t* carrier,
                                       uint32_t token) {
  if (iree_any_bit_set(carrier->progress.flags,
                       IREE_NET_SHM_PROGRESS_FLAG_WAIT_PENDING)) {
    return iree_ok_status();
  }
  iree_async_notification_wait_operation_t* wait = &carrier->progress.wait;
  iree_async_operation_initialize(
      &wait->base, IREE_ASYNC_OPERATION_TYPE_NOTIFICATION_WAIT,
      IREE_ASYNC_OPERATION_FLAG_CANCELLATION_IS_SUCCESS,
      wait->base.completion_fn, carrier);
  wait->notification = carrier->notification;
  wait->wait_flags = IREE_ASYNC_NOTIFICATION_WAIT_FLAG_USE_WAIT_TOKEN;
  wait->wait_token = token;
  IREE_RETURN_IF_ERROR(
      iree_async_proactor_submit_one(carrier->proactor, &wait->base));
  carrier->progress.flags |= IREE_NET_SHM_PROGRESS_FLAG_WAIT_PENDING;
  iree_atomic_fetch_add(&carrier->base.pending_operations, 1,
                        iree_memory_order_relaxed);
  return iree_ok_status();
}

static void iree_net_shm_progress(iree_net_shm_carrier_t* carrier) {
  uint32_t token = iree_async_notification_begin_observe(carrier->notification);
  bool wake = false;
  bool more = false;
  if (iree_net_shm_carrier_is_running(carrier) &&
      iree_atomic_load(carrier->outgoing->receiver_closed,
                       iree_memory_order_acquire)) {
    iree_net_shm_carrier_fail(
        &carrier->base,
        iree_make_status(IREE_STATUS_UNAVAILABLE, "SHM peer endpoint retired"));
  }
  iree_net_shm_complete_sends(carrier);
  if (iree_net_shm_carrier_is_running(carrier)) {
    iree_net_shm_publish(carrier, &wake, &more);
    iree_status_t status = iree_net_shm_receive(carrier, &wake, &more);
    if (!iree_status_is_ok(status)) {
      iree_net_shm_carrier_fail(&carrier->base, status);
    }
    iree_net_shm_complete_sends(carrier);
  }
  if (!iree_net_shm_carrier_is_running(carrier)) {
    iree_net_shm_stop_receive(carrier, &wake);
  }
  if (wake) {
    iree_net_shm_storage_signal(carrier->storage, carrier->storage->side ^ 1u);
  }
  if (iree_net_shm_carrier_is_running(carrier)) {
    if (more) {
      iree_slim_mutex_lock(&carrier->mutex);
      iree_net_shm_schedule_locked(carrier);
      iree_slim_mutex_unlock(&carrier->mutex);
    } else {
      iree_status_t status = iree_net_shm_wait(carrier, token);
      if (!iree_status_is_ok(status)) {
        iree_net_shm_carrier_fail(&carrier->base, status);
      }
    }
  }
  iree_async_notification_end_observe(carrier->notification);
}

static void iree_net_shm_retire_operation(iree_net_shm_carrier_t* carrier) {
  if (!iree_net_carrier_retire_pending_operation(&carrier->base)) {
    return;
  }
  iree_net_carrier_deactivate_callback_fn_t callback = NULL;
  void* user_data = NULL;
  iree_slim_mutex_lock(&carrier->mutex);
  if (iree_net_carrier_state(&carrier->base) ==
          IREE_NET_CARRIER_STATE_DRAINING &&
      !iree_net_carrier_pending_operation_count(&carrier->base)) {
    iree_net_carrier_set_state(&carrier->base,
                               IREE_NET_CARRIER_STATE_DEACTIVATED);
    callback = carrier->deactivate.fn;
    user_data = carrier->deactivate.user_data;
  }
  iree_slim_mutex_unlock(&carrier->mutex);
  if (callback) {
    callback(user_data);
    // Activation holds one reference through the final drain callback.
    iree_net_carrier_release(&carrier->base);
  }
}

static void iree_net_shm_operation_completed(
    void* user_data, iree_async_operation_t* operation, iree_status_t status,
    iree_async_completion_flags_t flags) {
  (void)flags;
  iree_net_shm_carrier_t* carrier = user_data;
  if (operation == &carrier->progress.wait.base) {
    carrier->progress.flags &= ~(IREE_NET_SHM_PROGRESS_FLAG_WAIT_PENDING |
                                 IREE_NET_SHM_PROGRESS_FLAG_WAIT_CANCELLED);
  } else {
    iree_slim_mutex_lock(&carrier->mutex);
    carrier->admission.flags &= ~IREE_NET_SHM_ADMISSION_FLAG_DISPATCH_PENDING;
    iree_slim_mutex_unlock(&carrier->mutex);
  }
  if (!iree_status_is_ok(status)) {
    iree_net_shm_carrier_fail(&carrier->base, status);
  }
  iree_net_shm_progress(carrier);
  iree_net_shm_retire_operation(carrier);
}

static void iree_net_shm_carrier_destroy(iree_net_carrier_t* base_carrier) {
  iree_net_shm_carrier_t* carrier = iree_net_shm_carrier_cast(base_carrier);
  IREE_ASSERT(iree_net_carrier_state(base_carrier) ==
                  IREE_NET_CARRIER_STATE_CREATED ||
              iree_net_carrier_state(base_carrier) ==
                  IREE_NET_CARRIER_STATE_DEACTIVATED);
  iree_allocator_t host_allocator = base_carrier->host_allocator;
  iree_async_notification_release(carrier->notification);
  iree_net_shm_storage_release(carrier->storage);
  iree_async_proactor_release(carrier->proactor);
  iree_slim_mutex_deinitialize(&carrier->mutex);
  iree_net_carrier_deinitialize(base_carrier);
  iree_allocator_free(host_allocator, carrier);
}

static iree_status_t iree_net_shm_carrier_activate(
    iree_net_carrier_t* base_carrier) {
  iree_net_shm_carrier_t* carrier = iree_net_shm_carrier_cast(base_carrier);
  IREE_RETURN_IF_ERROR(iree_net_shm_storage_clone_failure(carrier->storage));
  iree_slim_mutex_lock(&carrier->mutex);
  if (iree_net_carrier_state(base_carrier) != IREE_NET_CARRIER_STATE_CREATED) {
    iree_slim_mutex_unlock(&carrier->mutex);
    return iree_make_status(
        IREE_STATUS_FAILED_PRECONDITION,
        "SHM carrier has already been activated or retired");
  }
  iree_status_t status = iree_net_carrier_clone_terminal_error(base_carrier);
  if (!iree_status_is_ok(status)) {
    iree_slim_mutex_unlock(&carrier->mutex);
    return status;
  }
  iree_net_carrier_retain(base_carrier);
  iree_net_carrier_set_state(base_carrier, IREE_NET_CARRIER_STATE_ACTIVE);
  iree_net_shm_schedule_locked(carrier);
  iree_slim_mutex_unlock(&carrier->mutex);
  return iree_ok_status();
}

static void iree_net_shm_carrier_deactivate(
    iree_net_carrier_t* base_carrier,
    iree_net_carrier_deactivate_callback_fn_t callback, void* user_data) {
  iree_net_shm_carrier_t* carrier = iree_net_shm_carrier_cast(base_carrier);
  iree_slim_mutex_lock(&carrier->mutex);
  bool inactive =
      iree_net_carrier_state(base_carrier) == IREE_NET_CARRIER_STATE_CREATED;
  carrier->deactivate.fn = callback;
  carrier->deactivate.user_data = user_data;
  iree_net_carrier_set_state(base_carrier,
                             inactive ? IREE_NET_CARRIER_STATE_DEACTIVATED
                                      : IREE_NET_CARRIER_STATE_DRAINING);
  if (!inactive) {
    iree_net_shm_schedule_locked(carrier);
  }
  iree_slim_mutex_unlock(&carrier->mutex);
  if (inactive) {
    iree_atomic_store(carrier->receive->incoming->receiver_closed, 1,
                      iree_memory_order_release);
    iree_net_shm_storage_signal(carrier->storage, carrier->storage->side ^ 1u);
    callback(user_data);
  }
}

static iree_net_carrier_send_budget_t iree_net_shm_carrier_query_send_budget(
    iree_net_carrier_t* base_carrier) {
  iree_net_shm_carrier_t* carrier = iree_net_shm_carrier_cast(base_carrier);
  iree_net_carrier_send_budget_t budget = {0};
  iree_slim_mutex_lock(&carrier->mutex);
  if (iree_net_shm_carrier_is_running(carrier) &&
      !iree_any_bit_set(carrier->admission.flags,
                        IREE_NET_SHM_ADMISSION_FLAG_SEND_SHUTDOWN)) {
    budget.slots = carrier->admission.capacity - carrier->admission.count;
    budget.bytes = budget.slots ? IREE_HOST_SIZE_MAX : 0;
  }
  iree_slim_mutex_unlock(&carrier->mutex);
  return budget;
}

static iree_status_t iree_net_shm_carrier_send(
    iree_net_carrier_t* base_carrier, const iree_net_send_params_t* params) {
  iree_net_shm_carrier_t* carrier = iree_net_shm_carrier_cast(base_carrier);
  iree_host_size_t total_length = params->generated_prefix.length;
  for (iree_host_size_t i = 0; i < params->data.count; ++i) {
    IREE_RETURN_IF_ERROR(iree_net_shm_validate_span(params->data.values[i]));
    total_length += params->data.values[i].length;
  }
  iree_slim_mutex_lock(&carrier->mutex);
  if (!iree_net_shm_carrier_is_running(carrier) ||
      iree_any_bit_set(carrier->admission.flags,
                       IREE_NET_SHM_ADMISSION_FLAG_SEND_SHUTDOWN)) {
    iree_slim_mutex_unlock(&carrier->mutex);
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "SHM carrier is not accepting sends");
  }
  if (carrier->admission.count == carrier->admission.capacity) {
    iree_slim_mutex_unlock(&carrier->mutex);
    return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                            "SHM send admission capacity is exhausted");
  }
  uint32_t index = (carrier->admission.head + carrier->admission.count) %
                   carrier->admission.capacity;
  iree_net_shm_send_t* send = &carrier->admission.records[index];
  uint8_t* prefix =
      carrier->admission.prefixes + index * carrier->admission.prefix_stride;
  iree_status_t status = iree_ok_status();
  if (params->generated_prefix.length > carrier->admission.prefix_capacity) {
    status = iree_allocator_malloc_uninitialized(
        base_carrier->host_allocator, params->generated_prefix.length,
        &send->owned_prefix);
    prefix = send->owned_prefix;
  }
  if (!iree_status_is_ok(status)) {
    iree_slim_mutex_unlock(&carrier->mutex);
    return status;
  }
  send->phase = IREE_NET_SHM_SEND_PHASE_PREPARING;
  send->span_count = params->data.count + 1;
  send->span_index = 0;
  send->span_offset = 0;
  send->total_length = total_length;
  send->copied_length = 0;
  send->status = iree_ok_status();
  send->callback = params->completion_callback;
  send->spans[0] =
      iree_async_span_from_ptr(prefix, params->generated_prefix.length);
  if (params->data.count) {
    memcpy(send->spans + 1, params->data.values,
           params->data.count * sizeof(iree_async_span_t));
  }
  iree_async_span_list_retain_regions(
      iree_async_span_list_make(send->spans, send->span_count));
  ++carrier->admission.count;
  iree_atomic_fetch_add(&base_carrier->pending_operations, 1,
                        iree_memory_order_relaxed);
  iree_slim_mutex_unlock(&carrier->mutex);

  if (params->generated_prefix.write) {
    status = params->generated_prefix.write(
        params->generated_prefix.user_data,
        iree_make_byte_span(prefix, params->generated_prefix.length));
  }
  iree_slim_mutex_lock(&carrier->mutex);
  send->status = status;
  send->phase = IREE_NET_SHM_SEND_PHASE_READY;
  iree_net_shm_schedule_locked(carrier);
  iree_slim_mutex_unlock(&carrier->mutex);
  return iree_ok_status();
}

static iree_status_t iree_net_shm_carrier_shutdown(
    iree_net_carrier_t* base_carrier) {
  iree_net_shm_carrier_t* carrier = iree_net_shm_carrier_cast(base_carrier);
  IREE_RETURN_IF_ERROR(iree_net_carrier_clone_terminal_error(base_carrier));
  iree_slim_mutex_lock(&carrier->mutex);
  if (iree_net_carrier_state(base_carrier) != IREE_NET_CARRIER_STATE_ACTIVE) {
    iree_slim_mutex_unlock(&carrier->mutex);
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "SHM shutdown requires an active carrier");
  }
  carrier->admission.flags |= IREE_NET_SHM_ADMISSION_FLAG_SEND_SHUTDOWN;
  iree_net_shm_schedule_locked(carrier);
  iree_slim_mutex_unlock(&carrier->mutex);
  return iree_ok_status();
}

static const iree_net_carrier_vtable_t iree_net_shm_carrier_vtable = {
    .destroy = iree_net_shm_carrier_destroy,
    .activate = iree_net_shm_carrier_activate,
    .deactivate = iree_net_shm_carrier_deactivate,
    .query_send_budget = iree_net_shm_carrier_query_send_budget,
    .send = iree_net_shm_carrier_send,
    .shutdown = iree_net_shm_carrier_shutdown,
};

iree_status_t iree_net_shm_carrier_create(
    iree_async_proactor_t* proactor, iree_net_shm_storage_t* storage,
    iree_async_notification_t* notification, uint32_t endpoint_ordinal,
    const iree_net_shm_carrier_options_t* options,
    iree_allocator_t host_allocator, iree_net_carrier_t** out_carrier) {
  *out_carrier = NULL;
  if (!options->max_send_operations ||
      options->max_send_operations > INT32_MAX - 4 ||
      endpoint_ordinal >= storage->layout.options.endpoint_count) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "SHM carrier requires a valid endpoint and send capacity");
  }
  iree_host_size_t prefix_stride = 0;
  iree_host_size_t prefix_bytes = 0;
  if (!iree_host_size_checked_align(options->generated_prefix_capacity,
                                    IREE_NET_SEND_PREFIX_ALIGNMENT,
                                    &prefix_stride) ||
      !iree_host_size_checked_mul(options->max_send_operations, prefix_stride,
                                  &prefix_bytes)) {
    return iree_make_status(
        IREE_STATUS_OUT_OF_RANGE,
        "SHM generated prefix storage exceeds the host size");
  }
  iree_host_size_t allocation_size = 0;
  iree_host_size_t sends_offset = 0;
  iree_host_size_t prefixes_offset = 0;
  IREE_RETURN_IF_ERROR(IREE_STRUCT_LAYOUT(
      sizeof(iree_net_shm_carrier_t), &allocation_size,
      IREE_STRUCT_FIELD(options->max_send_operations, iree_net_shm_send_t,
                        &sends_offset),
      IREE_STRUCT_FIELD_ALIGNED(prefix_bytes, uint8_t,
                                IREE_NET_SEND_PREFIX_ALIGNMENT,
                                &prefixes_offset)));
  iree_net_shm_carrier_t* carrier = NULL;
  IREE_RETURN_IF_ERROR(
      iree_allocator_malloc(host_allocator, allocation_size, (void**)&carrier));
  iree_net_carrier_initialize(&iree_net_shm_carrier_vtable,
                              IREE_NET_CARRIER_CAPABILITY_RELIABLE |
                                  IREE_NET_CARRIER_CAPABILITY_ORDERED |
                                  IREE_NET_CARRIER_CAPABILITY_ZERO_COPY_RX,
                              IREE_NET_SHM_MAX_SEND_SPANS, host_allocator,
                              &carrier->base);
  carrier->proactor = proactor;
  iree_async_proactor_retain(proactor);
  carrier->storage = storage;
  iree_net_shm_storage_retain(storage);
  carrier->notification = notification;
  iree_async_notification_retain(notification);
  carrier->outgoing =
      &storage
           ->directions[(iree_host_size_t)endpoint_ordinal * 2 + storage->side];
  carrier->receive = &storage->endpoints[endpoint_ordinal];
  iree_slim_mutex_initialize(&carrier->mutex);
  carrier->admission.records =
      (iree_net_shm_send_t*)((uint8_t*)carrier + sends_offset);
  carrier->admission.capacity = options->max_send_operations;
  carrier->admission.prefixes = (uint8_t*)carrier + prefixes_offset;
  carrier->admission.prefix_capacity = options->generated_prefix_capacity;
  carrier->admission.prefix_stride = prefix_stride;
  carrier->dispatch.base.completion_fn = iree_net_shm_operation_completed;
  carrier->progress.wait.base.completion_fn = iree_net_shm_operation_completed;
  *out_carrier = &carrier->base;
  return iree_ok_status();
}
