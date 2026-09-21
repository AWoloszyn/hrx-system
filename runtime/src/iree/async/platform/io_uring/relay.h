// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Internal header for io_uring relay implementation.
//
// io_uring-specific state enum and implementation functions. The relay struct
// is defined in the shared iree/async/relay.h with a platform union.
// Primitive sources use multishot POLL_ADD. Notification sources subscribe to
// the notification's shared native monitor. Both execute sinks during poll().

#ifndef IREE_ASYNC_PLATFORM_IO_URING_RELAY_H_
#define IREE_ASYNC_PLATFORM_IO_URING_RELAY_H_

#include "iree/async/relay.h"
#include "iree/base/api.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

typedef struct iree_async_proactor_io_uring_t iree_async_proactor_io_uring_t;

//===----------------------------------------------------------------------===//
// Relay state
//===----------------------------------------------------------------------===//

// Internal state for relay lifecycle management.
typedef enum iree_async_io_uring_relay_state_e {
  // Registration is complete but the poll owner has not submitted source
  // monitoring yet.
  IREE_ASYNC_IO_URING_RELAY_STATE_ARM_PENDING = 0,

  // Relay is active and monitoring source.
  IREE_ASYNC_IO_URING_RELAY_STATE_ACTIVE = 1,

  // Terminal unregistration was requested but an SQE was not available for
  // the cancellation operation. The sink will not fire in this state.
  IREE_ASYNC_IO_URING_RELAY_STATE_UNREGISTRATION_PENDING = 2,

  // The terminal cancellation operation was submitted and the relay is
  // waiting for its source and cancellation CQEs. The sink will not fire.
  IREE_ASYNC_IO_URING_RELAY_STATE_UNREGISTRATION_SUBMITTED = 3,

  // Relay faulted while its multishot source was still active, but an SQE was
  // not available to cancel it. The sink will not fire in this state.
  IREE_ASYNC_IO_URING_RELAY_STATE_FAULT_CANCELLATION_PENDING = 4,

  // Relay faulted and cancellation of its active multishot source was
  // submitted. The sink will not fire in this state.
  IREE_ASYNC_IO_URING_RELAY_STATE_FAULT_CANCELLATION_SUBMITTED = 5,

  // Relay faulted and has no remaining kernel references. The caller-visible
  // handle remains valid until terminal unregistration.
  IREE_ASYNC_IO_URING_RELAY_STATE_FAULTED = 6,

  // A persistent poll source terminated without a relay fault. The
  // caller-visible handle remains valid until terminal unregistration.
  IREE_ASYNC_IO_URING_RELAY_STATE_TERMINAL = 7,

  // Notification-source fault classified but not yet detached and reported.
  IREE_ASYNC_IO_URING_RELAY_STATE_FAULT_PENDING = 8,
} iree_async_io_uring_relay_state_t;

// Primitive sources retain independent target and cancellation obligations.
enum iree_async_io_uring_relay_operation_flag_bits_e {
  IREE_ASYNC_IO_URING_RELAY_OPERATION_POLL = 1u << 0,
  IREE_ASYNC_IO_URING_RELAY_OPERATION_CANCEL = 1u << 1,
};
typedef uint32_t iree_async_io_uring_relay_operation_flags_t;

//===----------------------------------------------------------------------===//
// Implementation functions
//===----------------------------------------------------------------------===//

iree_status_t iree_async_io_uring_register_relay(
    iree_async_proactor_io_uring_t* proactor, iree_async_relay_source_t source,
    iree_async_relay_sink_t sink, iree_async_relay_flags_t flags,
    iree_async_relay_error_callback_t error_callback,
    iree_async_relay_t** out_relay);

void iree_async_io_uring_unregister_relay(
    iree_async_proactor_io_uring_t* proactor, iree_async_relay_t* relay,
    iree_async_relay_unregistered_callback_t callback);

// Begins unregistration of remaining relays without replacing owned callbacks.
// Native receipts must still be driven before the ring is closed.
void iree_async_io_uring_unregister_all_relays(
    iree_async_proactor_io_uring_t* proactor);

// Called from CQE processing when a relay's source fires.
// Executes the sink action and handles re-arming or cleanup.
void iree_async_io_uring_handle_relay_cqe(
    iree_async_proactor_io_uring_t* proactor, iree_async_relay_t* relay,
    int32_t result, uint32_t cqe_flags);

// Retires a primitive relay's cancellation key independently of its poll.
iree_status_t iree_async_io_uring_relay_complete_cancel(
    iree_async_proactor_io_uring_t* proactor, iree_async_relay_t* relay,
    int32_t result);

// Queues primitive-source arms and terminal unregistrations deferred to the
// poll owner. Returns true when SQ pressure left work pending.
bool iree_async_io_uring_retry_pending_relays(
    iree_async_proactor_io_uring_t* proactor);

// Executes the sink without calling user code. Returns zero or a native errno.
int iree_async_io_uring_relay_fire_sink(iree_async_relay_t* relay);

// Reports a previously classified fault after source bookkeeping is settled.
// Takes ownership of |status|. A persistent handle remains caller-owned.
void iree_async_io_uring_relay_report_fault(iree_async_relay_t* relay,
                                            iree_status_t status);

// Completes terminal cleanup after source-local/native ownership has retired.
void iree_async_io_uring_relay_cleanup(iree_async_proactor_io_uring_t* proactor,
                                       iree_async_relay_t* relay);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_ASYNC_PLATFORM_IO_URING_RELAY_H_
