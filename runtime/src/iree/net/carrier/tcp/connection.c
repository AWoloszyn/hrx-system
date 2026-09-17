// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/carrier/tcp/connection.h"

#include <string.h>

#include "iree/async/operations/net.h"
#include "iree/async/operations/scheduling.h"
#include "iree/base/threading/mutex.h"
#include "iree/net/endpoint_lifecycle.h"
#include "iree/net/framing_adapter.h"

// "IRN1" in little-endian byte order.
#define IREE_NET_TCP_FRAME_MAGIC UINT32_C(0x314E5249)

#define IREE_NET_TCP_FRAME_VERSION 1u
#define IREE_NET_TCP_FRAME_HEADER_SIZE 16u
#define IREE_NET_TCP_INDEX_NONE UINT32_MAX

typedef struct iree_net_tcp_connection_t iree_net_tcp_connection_t;
typedef struct iree_net_tcp_endpoint_t iree_net_tcp_endpoint_t;

typedef enum iree_net_tcp_connection_state_e {
  // Endpoint opens may be accepted.
  IREE_NET_TCP_CONNECTION_STATE_OPEN = 0,
  // Deactivation is waiting for accepted endpoint-ready callbacks.
  IREE_NET_TCP_CONNECTION_STATE_DRAINING_READY_CALLBACKS = 1,
  // Endpoint and shared carrier drains have started.
  IREE_NET_TCP_CONNECTION_STATE_DRAINING_ENDPOINTS = 2,
  // Every endpoint and shared carrier operation has drained.
  IREE_NET_TCP_CONNECTION_STATE_DEACTIVATED = 3,
} iree_net_tcp_connection_state_t;

typedef enum iree_net_tcp_endpoint_phase_e {
  // The endpoint has not been activated locally.
  IREE_NET_TCP_ENDPOINT_PHASE_CREATED = 0,
  // A proactor operation is draining pre-activation frames.
  IREE_NET_TCP_ENDPOINT_PHASE_ACTIVATING = 1,
  // Incoming frames may be delivered directly to the consumer.
  IREE_NET_TCP_ENDPOINT_PHASE_ACTIVE = 2,
  // Endpoint deactivation has begun.
  IREE_NET_TCP_ENDPOINT_PHASE_DRAINING = 3,
  // Endpoint deactivation has completed.
  IREE_NET_TCP_ENDPOINT_PHASE_DEACTIVATED = 4,
} iree_net_tcp_endpoint_phase_t;

typedef enum iree_net_tcp_send_state_phase_e {
  // The state record is available for admission.
  IREE_NET_TCP_SEND_STATE_PHASE_FREE = 0,
  // The shared carrier owns the submitted logical send.
  IREE_NET_TCP_SEND_STATE_PHASE_IN_FLIGHT = 1,
} iree_net_tcp_send_state_phase_t;

typedef struct iree_net_tcp_pending_frame_t {
  // Next frame index in an endpoint queue or the connection free list.
  uint32_t next;

  // Storage lease moved from the framing adapter.
  iree_async_buffer_lease_t lease;

  // Payload view kept valid by |lease|.
  iree_const_byte_span_t payload;
} iree_net_tcp_pending_frame_t;

typedef struct iree_net_tcp_send_state_t {
  // Endpoint operation retained through terminal completion.
  iree_net_tcp_endpoint_t* endpoint;

  // Index of this record in the connection send-state array.
  uint32_t index;

  // Next available record while this state is free.
  uint32_t next_free;

  // Current ownership phase.
  iree_net_tcp_send_state_phase_t phase;

  // Payload bytes represented by the framed send.
  iree_host_size_t payload_length;

  // User completion invoked after connection state is released.
  iree_net_send_completion_callback_t completion_callback;

} iree_net_tcp_send_state_t;

typedef struct iree_net_tcp_frame_prefix_t {
  // Payload byte count encoded into the wire header.
  uint32_t payload_length;

  // Endpoint ordinal encoded into the wire header.
  uint16_t endpoint_ordinal;

  // Message prefix generated after the wire header.
  iree_net_send_prefix_t message_prefix;
} iree_net_tcp_frame_prefix_t;

struct iree_net_tcp_endpoint_t {
  // Connection owning this ordinal endpoint.
  iree_net_tcp_connection_t* connection;

  // Ordinal encoded in the TCP frame header.
  uint16_t ordinal;

  // Current routing and activation phase.
  iree_net_tcp_endpoint_phase_t phase;

  // Message and terminal-error callbacks installed by the consumer.
  iree_net_message_endpoint_callbacks_t callbacks;

  // Coordinates endpoint operations with endpoint/connection drain.
  iree_net_endpoint_lifecycle_t lifecycle;

  // Preallocated operation delivering the endpoint-ready callback.
  iree_async_nop_operation_t ready_operation;

  // Callback owned while |ready_operation| is submitted.
  iree_net_endpoint_ready_callback_t ready_callback;

  // Preallocated operation draining frames queued before activation.
  iree_async_nop_operation_t activation_operation;

  // Endpoint-consumer callback awaiting explicit endpoint deactivation.
  struct {
    // Function invoked after the endpoint lifecycle drains.
    iree_net_message_endpoint_deactivate_fn_t fn;
    // Opaque value passed to |fn|.
    void* user_data;
  } deactivate_callback;

  // Head pending-frame index in wire order.
  uint32_t pending_head;

  // Tail pending-frame index in wire order.
  uint32_t pending_tail;

  // Number of frames queued before activation.
  uint32_t pending_count;
};

struct iree_net_tcp_connection_t {
  // Public connection base; must be first.
  iree_net_connection_t base;

  // Serializes connection, endpoint, send-state, and pending-frame ownership.
  iree_slim_mutex_t mutex;

  // Proactor dispatching all connection callbacks. Retained.
  iree_async_proactor_t* proactor;

  // Owned framing adapter and raw TCP carrier stack.
  iree_net_framing_adapter_t* framing_adapter;

  // Borrowed wire-frame endpoint exposed by |framing_adapter|.
  iree_net_message_endpoint_t wire_endpoint;

  // Current connection lifecycle phase.
  iree_net_tcp_connection_state_t state;

  // True after ownership has transferred to a public caller.
  bool published;

  // Number of monotonically claimed endpoint ordinals.
  uint32_t opened_endpoint_count;

  // Accepted endpoint-ready operations not yet retired.
  uint32_t pending_ready_count;

  // Maximum accepted total frame extent including its header.
  uint32_t max_frame_size;

  // Maximum frames retained per endpoint before activation.
  uint32_t max_pending_frames_per_endpoint;

  // First terminal shared connection status, owned until destruction.
  iree_status_t terminal_status;

  // Callback awaiting complete connection deactivation.
  iree_net_connection_deactivate_callback_t deactivate_callback;

  // Barrier joining endpoint and shared adapter drains.
  iree_net_endpoint_deactivation_barrier_t deactivation_barrier;

  // Preallocated endpoint slots indexed by wire ordinal.
  iree_net_tcp_endpoint_t* endpoints;

  // Number of connection-wide framing records.
  uint32_t send_state_count;

  // Number of framing records currently available.
  uint32_t free_send_state_count;

  // Head index of the send-state free list.
  uint32_t free_send_state_head;

  // Preallocated framing completion records.
  iree_net_tcp_send_state_t* send_states;

  // Number of preallocated pending-frame records.
  uint32_t pending_frame_count;

  // Number of pending-frame records currently available.
  uint32_t free_pending_frame_count;

  // Head index of the pending-frame free list.
  uint32_t free_pending_frame_head;

  // Preallocated metadata for frames received before endpoint activation.
  iree_net_tcp_pending_frame_t* pending_frames;
};

//===----------------------------------------------------------------------===//
// Wire format
//===----------------------------------------------------------------------===//

static void iree_net_tcp_encode_frame_header(uint8_t* header,
                                             uint32_t payload_length,
                                             uint16_t endpoint_ordinal) {
  iree_unaligned_store_le_u32(header + 0, IREE_NET_TCP_FRAME_MAGIC);
  iree_unaligned_store_le_u16(header + 4, IREE_NET_TCP_FRAME_VERSION);
  iree_unaligned_store_le_u16(header + 6, 0);
  iree_unaligned_store_le_u32(header + 8, payload_length);
  iree_unaligned_store_le_u16(header + 12, endpoint_ordinal);
  iree_unaligned_store_le_u16(header + 14, 0);
}

static iree_status_t iree_net_tcp_write_frame_prefix(void* user_data,
                                                     iree_byte_span_t target) {
  iree_net_tcp_frame_prefix_t* prefix = (iree_net_tcp_frame_prefix_t*)user_data;
  iree_net_tcp_encode_frame_header(target.data, prefix->payload_length,
                                   prefix->endpoint_ordinal);
  if (prefix->message_prefix.length == 0) {
    return iree_ok_status();
  }
  return prefix->message_prefix.write(
      prefix->message_prefix.user_data,
      iree_make_byte_span(target.data + IREE_NET_TCP_FRAME_HEADER_SIZE,
                          prefix->message_prefix.length));
}

static iree_status_t iree_net_tcp_calculate_frame_size(
    iree_net_tcp_connection_t* connection, iree_host_size_t payload_length,
    uint32_t* out_frame_size) {
  if (payload_length > UINT32_MAX - IREE_NET_TCP_FRAME_HEADER_SIZE) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "TCP message length %" PRIhsz
                            " exceeds the 32-bit wire extent",
                            payload_length);
  }
  const uint32_t frame_size =
      (uint32_t)payload_length + IREE_NET_TCP_FRAME_HEADER_SIZE;
  if (frame_size > connection->max_frame_size) {
    return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                            "TCP frame size %u exceeds configured limit %u",
                            frame_size, connection->max_frame_size);
  }
  *out_frame_size = frame_size;
  return iree_ok_status();
}

static iree_status_t iree_net_tcp_resolve_frame_size(
    void* user_data, iree_const_byte_span_t available,
    iree_host_size_t* out_frame_size) {
  iree_net_tcp_connection_t* connection = (iree_net_tcp_connection_t*)user_data;
  *out_frame_size = 0;
  if (available.data_length < IREE_NET_TCP_FRAME_HEADER_SIZE) {
    return iree_ok_status();
  }

  const uint32_t magic = iree_unaligned_load_le_u32(available.data + 0);
  if (magic != IREE_NET_TCP_FRAME_MAGIC) {
    return iree_make_status(IREE_STATUS_DATA_LOSS,
                            "invalid TCP frame magic 0x%08X", magic);
  }
  const uint16_t version = iree_unaligned_load_le_u16(available.data + 4);
  if (version != IREE_NET_TCP_FRAME_VERSION) {
    return iree_make_status(IREE_STATUS_DATA_LOSS,
                            "unsupported TCP frame version %u", version);
  }
  const uint16_t flags = iree_unaligned_load_le_u16(available.data + 6);
  if (flags != 0) {
    return iree_make_status(IREE_STATUS_DATA_LOSS,
                            "unsupported TCP frame flags 0x%04X", flags);
  }
  const uint16_t endpoint_ordinal =
      iree_unaligned_load_le_u16(available.data + 12);
  if (endpoint_ordinal >= connection->base.max_endpoint_count) {
    return iree_make_status(
        IREE_STATUS_DATA_LOSS, "TCP frame endpoint ordinal %u exceeds limit %u",
        endpoint_ordinal, connection->base.max_endpoint_count);
  }
  const uint16_t reserved = iree_unaligned_load_le_u16(available.data + 14);
  if (reserved != 0) {
    return iree_make_status(IREE_STATUS_DATA_LOSS,
                            "TCP frame reserved field is nonzero");
  }

  const uint32_t payload_length =
      iree_unaligned_load_le_u32(available.data + 8);
  if (payload_length == 0) {
    return iree_make_status(IREE_STATUS_DATA_LOSS,
                            "TCP frame payload must be nonempty");
  }
  iree_host_size_t frame_size = 0;
  if (!iree_host_size_checked_add(IREE_NET_TCP_FRAME_HEADER_SIZE,
                                  payload_length, &frame_size)) {
    return iree_make_status(IREE_STATUS_DATA_LOSS,
                            "TCP frame extent overflows host size");
  }
  *out_frame_size = frame_size;
  return iree_ok_status();
}

static iree_host_size_t iree_net_tcp_message_length(
    const iree_net_message_endpoint_send_params_t* params) {
  iree_host_size_t message_length = params->generated_prefix.length;
  for (iree_host_size_t i = 0; i < params->data.count; ++i) {
    message_length += params->data.values[i].length;
  }
  return message_length;
}

//===----------------------------------------------------------------------===//
// Bounded state pools
//===----------------------------------------------------------------------===//

static iree_net_tcp_send_state_t* iree_net_tcp_acquire_send_state_locked(
    iree_net_tcp_connection_t* connection, iree_net_tcp_endpoint_t* endpoint) {
  if (connection->free_send_state_head == IREE_NET_TCP_INDEX_NONE) {
    return NULL;
  }
  iree_net_tcp_send_state_t* send_state =
      &connection->send_states[connection->free_send_state_head];
  connection->free_send_state_head = send_state->next_free;
  --connection->free_send_state_count;
  send_state->next_free = IREE_NET_TCP_INDEX_NONE;
  send_state->phase = IREE_NET_TCP_SEND_STATE_PHASE_IN_FLIGHT;
  send_state->endpoint = endpoint;
  return send_state;
}

static void iree_net_tcp_release_send_state_locked(
    iree_net_tcp_connection_t* connection,
    iree_net_tcp_send_state_t* send_state) {
  send_state->endpoint = NULL;
  send_state->phase = IREE_NET_TCP_SEND_STATE_PHASE_FREE;
  send_state->payload_length = 0;
  send_state->completion_callback = (iree_net_send_completion_callback_t){0};
  send_state->next_free = connection->free_send_state_head;
  connection->free_send_state_head = send_state->index;
  ++connection->free_send_state_count;
}

static uint32_t iree_net_tcp_acquire_pending_frame_locked(
    iree_net_tcp_connection_t* connection) {
  const uint32_t frame_index = connection->free_pending_frame_head;
  if (frame_index == IREE_NET_TCP_INDEX_NONE) {
    return frame_index;
  }
  iree_net_tcp_pending_frame_t* pending_frame =
      &connection->pending_frames[frame_index];
  connection->free_pending_frame_head = pending_frame->next;
  --connection->free_pending_frame_count;
  pending_frame->next = IREE_NET_TCP_INDEX_NONE;
  return frame_index;
}

static void iree_net_tcp_release_pending_frame_locked(
    iree_net_tcp_connection_t* connection, uint32_t frame_index) {
  iree_net_tcp_pending_frame_t* pending_frame =
      &connection->pending_frames[frame_index];
  pending_frame->lease = (iree_async_buffer_lease_t){0};
  pending_frame->payload = iree_const_byte_span_empty();
  pending_frame->next = connection->free_pending_frame_head;
  connection->free_pending_frame_head = frame_index;
  ++connection->free_pending_frame_count;
}

static uint32_t iree_net_tcp_pop_pending_frame_locked(
    iree_net_tcp_connection_t* connection, iree_net_tcp_endpoint_t* endpoint) {
  const uint32_t frame_index = endpoint->pending_head;
  if (frame_index == IREE_NET_TCP_INDEX_NONE) {
    return frame_index;
  }
  iree_net_tcp_pending_frame_t* pending_frame =
      &connection->pending_frames[frame_index];
  endpoint->pending_head = pending_frame->next;
  if (endpoint->pending_head == IREE_NET_TCP_INDEX_NONE) {
    endpoint->pending_tail = IREE_NET_TCP_INDEX_NONE;
  }
  pending_frame->next = IREE_NET_TCP_INDEX_NONE;
  --endpoint->pending_count;
  return frame_index;
}

static iree_status_t iree_net_tcp_enqueue_pending_frame_locked(
    iree_net_tcp_endpoint_t* endpoint, iree_const_byte_span_t payload,
    iree_async_buffer_lease_t* lease) {
  iree_net_tcp_connection_t* connection = endpoint->connection;
  if (endpoint->pending_count >= connection->max_pending_frames_per_endpoint) {
    return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                            "TCP endpoint %u has %u frames pending activation",
                            endpoint->ordinal, endpoint->pending_count);
  }
  const uint32_t frame_index =
      iree_net_tcp_acquire_pending_frame_locked(connection);
  if (frame_index == IREE_NET_TCP_INDEX_NONE) {
    return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                            "TCP pending-frame records are exhausted");
  }

  iree_net_tcp_pending_frame_t* pending_frame =
      &connection->pending_frames[frame_index];
  pending_frame->lease = *lease;
  *lease = (iree_async_buffer_lease_t){0};
  pending_frame->payload = payload;
  if (endpoint->pending_tail == IREE_NET_TCP_INDEX_NONE) {
    endpoint->pending_head = frame_index;
  } else {
    connection->pending_frames[endpoint->pending_tail].next = frame_index;
  }
  endpoint->pending_tail = frame_index;
  ++endpoint->pending_count;
  return iree_ok_status();
}

static void iree_net_tcp_release_pending_frame(
    iree_net_tcp_connection_t* connection, uint32_t frame_index) {
  iree_net_tcp_pending_frame_t* pending_frame =
      &connection->pending_frames[frame_index];
  iree_async_buffer_lease_release(&pending_frame->lease);
  iree_slim_mutex_lock(&connection->mutex);
  iree_net_tcp_release_pending_frame_locked(connection, frame_index);
  iree_slim_mutex_unlock(&connection->mutex);
}

static void iree_net_tcp_clear_endpoint_pending_frames(
    iree_net_tcp_endpoint_t* endpoint) {
  iree_net_tcp_connection_t* connection = endpoint->connection;
  while (true) {
    iree_slim_mutex_lock(&connection->mutex);
    const uint32_t frame_index =
        iree_net_tcp_pop_pending_frame_locked(connection, endpoint);
    iree_slim_mutex_unlock(&connection->mutex);
    if (frame_index == IREE_NET_TCP_INDEX_NONE) {
      break;
    }
    iree_net_tcp_release_pending_frame(connection, frame_index);
  }
}

static void iree_net_tcp_clear_all_pending_frames(
    iree_net_tcp_connection_t* connection) {
  for (uint32_t i = 0; i < connection->base.max_endpoint_count; ++i) {
    iree_net_tcp_clear_endpoint_pending_frames(&connection->endpoints[i]);
  }
}

//===----------------------------------------------------------------------===//
// Shared receive and terminal error dispatch
//===----------------------------------------------------------------------===//

static void iree_net_tcp_connection_record_terminal_error(
    iree_net_tcp_connection_t* connection, iree_status_t status) {
  IREE_ASSERT(!iree_status_is_ok(status),
              "terminal connection error must be non-OK");
  if (iree_status_is_ok(status)) {
    return;
  }

  iree_net_connection_retain(&connection->base);
  bool is_first_error = false;
  iree_slim_mutex_lock(&connection->mutex);
  if (iree_status_is_ok(connection->terminal_status)) {
    connection->terminal_status = status;
    is_first_error = true;
  }
  iree_slim_mutex_unlock(&connection->mutex);
  if (!is_first_error) {
    iree_status_free(status);
    iree_net_connection_release(&connection->base);
    return;
  }

  iree_net_tcp_clear_all_pending_frames(connection);
  for (uint32_t i = 0; i < connection->base.max_endpoint_count; ++i) {
    iree_net_tcp_endpoint_t* endpoint = &connection->endpoints[i];
    iree_net_message_endpoint_error_fn_t on_error = NULL;
    void* callback_user_data = NULL;
    iree_slim_mutex_lock(&connection->mutex);
    if ((endpoint->phase == IREE_NET_TCP_ENDPOINT_PHASE_ACTIVATING ||
         endpoint->phase == IREE_NET_TCP_ENDPOINT_PHASE_ACTIVE) &&
        endpoint->callbacks.on_error &&
        iree_net_endpoint_lifecycle_try_begin_operation(&endpoint->lifecycle)) {
      on_error = endpoint->callbacks.on_error;
      callback_user_data = endpoint->callbacks.user_data;
    }
    iree_slim_mutex_unlock(&connection->mutex);
    if (on_error) {
      on_error(callback_user_data,
               iree_status_clone(connection->terminal_status));
      iree_net_endpoint_lifecycle_end_operation(&endpoint->lifecycle);
    }
  }
  iree_net_connection_release(&connection->base);
}

static iree_status_t iree_net_tcp_on_wire_frame(
    void* user_data, iree_const_byte_span_t frame,
    iree_async_buffer_lease_t* lease) {
  iree_net_tcp_connection_t* connection = (iree_net_tcp_connection_t*)user_data;
  const uint16_t endpoint_ordinal = iree_unaligned_load_le_u16(frame.data + 12);
  iree_net_tcp_endpoint_t* endpoint = &connection->endpoints[endpoint_ordinal];
  const iree_const_byte_span_t payload = iree_make_const_byte_span(
      frame.data + IREE_NET_TCP_FRAME_HEADER_SIZE,
      frame.data_length - IREE_NET_TCP_FRAME_HEADER_SIZE);

  iree_net_message_endpoint_message_fn_t on_message = NULL;
  void* callback_user_data = NULL;
  iree_slim_mutex_lock(&connection->mutex);
  // Leaving OPEN closes receive admission before endpoint queues are cleared.
  // Frames that lose this lock race remain owned by the framing adapter and
  // are intentionally discarded during connection drain.
  if (connection->state != IREE_NET_TCP_CONNECTION_STATE_OPEN) {
    iree_slim_mutex_unlock(&connection->mutex);
    return iree_ok_status();
  }
  iree_status_t status = iree_ok_status();
  if (!iree_status_is_ok(connection->terminal_status)) {
    status = iree_status_clone(connection->terminal_status);
  } else if (endpoint->phase == IREE_NET_TCP_ENDPOINT_PHASE_CREATED ||
             endpoint->phase == IREE_NET_TCP_ENDPOINT_PHASE_ACTIVATING) {
    status =
        iree_net_tcp_enqueue_pending_frame_locked(endpoint, payload, lease);
  } else if (endpoint->phase != IREE_NET_TCP_ENDPOINT_PHASE_ACTIVE ||
             !iree_net_endpoint_lifecycle_try_begin_operation(
                 &endpoint->lifecycle)) {
    status =
        iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                         "TCP endpoint %u is not active", endpoint_ordinal);
  } else {
    on_message = endpoint->callbacks.on_message;
    callback_user_data = endpoint->callbacks.user_data;
  }
  iree_slim_mutex_unlock(&connection->mutex);

  if (on_message) {
    status = on_message(callback_user_data, payload, lease);
    iree_net_endpoint_lifecycle_end_operation(&endpoint->lifecycle);
  }
  return status;
}

static void iree_net_tcp_on_wire_error(void* user_data, iree_status_t status) {
  iree_net_tcp_connection_record_terminal_error(
      (iree_net_tcp_connection_t*)user_data, status);
}

//===----------------------------------------------------------------------===//
// Endpoint implementation
//===----------------------------------------------------------------------===//

static void iree_net_tcp_endpoint_send_complete(
    void* user_data, iree_status_t status,
    iree_host_size_t wire_bytes_transferred) {
  iree_net_tcp_send_state_t* send_state = (iree_net_tcp_send_state_t*)user_data;
  iree_net_tcp_endpoint_t* endpoint = send_state->endpoint;
  iree_net_tcp_connection_t* connection = endpoint->connection;

  iree_slim_mutex_lock(&connection->mutex);
  IREE_ASSERT(send_state->phase == IREE_NET_TCP_SEND_STATE_PHASE_IN_FLIGHT,
              "TCP send completed from phase %d", (int)send_state->phase);
  const iree_host_size_t payload_length = send_state->payload_length;
  iree_net_send_completion_callback_t completion_callback =
      send_state->completion_callback;
  iree_net_tcp_release_send_state_locked(connection, send_state);
  iree_slim_mutex_unlock(&connection->mutex);

  IREE_ASSERT(!iree_status_is_ok(status) ||
                  wire_bytes_transferred ==
                      payload_length + IREE_NET_TCP_FRAME_HEADER_SIZE,
              "successful TCP framed send completed %" PRIhsz " of %" PRIhsz
              " wire bytes",
              wire_bytes_transferred,
              payload_length + IREE_NET_TCP_FRAME_HEADER_SIZE);
  iree_host_size_t payload_bytes_transferred = 0;
  if (wire_bytes_transferred > IREE_NET_TCP_FRAME_HEADER_SIZE) {
    payload_bytes_transferred =
        iree_min(payload_length,
                 wire_bytes_transferred - IREE_NET_TCP_FRAME_HEADER_SIZE);
  }
  completion_callback.fn(completion_callback.user_data, status,
                         payload_bytes_transferred);
  iree_net_endpoint_lifecycle_end_operation(&endpoint->lifecycle);
}

static void iree_net_tcp_endpoint_set_callbacks(
    void* self, iree_net_message_endpoint_callbacks_t callbacks) {
  iree_net_tcp_endpoint_t* endpoint = (iree_net_tcp_endpoint_t*)self;
  iree_net_tcp_connection_t* connection = endpoint->connection;
  iree_slim_mutex_lock(&connection->mutex);
  endpoint->callbacks = callbacks;
  iree_slim_mutex_unlock(&connection->mutex);
}

static void iree_net_tcp_endpoint_activation_complete(
    void* user_data, iree_async_operation_t* operation, iree_status_t status,
    iree_async_completion_flags_t flags) {
  (void)operation;
  IREE_ASSERT(!iree_any_bit_set(flags, IREE_ASYNC_COMPLETION_FLAG_MORE),
              "endpoint activation NOP produced a nonterminal completion");
  iree_net_tcp_endpoint_t* endpoint = (iree_net_tcp_endpoint_t*)user_data;
  iree_net_tcp_connection_t* connection = endpoint->connection;

  if (!iree_status_is_ok(status)) {
    iree_net_tcp_connection_record_terminal_error(connection, status);
  } else {
    iree_status_free(status);
    while (true) {
      iree_net_message_endpoint_message_fn_t on_message = NULL;
      void* callback_user_data = NULL;
      uint32_t frame_index = IREE_NET_TCP_INDEX_NONE;
      iree_slim_mutex_lock(&connection->mutex);
      if (endpoint->phase == IREE_NET_TCP_ENDPOINT_PHASE_ACTIVATING) {
        frame_index =
            iree_net_tcp_pop_pending_frame_locked(connection, endpoint);
        if (frame_index == IREE_NET_TCP_INDEX_NONE) {
          endpoint->phase = IREE_NET_TCP_ENDPOINT_PHASE_ACTIVE;
        } else {
          on_message = endpoint->callbacks.on_message;
          callback_user_data = endpoint->callbacks.user_data;
        }
      }
      iree_slim_mutex_unlock(&connection->mutex);
      if (frame_index == IREE_NET_TCP_INDEX_NONE) {
        break;
      }

      iree_net_tcp_pending_frame_t* pending_frame =
          &connection->pending_frames[frame_index];
      iree_status_t callback_status = on_message(
          callback_user_data, pending_frame->payload, &pending_frame->lease);
      iree_net_tcp_release_pending_frame(connection, frame_index);
      if (!iree_status_is_ok(callback_status)) {
        iree_net_tcp_connection_record_terminal_error(connection,
                                                      callback_status);
        break;
      }
    }
  }

  iree_slim_mutex_lock(&connection->mutex);
  if (endpoint->phase == IREE_NET_TCP_ENDPOINT_PHASE_ACTIVATING) {
    endpoint->phase = IREE_NET_TCP_ENDPOINT_PHASE_ACTIVE;
  }
  iree_slim_mutex_unlock(&connection->mutex);
  iree_net_endpoint_lifecycle_end_operation(&endpoint->lifecycle);
  iree_net_connection_release(&connection->base);
}

static iree_status_t iree_net_tcp_endpoint_activate(void* self) {
  iree_net_tcp_endpoint_t* endpoint = (iree_net_tcp_endpoint_t*)self;
  iree_net_tcp_connection_t* connection = endpoint->connection;
  bool release_connection = false;
  iree_slim_mutex_lock(&connection->mutex);
  iree_status_t status = iree_ok_status();
  if (endpoint->phase != IREE_NET_TCP_ENDPOINT_PHASE_CREATED) {
    status = iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                              "TCP endpoint %u cannot activate from phase %d",
                              endpoint->ordinal, (int)endpoint->phase);
  } else if (!endpoint->callbacks.on_message || !endpoint->callbacks.on_error) {
    status = iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                              "message and error callbacks are required");
  } else if (connection->state != IREE_NET_TCP_CONNECTION_STATE_OPEN) {
    status = iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                              "TCP connection is deactivating");
  } else if (!iree_status_is_ok(connection->terminal_status)) {
    status = iree_status_clone(connection->terminal_status);
  } else {
    status = iree_net_endpoint_lifecycle_activate(&endpoint->lifecycle);
  }

  if (iree_status_is_ok(status)) {
    const bool operation_accepted =
        iree_net_endpoint_lifecycle_try_begin_operation(&endpoint->lifecycle);
    IREE_ASSERT(operation_accepted,
                "newly activated TCP endpoint rejected its drain operation");
    endpoint->phase = IREE_NET_TCP_ENDPOINT_PHASE_ACTIVATING;
    iree_async_operation_initialize(
        &endpoint->activation_operation.base, IREE_ASYNC_OPERATION_TYPE_NOP,
        IREE_ASYNC_OPERATION_FLAG_NONE,
        iree_net_tcp_endpoint_activation_complete, endpoint);
    iree_net_connection_retain(&connection->base);
    status = iree_async_proactor_submit_one(
        connection->proactor, &endpoint->activation_operation.base);
    if (!iree_status_is_ok(status)) {
      endpoint->phase = IREE_NET_TCP_ENDPOINT_PHASE_CREATED;
      iree_net_endpoint_lifecycle_end_operation(&endpoint->lifecycle);
      iree_net_endpoint_lifecycle_rollback_activation(&endpoint->lifecycle);
      release_connection = true;
    }
  }
  iree_slim_mutex_unlock(&connection->mutex);
  if (release_connection) {
    iree_net_connection_release(&connection->base);
  }
  return status;
}

static void iree_net_tcp_endpoint_deactivation_complete(void* user_data) {
  iree_net_tcp_endpoint_t* endpoint = (iree_net_tcp_endpoint_t*)user_data;
  iree_net_tcp_connection_t* connection = endpoint->connection;
  iree_slim_mutex_lock(&connection->mutex);
  endpoint->phase = IREE_NET_TCP_ENDPOINT_PHASE_DEACTIVATED;
  iree_net_message_endpoint_deactivate_fn_t callback =
      endpoint->deactivate_callback.fn;
  void* callback_user_data = endpoint->deactivate_callback.user_data;
  endpoint->deactivate_callback.fn = NULL;
  endpoint->deactivate_callback.user_data = NULL;
  iree_slim_mutex_unlock(&connection->mutex);
  if (callback) {
    callback(callback_user_data);
  }
}

static iree_status_t iree_net_tcp_endpoint_deactivate(
    void* self, iree_net_message_endpoint_deactivate_fn_t callback,
    void* user_data) {
  iree_net_tcp_endpoint_t* endpoint = (iree_net_tcp_endpoint_t*)self;
  iree_net_tcp_connection_t* connection = endpoint->connection;
  iree_slim_mutex_lock(&connection->mutex);
  iree_net_endpoint_lifecycle_actions_t actions =
      IREE_NET_ENDPOINT_LIFECYCLE_ACTION_NONE;
  iree_status_t status = iree_net_endpoint_lifecycle_request_deactivation(
      &endpoint->lifecycle, iree_net_tcp_endpoint_deactivation_complete,
      endpoint, &actions);
  if (iree_status_is_ok(status)) {
    endpoint->phase = IREE_NET_TCP_ENDPOINT_PHASE_DRAINING;
    endpoint->deactivate_callback.fn = callback;
    endpoint->deactivate_callback.user_data = user_data;
  }
  iree_slim_mutex_unlock(&connection->mutex);
  if (!iree_status_is_ok(status)) {
    return status;
  }

  iree_net_tcp_clear_endpoint_pending_frames(endpoint);
  IREE_ASSERT(
      iree_any_bit_set(actions,
                       IREE_NET_ENDPOINT_LIFECYCLE_ACTION_BEGIN_DEACTIVATION),
      "accepted endpoint deactivation did not begin owner drain");
  iree_net_endpoint_lifecycle_complete_deactivation(&endpoint->lifecycle);
  return iree_ok_status();
}

static iree_status_t iree_net_tcp_endpoint_acquire_send_state(
    iree_net_tcp_endpoint_t* endpoint,
    iree_net_tcp_send_state_t** out_send_state) {
  *out_send_state = NULL;
  iree_net_tcp_connection_t* connection = endpoint->connection;
  iree_slim_mutex_lock(&connection->mutex);
  iree_status_t status = iree_ok_status();
  if (!iree_status_is_ok(connection->terminal_status)) {
    status = iree_status_clone(connection->terminal_status);
  } else if ((endpoint->phase != IREE_NET_TCP_ENDPOINT_PHASE_ACTIVATING &&
              endpoint->phase != IREE_NET_TCP_ENDPOINT_PHASE_ACTIVE) ||
             !iree_net_endpoint_lifecycle_try_begin_operation(
                 &endpoint->lifecycle)) {
    status =
        iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                         "TCP endpoint %u is not active", endpoint->ordinal);
  } else {
    *out_send_state =
        iree_net_tcp_acquire_send_state_locked(connection, endpoint);
    if (!*out_send_state) {
      iree_net_endpoint_lifecycle_end_operation(&endpoint->lifecycle);
      status = iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                                "TCP connection send slots are exhausted");
    }
  }
  iree_slim_mutex_unlock(&connection->mutex);
  return status;
}

static void iree_net_tcp_endpoint_reject_send_state(
    iree_net_tcp_send_state_t* send_state) {
  iree_net_tcp_endpoint_t* endpoint = send_state->endpoint;
  iree_net_tcp_connection_t* connection = endpoint->connection;
  iree_slim_mutex_lock(&connection->mutex);
  iree_net_tcp_release_send_state_locked(connection, send_state);
  iree_slim_mutex_unlock(&connection->mutex);
  iree_net_endpoint_lifecycle_end_operation(&endpoint->lifecycle);
}

static iree_status_t iree_net_tcp_endpoint_send(
    void* self, const iree_net_message_endpoint_send_params_t* params) {
  iree_net_tcp_endpoint_t* endpoint = (iree_net_tcp_endpoint_t*)self;
  iree_net_tcp_connection_t* connection = endpoint->connection;
  const iree_host_size_t payload_length = iree_net_tcp_message_length(params);
  uint32_t frame_size = 0;
  IREE_RETURN_IF_ERROR(iree_net_tcp_calculate_frame_size(
      connection, payload_length, &frame_size));

  iree_net_tcp_send_state_t* send_state = NULL;
  IREE_RETURN_IF_ERROR(
      iree_net_tcp_endpoint_acquire_send_state(endpoint, &send_state));
  send_state->payload_length = payload_length;
  send_state->completion_callback = params->completion_callback;

  iree_net_tcp_frame_prefix_t frame_prefix = {
      .payload_length = (uint32_t)payload_length,
      .endpoint_ordinal = endpoint->ordinal,
      .message_prefix = params->generated_prefix,
  };
  iree_net_message_endpoint_send_params_t wire_params = {
      .generated_prefix =
          {
              .length = IREE_NET_TCP_FRAME_HEADER_SIZE +
                        params->generated_prefix.length,
              .write = iree_net_tcp_write_frame_prefix,
              .user_data = &frame_prefix,
          },
      .data = params->data,
      .completion_callback =
          {
              .fn = iree_net_tcp_endpoint_send_complete,
              .user_data = send_state,
          },
  };
  iree_status_t status =
      iree_net_message_endpoint_send(connection->wire_endpoint, &wire_params);

  if (!iree_status_is_ok(status)) {
    iree_net_tcp_endpoint_reject_send_state(send_state);
  }
  return status;
}

static iree_net_carrier_send_budget_t iree_net_tcp_endpoint_query_send_budget(
    void* self) {
  iree_net_tcp_endpoint_t* endpoint = (iree_net_tcp_endpoint_t*)self;
  iree_net_tcp_connection_t* connection = endpoint->connection;
  uint32_t free_send_state_count = 0;
  bool active = false;
  iree_slim_mutex_lock(&connection->mutex);
  active = iree_status_is_ok(connection->terminal_status) &&
           (endpoint->phase == IREE_NET_TCP_ENDPOINT_PHASE_ACTIVATING ||
            endpoint->phase == IREE_NET_TCP_ENDPOINT_PHASE_ACTIVE);
  if (active) {
    free_send_state_count = connection->free_send_state_count;
  }
  iree_slim_mutex_unlock(&connection->mutex);
  if (!active) {
    return (iree_net_carrier_send_budget_t){0};
  }

  iree_net_carrier_send_budget_t budget =
      iree_net_message_endpoint_query_send_budget(connection->wire_endpoint);
  budget.slots = iree_min(budget.slots, free_send_state_count);
  const iree_host_size_t max_payload_size =
      connection->max_frame_size - IREE_NET_TCP_FRAME_HEADER_SIZE;
  if (budget.bytes != IREE_HOST_SIZE_MAX) {
    budget.bytes = budget.bytes > IREE_NET_TCP_FRAME_HEADER_SIZE
                       ? budget.bytes - IREE_NET_TCP_FRAME_HEADER_SIZE
                       : 0;
  }
  budget.bytes = iree_min(budget.bytes, max_payload_size);
  return budget;
}

static const iree_net_message_endpoint_vtable_t iree_net_tcp_endpoint_vtable = {
    .set_callbacks = iree_net_tcp_endpoint_set_callbacks,
    .activate = iree_net_tcp_endpoint_activate,
    .deactivate = iree_net_tcp_endpoint_deactivate,
    .send = iree_net_tcp_endpoint_send,
    .query_send_budget = iree_net_tcp_endpoint_query_send_budget,
};

//===----------------------------------------------------------------------===//
// Connection implementation
//===----------------------------------------------------------------------===//

static void iree_net_tcp_connection_deactivation_complete(void* user_data) {
  iree_net_tcp_connection_t* connection = (iree_net_tcp_connection_t*)user_data;
  iree_slim_mutex_lock(&connection->mutex);
  IREE_ASSERT(
      connection->state == IREE_NET_TCP_CONNECTION_STATE_DRAINING_ENDPOINTS,
      "TCP connection drain completed from state %d", (int)connection->state);
  connection->state = IREE_NET_TCP_CONNECTION_STATE_DEACTIVATED;
  iree_net_connection_deactivate_callback_t callback =
      connection->deactivate_callback;
  connection->deactivate_callback =
      (iree_net_connection_deactivate_callback_t){0};
  iree_slim_mutex_unlock(&connection->mutex);

  callback.fn(callback.user_data);
  iree_net_connection_release(&connection->base);
}

static void iree_net_tcp_connection_begin_endpoint_drain(
    iree_net_tcp_connection_t* connection) {
  for (uint32_t i = 0; i < connection->base.max_endpoint_count; ++i) {
    iree_net_tcp_endpoint_t* endpoint = &connection->endpoints[i];
    iree_slim_mutex_lock(&connection->mutex);
    iree_net_endpoint_lifecycle_actions_t actions =
        iree_net_endpoint_lifecycle_join_deactivation(&endpoint->lifecycle);
    if (iree_any_bit_set(
            actions, IREE_NET_ENDPOINT_LIFECYCLE_ACTION_BEGIN_DEACTIVATION)) {
      endpoint->phase = IREE_NET_TCP_ENDPOINT_PHASE_DRAINING;
    }
    iree_slim_mutex_unlock(&connection->mutex);

    iree_net_tcp_clear_endpoint_pending_frames(endpoint);
    if (iree_any_bit_set(
            actions, IREE_NET_ENDPOINT_LIFECYCLE_ACTION_BEGIN_DEACTIVATION)) {
      iree_net_endpoint_lifecycle_complete_deactivation(&endpoint->lifecycle);
    }
  }

  iree_net_framing_adapter_join_deactivation(connection->framing_adapter);
  iree_net_endpoint_deactivation_barrier_commit(
      &connection->deactivation_barrier,
      (iree_net_connection_deactivate_callback_t){
          .fn = iree_net_tcp_connection_deactivation_complete,
          .user_data = connection,
      });
}

static void iree_net_tcp_endpoint_ready_complete(
    void* user_data, iree_async_operation_t* operation, iree_status_t status,
    iree_async_completion_flags_t flags) {
  (void)operation;
  IREE_ASSERT(!iree_any_bit_set(flags, IREE_ASYNC_COMPLETION_FLAG_MORE),
              "endpoint-ready NOP produced a nonterminal completion");
  iree_net_tcp_endpoint_t* endpoint = (iree_net_tcp_endpoint_t*)user_data;
  iree_net_tcp_connection_t* connection = endpoint->connection;

  iree_net_message_endpoint_t message_endpoint = {0};
  iree_slim_mutex_lock(&connection->mutex);
  iree_net_endpoint_ready_callback_t ready_callback = endpoint->ready_callback;
  endpoint->ready_callback = (iree_net_endpoint_ready_callback_t){0};
  if (iree_status_is_ok(status) &&
      connection->state == IREE_NET_TCP_CONNECTION_STATE_OPEN &&
      iree_status_is_ok(connection->terminal_status)) {
    message_endpoint.self = endpoint;
    message_endpoint.vtable = &iree_net_tcp_endpoint_vtable;
  } else if (iree_status_is_ok(status) &&
             !iree_status_is_ok(connection->terminal_status)) {
    iree_status_free(status);
    status = iree_status_clone(connection->terminal_status);
  } else if (iree_status_is_ok(status)) {
    status = iree_make_status(IREE_STATUS_CANCELLED,
                              "TCP connection deactivated before endpoint "
                              "ready");
  }
  iree_slim_mutex_unlock(&connection->mutex);

  ready_callback.fn(ready_callback.user_data, status, message_endpoint);

  bool begin_endpoint_drain = false;
  iree_slim_mutex_lock(&connection->mutex);
  IREE_ASSERT(connection->pending_ready_count > 0,
              "TCP connection retired an unowned ready callback");
  --connection->pending_ready_count;
  if (connection->pending_ready_count == 0 &&
      connection->state ==
          IREE_NET_TCP_CONNECTION_STATE_DRAINING_READY_CALLBACKS) {
    connection->state = IREE_NET_TCP_CONNECTION_STATE_DRAINING_ENDPOINTS;
    begin_endpoint_drain = true;
  }
  iree_slim_mutex_unlock(&connection->mutex);

  if (begin_endpoint_drain) {
    iree_net_tcp_connection_begin_endpoint_drain(connection);
  }
  iree_net_connection_release(&connection->base);
}

static void iree_net_tcp_connection_destroy(
    iree_net_connection_t* base_connection) {
  iree_net_tcp_connection_t* connection =
      (iree_net_tcp_connection_t*)base_connection;
  IREE_ASSERT(
      !connection->published ||
          connection->state == IREE_NET_TCP_CONNECTION_STATE_DEACTIVATED,
      "published TCP connection released before deactivation");
  IREE_ASSERT(connection->pending_ready_count == 0,
              "TCP connection destroyed with pending ready callbacks");
  IREE_ASSERT(connection->free_send_state_count == connection->send_state_count,
              "TCP connection destroyed with owned send states");
  IREE_ASSERT(
      connection->free_pending_frame_count == connection->pending_frame_count,
      "TCP connection destroyed with pending frames");

  iree_allocator_t host_allocator = connection->base.host_allocator;
  for (uint32_t i = 0; i < connection->base.max_endpoint_count; ++i) {
    iree_net_endpoint_lifecycle_deinitialize(
        &connection->endpoints[i].lifecycle);
  }
  iree_status_free(connection->terminal_status);
  iree_net_framing_adapter_free(connection->framing_adapter);
  iree_async_proactor_release(connection->proactor);
  iree_slim_mutex_deinitialize(&connection->mutex);
  iree_allocator_free(host_allocator, connection);
}

static void iree_net_tcp_connection_deactivate(
    iree_net_connection_t* base_connection,
    iree_net_connection_deactivate_callback_t callback) {
  iree_net_tcp_connection_t* connection =
      (iree_net_tcp_connection_t*)base_connection;
  bool begin_endpoint_drain = false;
  bool valid_request = false;
  iree_slim_mutex_lock(&connection->mutex);
  if (connection->published &&
      connection->state == IREE_NET_TCP_CONNECTION_STATE_OPEN) {
    valid_request = true;
    connection->deactivate_callback = callback;
    iree_net_connection_retain(base_connection);
    if (connection->pending_ready_count == 0) {
      connection->state = IREE_NET_TCP_CONNECTION_STATE_DRAINING_ENDPOINTS;
      begin_endpoint_drain = true;
    } else {
      connection->state =
          IREE_NET_TCP_CONNECTION_STATE_DRAINING_READY_CALLBACKS;
    }
  }
  iree_slim_mutex_unlock(&connection->mutex);

  IREE_ASSERT(valid_request,
              "TCP connection deactivated from an invalid state");
  if (begin_endpoint_drain) {
    iree_net_tcp_connection_begin_endpoint_drain(connection);
  }
}

static iree_status_t iree_net_tcp_connection_open_endpoint(
    iree_net_connection_t* base_connection,
    iree_net_endpoint_ready_callback_t callback) {
  iree_net_tcp_connection_t* connection =
      (iree_net_tcp_connection_t*)base_connection;
  bool release_connection = false;
  iree_slim_mutex_lock(&connection->mutex);
  iree_status_t status = iree_ok_status();
  iree_net_tcp_endpoint_t* endpoint = NULL;
  if (!connection->published) {
    status = iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                              "TCP connection is not published");
  } else if (connection->state != IREE_NET_TCP_CONNECTION_STATE_OPEN) {
    status = iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                              "TCP connection is deactivating");
  } else if (!iree_status_is_ok(connection->terminal_status)) {
    status = iree_status_clone(connection->terminal_status);
  } else if (connection->opened_endpoint_count >=
             connection->base.max_endpoint_count) {
    status = iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                              "all %u TCP endpoint slots are claimed",
                              connection->base.max_endpoint_count);
  } else {
    endpoint = &connection->endpoints[connection->opened_endpoint_count];
    endpoint->ready_callback = callback;
    iree_async_operation_initialize(
        &endpoint->ready_operation.base, IREE_ASYNC_OPERATION_TYPE_NOP,
        IREE_ASYNC_OPERATION_FLAG_NONE, iree_net_tcp_endpoint_ready_complete,
        endpoint);
    ++connection->pending_ready_count;
    iree_net_connection_retain(base_connection);
    status = iree_async_proactor_submit_one(connection->proactor,
                                            &endpoint->ready_operation.base);
    if (iree_status_is_ok(status)) {
      ++connection->opened_endpoint_count;
    } else {
      --connection->pending_ready_count;
      endpoint->ready_callback = (iree_net_endpoint_ready_callback_t){0};
      release_connection = true;
    }
  }
  iree_slim_mutex_unlock(&connection->mutex);
  if (release_connection) {
    iree_net_connection_release(base_connection);
  }
  return status;
}

static iree_async_proactor_t* iree_net_tcp_connection_proactor(
    iree_net_connection_t* base_connection) {
  return ((iree_net_tcp_connection_t*)base_connection)->proactor;
}

static const iree_net_connection_vtable_t iree_net_tcp_connection_vtable = {
    .destroy = iree_net_tcp_connection_destroy,
    .deactivate = iree_net_tcp_connection_deactivate,
    .open_endpoint = iree_net_tcp_connection_open_endpoint,
    .proactor = iree_net_tcp_connection_proactor,
};

//===----------------------------------------------------------------------===//
// Public API
//===----------------------------------------------------------------------===//

typedef struct iree_net_tcp_connection_layout_t {
  // Number of connection-wide pending-frame records.
  iree_host_size_t pending_frame_count;
} iree_net_tcp_connection_layout_t;

static iree_status_t iree_net_tcp_connection_options_validate_impl(
    const iree_net_tcp_connection_options_t* options,
    iree_net_tcp_connection_layout_t* out_layout) {
  if (!options) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "TCP connection options are required");
  }
  if (options->max_endpoint_count == 0 ||
      options->max_endpoint_count > UINT16_MAX) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "TCP endpoint count must be in [1, %u]",
                            UINT16_MAX);
  }
  if (options->max_frame_size <= IREE_NET_TCP_FRAME_HEADER_SIZE) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "TCP frame limit must exceed the %u-byte header",
                            IREE_NET_TCP_FRAME_HEADER_SIZE);
  }
  if (options->max_pending_frames_per_endpoint == 0) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "TCP pending-frame limit per endpoint must be nonzero");
  }
  if (options->carrier_options.max_send_operations == 0) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "TCP send operation limit must be nonzero");
  }
  if (options->carrier_options.max_send_operations >= IREE_NET_TCP_INDEX_NONE) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "TCP send operation count exceeds index range");
  }

  iree_host_size_t pending_frame_count = 0;
  if (!iree_host_size_checked_mul(options->max_endpoint_count,
                                  options->max_pending_frames_per_endpoint,
                                  &pending_frame_count) ||
      pending_frame_count >= IREE_NET_TCP_INDEX_NONE) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "TCP pending-frame record count exceeds 32 bits");
  }

  if (out_layout) {
    *out_layout = (iree_net_tcp_connection_layout_t){
        .pending_frame_count = pending_frame_count,
    };
  }
  return iree_ok_status();
}

iree_status_t iree_net_tcp_connection_options_validate(
    const iree_net_tcp_connection_options_t* options) {
  return iree_net_tcp_connection_options_validate_impl(options,
                                                       /*out_layout=*/NULL);
}

iree_status_t iree_net_tcp_connection_create(
    iree_async_proactor_t* proactor, iree_async_socket_t* socket,
    iree_async_buffer_pool_t* receive_pool,
    const iree_net_tcp_connection_options_t* options,
    iree_allocator_t host_allocator, iree_net_connection_t** out_connection) {
  IREE_ASSERT_ARGUMENT(out_connection);
  *out_connection = NULL;
  if (!proactor || !socket || !receive_pool) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "proactor, connected socket, and receive pool are required");
  }
  iree_net_tcp_connection_options_t default_options =
      iree_net_tcp_connection_options_default();
  if (!options) {
    options = &default_options;
  }
  iree_net_tcp_connection_layout_t layout = {0};
  IREE_RETURN_IF_ERROR(
      iree_net_tcp_connection_options_validate_impl(options, &layout));

  iree_host_size_t endpoint_offset = 0;
  iree_host_size_t send_state_offset = 0;
  iree_host_size_t pending_frame_offset = 0;
  iree_host_size_t allocation_size = 0;
  IREE_RETURN_IF_ERROR(IREE_STRUCT_LAYOUT(
      sizeof(iree_net_tcp_connection_t), &allocation_size,
      IREE_STRUCT_FIELD_ALIGNED(
          options->max_endpoint_count, iree_net_tcp_endpoint_t,
          iree_alignof(iree_net_tcp_endpoint_t), &endpoint_offset),
      IREE_STRUCT_FIELD_ALIGNED(options->carrier_options.max_send_operations,
                                iree_net_tcp_send_state_t,
                                iree_alignof(iree_net_tcp_send_state_t),
                                &send_state_offset),
      IREE_STRUCT_ARRAY_FIELD_ALIGNED(
          layout.pending_frame_count, 1, iree_net_tcp_pending_frame_t,
          iree_alignof(iree_net_tcp_pending_frame_t), &pending_frame_offset)));

  iree_net_tcp_connection_t* connection = NULL;
  IREE_RETURN_IF_ERROR(iree_allocator_malloc(host_allocator, allocation_size,
                                             (void**)&connection));
  iree_net_connection_initialize(&iree_net_tcp_connection_vtable,
                                 host_allocator, options->max_endpoint_count,
                                 &connection->base);
  iree_slim_mutex_initialize(&connection->mutex);
  iree_net_endpoint_deactivation_barrier_initialize(
      &connection->deactivation_barrier);
  connection->proactor = proactor;
  iree_async_proactor_retain(proactor);
  connection->state = IREE_NET_TCP_CONNECTION_STATE_OPEN;
  connection->max_frame_size = options->max_frame_size;
  connection->max_pending_frames_per_endpoint =
      options->max_pending_frames_per_endpoint;
  connection->terminal_status = iree_ok_status();
  connection->endpoints =
      (iree_net_tcp_endpoint_t*)((uint8_t*)connection + endpoint_offset);
  connection->send_state_count = options->carrier_options.max_send_operations;
  connection->free_send_state_count = connection->send_state_count;
  connection->free_send_state_head = 0;
  connection->send_states =
      (iree_net_tcp_send_state_t*)((uint8_t*)connection + send_state_offset);
  connection->pending_frame_count = (uint32_t)layout.pending_frame_count;
  connection->free_pending_frame_count = connection->pending_frame_count;
  connection->free_pending_frame_head = 0;
  connection->pending_frames =
      (iree_net_tcp_pending_frame_t*)((uint8_t*)connection +
                                      pending_frame_offset);

  for (uint32_t i = 0; i < options->max_endpoint_count; ++i) {
    iree_net_tcp_endpoint_t* endpoint = &connection->endpoints[i];
    endpoint->connection = connection;
    endpoint->ordinal = (uint16_t)i;
    endpoint->phase = IREE_NET_TCP_ENDPOINT_PHASE_CREATED;
    endpoint->pending_head = IREE_NET_TCP_INDEX_NONE;
    endpoint->pending_tail = IREE_NET_TCP_INDEX_NONE;
    iree_net_endpoint_lifecycle_initialize(&connection->deactivation_barrier,
                                           &endpoint->lifecycle);
  }
  for (uint32_t i = 0; i < connection->send_state_count; ++i) {
    iree_net_tcp_send_state_t* send_state = &connection->send_states[i];
    send_state->index = i;
    send_state->next_free =
        i + 1 < connection->send_state_count ? i + 1 : IREE_NET_TCP_INDEX_NONE;
  }
  for (uint32_t i = 0; i < connection->pending_frame_count; ++i) {
    connection->pending_frames[i].next = i + 1 < connection->pending_frame_count
                                             ? i + 1
                                             : IREE_NET_TCP_INDEX_NONE;
  }

  iree_net_carrier_t* carrier = NULL;
  iree_status_t status = iree_net_tcp_carrier_create(
      proactor, socket, receive_pool, &options->carrier_options, host_allocator,
      &carrier);
  if (iree_status_is_ok(status)) {
    iree_net_frame_length_callback_t frame_length = {
        .fn = iree_net_tcp_resolve_frame_size,
        .user_data = connection,
        .max_header_size = IREE_NET_TCP_FRAME_HEADER_SIZE,
    };
    status = iree_net_framing_adapter_allocate(
        carrier, frame_length, options->max_frame_size,
        &connection->deactivation_barrier, host_allocator,
        &connection->framing_adapter);
    if (iree_status_is_ok(status)) {
      carrier = NULL;
    }
  }
  if (iree_status_is_ok(status)) {
    connection->wire_endpoint =
        iree_net_framing_adapter_as_endpoint(connection->framing_adapter);
    iree_net_message_endpoint_set_callbacks(
        connection->wire_endpoint, (iree_net_message_endpoint_callbacks_t){
                                       .on_message = iree_net_tcp_on_wire_frame,
                                       .on_error = iree_net_tcp_on_wire_error,
                                       .user_data = connection,
                                   });
    status = iree_net_message_endpoint_activate(connection->wire_endpoint);
  }

  if (iree_status_is_ok(status)) {
    connection->published = true;
    *out_connection = &connection->base;
  } else {
    iree_net_carrier_release(carrier);
    iree_net_connection_release(&connection->base);
  }
  return status;
}
