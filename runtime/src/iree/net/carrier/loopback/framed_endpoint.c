// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/carrier/loopback/framed_endpoint.h"

#include <string.h>

#include "iree/base/alignment.h"
#include "iree/base/threading/mutex.h"
#include "iree/net/framing_adapter.h"

// "IRN1" in little-endian byte order.
#define IREE_NET_LOOPBACK_FRAME_MAGIC UINT32_C(0x314E5249)

#define IREE_NET_LOOPBACK_FRAME_HEADER_SIZE 8u

// Bounds stack-resident scatter/gather descriptors on the zero-copy path.
// Larger lists use the direct contiguous path instead of failing.
#define IREE_NET_LOOPBACK_INLINE_WIRE_SPAN_COUNT 64u

#define IREE_NET_LOOPBACK_SEND_STATE_NONE UINT32_MAX

typedef enum iree_net_loopback_framed_endpoint_state_e {
  IREE_NET_LOOPBACK_FRAMED_ENDPOINT_STATE_CREATED = 0,
  IREE_NET_LOOPBACK_FRAMED_ENDPOINT_STATE_ACTIVE = 1,
  IREE_NET_LOOPBACK_FRAMED_ENDPOINT_STATE_DRAINING = 2,
  IREE_NET_LOOPBACK_FRAMED_ENDPOINT_STATE_DEACTIVATED = 3,
} iree_net_loopback_framed_endpoint_state_t;

typedef enum iree_net_loopback_send_state_phase_e {
  IREE_NET_LOOPBACK_SEND_STATE_PHASE_FREE = 0,
  IREE_NET_LOOPBACK_SEND_STATE_PHASE_RESERVED = 1,
  IREE_NET_LOOPBACK_SEND_STATE_PHASE_IN_FLIGHT = 2,
} iree_net_loopback_send_state_phase_t;

typedef struct iree_net_loopback_send_state_t {
  // Endpoint owning this state record.
  struct iree_net_loopback_framed_endpoint_t* endpoint;

  // Index of this record in its endpoint's trailing state array.
  uint32_t index;

  // Next free record index while this state is available.
  uint32_t next_free;

  // Nonzero generation encoded into direct reservation handles.
  uint32_t generation;

  // Current ownership phase for this state record.
  iree_net_loopback_send_state_phase_t phase;

  // Underlying carrier reservation handle for direct sends.
  iree_net_carrier_send_handle_t carrier_handle;

  // Payload bytes represented by this framed send.
  iree_host_size_t payload_length;

  // User completion invoked after this state is returned to the pool.
  iree_net_send_completion_callback_t completion_callback;

  // Stable wire header retained through asynchronous completion.
  uint8_t header[IREE_NET_LOOPBACK_FRAME_HEADER_SIZE];
} iree_net_loopback_send_state_t;

struct iree_net_loopback_framed_endpoint_t {
  // Serializes lifecycle transitions and send-state allocation.
  iree_slim_mutex_t mutex;

  // Current endpoint lifecycle state.
  iree_net_loopback_framed_endpoint_state_t state;

  // Message callbacks installed by the endpoint consumer.
  iree_net_message_endpoint_callbacks_t callbacks;

  // Callback awaiting endpoint-initiated deactivation completion.
  struct {
    // Function invoked when the wire endpoint has drained.
    iree_net_message_endpoint_deactivate_fn_t fn;
    // Opaque value passed to |fn|.
    void* user_data;
  } deactivate_callback;

  // Owned wire-frame adapter and carrier stack.
  iree_net_framing_adapter_t* framing_adapter;

  // Borrowed endpoint view into |framing_adapter|.
  iree_net_message_endpoint_t wire_endpoint;

  // Host allocator owning this endpoint and its framing adapter.
  iree_allocator_t host_allocator;

  // Maximum carrier spans accepted by one wire send.
  iree_host_size_t max_wire_spans;

  // Number of records in the trailing send-state array.
  uint32_t send_state_count;

  // Number of records currently available for admission.
  uint32_t free_send_state_count;

  // Head index of the intrusive send-state free list.
  uint32_t free_send_state_head;

  // Fixed send framing and reservation records.
  iree_net_loopback_send_state_t send_states[];
};

static void iree_net_loopback_encode_frame_header(uint8_t* header,
                                                  uint32_t frame_length) {
  iree_unaligned_store_le_u32(header + 0, IREE_NET_LOOPBACK_FRAME_MAGIC);
  iree_unaligned_store_le_u32(header + 4, frame_length);
}

static iree_status_t iree_net_loopback_calculate_frame_length(
    iree_host_size_t payload_length, uint32_t* out_frame_length) {
  if (payload_length == 0) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "loopback messages must be nonempty");
  }
  if (payload_length >
      (iree_host_size_t)UINT32_MAX - IREE_NET_LOOPBACK_FRAME_HEADER_SIZE) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "loopback message length %" PRIhsz
                            " exceeds the 32-bit wire extent",
                            payload_length);
  }
  *out_frame_length =
      (uint32_t)(payload_length + IREE_NET_LOOPBACK_FRAME_HEADER_SIZE);
  return iree_ok_status();
}

static iree_status_t iree_net_loopback_resolve_frame_length(
    void* user_data, iree_const_byte_span_t available,
    iree_host_size_t* out_frame_size) {
  (void)user_data;
  *out_frame_size = 0;
  if (available.data_length < IREE_NET_LOOPBACK_FRAME_HEADER_SIZE) {
    return iree_ok_status();
  }
  const uint32_t magic = iree_unaligned_load_le_u32(available.data + 0);
  if (magic != IREE_NET_LOOPBACK_FRAME_MAGIC) {
    return iree_make_status(IREE_STATUS_DATA_LOSS,
                            "invalid loopback frame magic 0x%08X", magic);
  }
  const uint32_t frame_length = iree_unaligned_load_le_u32(available.data + 4);
  if (frame_length <= IREE_NET_LOOPBACK_FRAME_HEADER_SIZE) {
    return iree_make_status(IREE_STATUS_DATA_LOSS,
                            "invalid loopback frame length %u", frame_length);
  }
  *out_frame_size = frame_length;
  return iree_ok_status();
}

static iree_net_loopback_send_state_t*
iree_net_loopback_acquire_send_state_locked(
    iree_net_loopback_framed_endpoint_t* endpoint) {
  if (endpoint->free_send_state_head == IREE_NET_LOOPBACK_SEND_STATE_NONE) {
    return NULL;
  }
  iree_net_loopback_send_state_t* send_state =
      &endpoint->send_states[endpoint->free_send_state_head];
  endpoint->free_send_state_head = send_state->next_free;
  --endpoint->free_send_state_count;
  send_state->next_free = IREE_NET_LOOPBACK_SEND_STATE_NONE;
  ++send_state->generation;
  if (send_state->generation == 0) {
    ++send_state->generation;
  }
  send_state->phase = IREE_NET_LOOPBACK_SEND_STATE_PHASE_RESERVED;
  return send_state;
}

static void iree_net_loopback_release_send_state_locked(
    iree_net_loopback_framed_endpoint_t* endpoint,
    iree_net_loopback_send_state_t* send_state) {
  send_state->phase = IREE_NET_LOOPBACK_SEND_STATE_PHASE_FREE;
  send_state->carrier_handle = 0;
  send_state->payload_length = 0;
  send_state->completion_callback = (iree_net_send_completion_callback_t){0};
  send_state->next_free = endpoint->free_send_state_head;
  endpoint->free_send_state_head = send_state->index;
  ++endpoint->free_send_state_count;
}

static iree_net_loopback_send_state_t*
iree_net_loopback_lookup_reservation_locked(
    iree_net_loopback_framed_endpoint_t* endpoint,
    iree_net_carrier_send_handle_t handle) {
  const uint32_t index = (uint32_t)handle;
  const uint32_t generation = (uint32_t)(handle >> 32);
  if (index >= endpoint->send_state_count || generation == 0) {
    return NULL;
  }
  iree_net_loopback_send_state_t* send_state = &endpoint->send_states[index];
  if (send_state->phase != IREE_NET_LOOPBACK_SEND_STATE_PHASE_RESERVED ||
      send_state->generation != generation) {
    return NULL;
  }
  return send_state;
}

static void iree_net_loopback_send_complete(
    void* user_data, iree_status_t status,
    iree_host_size_t wire_bytes_transferred) {
  iree_net_loopback_send_state_t* send_state =
      (iree_net_loopback_send_state_t*)user_data;
  iree_net_loopback_framed_endpoint_t* endpoint = send_state->endpoint;

  iree_slim_mutex_lock(&endpoint->mutex);
  IREE_ASSERT(send_state->phase == IREE_NET_LOOPBACK_SEND_STATE_PHASE_IN_FLIGHT,
              "loopback send completed from phase %d", (int)send_state->phase);
  const iree_host_size_t payload_length = send_state->payload_length;
  iree_net_send_completion_callback_t completion_callback =
      send_state->completion_callback;
  iree_net_loopback_release_send_state_locked(endpoint, send_state);
  iree_slim_mutex_unlock(&endpoint->mutex);

  IREE_ASSERT(!iree_status_is_ok(status) ||
                  wire_bytes_transferred ==
                      payload_length + IREE_NET_LOOPBACK_FRAME_HEADER_SIZE,
              "successful loopback framed send completed %" PRIhsz
              " of %" PRIhsz " wire bytes",
              wire_bytes_transferred,
              payload_length + IREE_NET_LOOPBACK_FRAME_HEADER_SIZE);
  iree_host_size_t payload_bytes_transferred = 0;
  if (wire_bytes_transferred > IREE_NET_LOOPBACK_FRAME_HEADER_SIZE) {
    payload_bytes_transferred =
        iree_min(payload_length,
                 wire_bytes_transferred - IREE_NET_LOOPBACK_FRAME_HEADER_SIZE);
  }
  completion_callback.fn(completion_callback.user_data, status,
                         payload_bytes_transferred);
}

static iree_status_t iree_net_loopback_on_wire_message(
    void* user_data, iree_const_byte_span_t frame,
    iree_async_buffer_lease_t* lease) {
  iree_net_loopback_framed_endpoint_t* endpoint =
      (iree_net_loopback_framed_endpoint_t*)user_data;
  IREE_ASSERT(frame.data_length > IREE_NET_LOOPBACK_FRAME_HEADER_SIZE,
              "validated loopback frame must contain a payload");
  iree_const_byte_span_t payload = iree_make_const_byte_span(
      frame.data + IREE_NET_LOOPBACK_FRAME_HEADER_SIZE,
      frame.data_length - IREE_NET_LOOPBACK_FRAME_HEADER_SIZE);
  return endpoint->callbacks.on_message(endpoint->callbacks.user_data, payload,
                                        lease);
}

static void iree_net_loopback_on_wire_error(void* user_data,
                                            iree_status_t status) {
  iree_net_loopback_framed_endpoint_t* endpoint =
      (iree_net_loopback_framed_endpoint_t*)user_data;
  endpoint->callbacks.on_error(endpoint->callbacks.user_data, status);
}

static void iree_net_loopback_set_callbacks(
    void* self, iree_net_message_endpoint_callbacks_t callbacks) {
  iree_net_loopback_framed_endpoint_t* endpoint =
      (iree_net_loopback_framed_endpoint_t*)self;
  endpoint->callbacks = callbacks;
}

static iree_status_t iree_net_loopback_activate(void* self) {
  iree_net_loopback_framed_endpoint_t* endpoint =
      (iree_net_loopback_framed_endpoint_t*)self;
  iree_slim_mutex_lock(&endpoint->mutex);
  iree_status_t status = iree_ok_status();
  if (endpoint->state != IREE_NET_LOOPBACK_FRAMED_ENDPOINT_STATE_CREATED) {
    status = iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                              "loopback endpoint cannot activate from state %d",
                              (int)endpoint->state);
  } else if (!endpoint->callbacks.on_message || !endpoint->callbacks.on_error) {
    status = iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                              "message and error callbacks are required");
  } else {
    endpoint->state = IREE_NET_LOOPBACK_FRAMED_ENDPOINT_STATE_ACTIVE;
  }
  iree_slim_mutex_unlock(&endpoint->mutex);
  if (!iree_status_is_ok(status)) {
    return status;
  }

  status = iree_net_message_endpoint_activate(endpoint->wire_endpoint);
  if (!iree_status_is_ok(status)) {
    iree_slim_mutex_lock(&endpoint->mutex);
    endpoint->state = IREE_NET_LOOPBACK_FRAMED_ENDPOINT_STATE_CREATED;
    iree_slim_mutex_unlock(&endpoint->mutex);
  }
  return status;
}

static void iree_net_loopback_clear_reservations_locked(
    iree_net_loopback_framed_endpoint_t* endpoint) {
  for (uint32_t i = 0; i < endpoint->send_state_count; ++i) {
    iree_net_loopback_send_state_t* send_state = &endpoint->send_states[i];
    if (send_state->phase == IREE_NET_LOOPBACK_SEND_STATE_PHASE_RESERVED) {
      iree_net_loopback_release_send_state_locked(endpoint, send_state);
    }
  }
}

static void iree_net_loopback_on_wire_deactivated(void* user_data) {
  iree_net_loopback_framed_endpoint_t* endpoint =
      (iree_net_loopback_framed_endpoint_t*)user_data;
  iree_slim_mutex_lock(&endpoint->mutex);
  IREE_ASSERT(
      endpoint->state == IREE_NET_LOOPBACK_FRAMED_ENDPOINT_STATE_DRAINING,
      "loopback endpoint deactivated from state %d", (int)endpoint->state);
  endpoint->state = IREE_NET_LOOPBACK_FRAMED_ENDPOINT_STATE_DEACTIVATED;
  iree_net_message_endpoint_deactivate_fn_t callback =
      endpoint->deactivate_callback.fn;
  void* callback_user_data = endpoint->deactivate_callback.user_data;
  endpoint->deactivate_callback.fn = NULL;
  endpoint->deactivate_callback.user_data = NULL;
  iree_slim_mutex_unlock(&endpoint->mutex);

  if (callback) {
    callback(callback_user_data);
  }
}

static iree_status_t iree_net_loopback_deactivate(
    void* self, iree_net_message_endpoint_deactivate_fn_t callback,
    void* user_data) {
  iree_net_loopback_framed_endpoint_t* endpoint =
      (iree_net_loopback_framed_endpoint_t*)self;
  iree_slim_mutex_lock(&endpoint->mutex);
  iree_status_t status = iree_ok_status();
  if (endpoint->state != IREE_NET_LOOPBACK_FRAMED_ENDPOINT_STATE_ACTIVE) {
    status = iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                              "loopback endpoint is not active (state=%d)",
                              (int)endpoint->state);
  } else {
    endpoint->state = IREE_NET_LOOPBACK_FRAMED_ENDPOINT_STATE_DRAINING;
    endpoint->deactivate_callback.fn = callback;
    endpoint->deactivate_callback.user_data = user_data;
    iree_net_loopback_clear_reservations_locked(endpoint);
  }
  iree_slim_mutex_unlock(&endpoint->mutex);
  if (!iree_status_is_ok(status)) {
    return status;
  }

  status = iree_net_message_endpoint_deactivate(
      endpoint->wire_endpoint, iree_net_loopback_on_wire_deactivated, endpoint);
  IREE_ASSERT(iree_status_is_ok(status),
              "mirrored wire endpoint rejected valid deactivation");
  return status;
}

static iree_status_t iree_net_loopback_calculate_payload_length(
    const iree_net_message_endpoint_send_params_t* params,
    iree_host_size_t* out_payload_length) {
  iree_host_size_t payload_length = params->copied_prefix.data_length;
  for (iree_host_size_t i = 0; i < params->data.count; ++i) {
    if (!iree_host_size_checked_add(
            payload_length, params->data.values[i].length, &payload_length)) {
      return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                              "loopback message length overflow");
    }
  }
  *out_payload_length = payload_length;
  return iree_ok_status();
}

static iree_status_t iree_net_loopback_send_contiguous_locked(
    iree_net_loopback_framed_endpoint_t* endpoint,
    iree_net_loopback_send_state_t* send_state,
    const iree_net_message_endpoint_send_params_t* params,
    uint32_t frame_length) {
  void* wire_data = NULL;
  iree_net_carrier_send_handle_t carrier_handle = 0;
  IREE_RETURN_IF_ERROR(iree_net_message_endpoint_begin_send(
      endpoint->wire_endpoint, frame_length, &wire_data, &carrier_handle));

  uint8_t* frame_data = (uint8_t*)wire_data;
  iree_net_loopback_encode_frame_header(frame_data, frame_length);
  uint8_t* payload_data = frame_data + IREE_NET_LOOPBACK_FRAME_HEADER_SIZE;
  if (!iree_const_byte_span_is_empty(params->copied_prefix)) {
    memcpy(payload_data, params->copied_prefix.data,
           params->copied_prefix.data_length);
    payload_data += params->copied_prefix.data_length;
  }
  for (iree_host_size_t i = 0; i < params->data.count; ++i) {
    const iree_async_span_t span = params->data.values[i];
    if (!iree_async_span_is_cpu_accessible(span)) {
      iree_net_message_endpoint_abort_send(endpoint->wire_endpoint,
                                           carrier_handle);
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "loopback message span is not CPU-accessible");
    }
    if (span.length > 0) {
      if (span.region && (span.offset > span.region->length ||
                          span.length > span.region->length - span.offset)) {
        iree_net_message_endpoint_abort_send(endpoint->wire_endpoint,
                                             carrier_handle);
        return iree_make_status(
            IREE_STATUS_OUT_OF_RANGE,
            "loopback message span exceeds its registered region");
      }
      const uint8_t* source = iree_async_span_ptr(span);
      if (!source) {
        iree_net_message_endpoint_abort_send(endpoint->wire_endpoint,
                                             carrier_handle);
        return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                                "loopback message span has null storage");
      }
      memcpy(payload_data, source, span.length);
      payload_data += span.length;
    }
  }

  send_state->phase = IREE_NET_LOOPBACK_SEND_STATE_PHASE_IN_FLIGHT;
  send_state->carrier_handle = carrier_handle;
  iree_status_t status = iree_net_message_endpoint_commit_send(
      endpoint->wire_endpoint, carrier_handle,
      (iree_net_send_completion_callback_t){
          .fn = iree_net_loopback_send_complete,
          .user_data = send_state,
      });
  if (!iree_status_is_ok(status)) {
    iree_net_loopback_release_send_state_locked(endpoint, send_state);
  }
  return status;
}

static iree_status_t iree_net_loopback_send(
    void* self, const iree_net_message_endpoint_send_params_t* params) {
  iree_net_loopback_framed_endpoint_t* endpoint =
      (iree_net_loopback_framed_endpoint_t*)self;
  iree_host_size_t payload_length = 0;
  IREE_RETURN_IF_ERROR(
      iree_net_loopback_calculate_payload_length(params, &payload_length));
  uint32_t frame_length = 0;
  IREE_RETURN_IF_ERROR(
      iree_net_loopback_calculate_frame_length(payload_length, &frame_length));

  iree_slim_mutex_lock(&endpoint->mutex);
  iree_status_t status = iree_ok_status();
  iree_net_loopback_send_state_t* send_state = NULL;
  if (endpoint->state != IREE_NET_LOOPBACK_FRAMED_ENDPOINT_STATE_ACTIVE) {
    status = iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                              "loopback endpoint is not active");
  } else {
    send_state = iree_net_loopback_acquire_send_state_locked(endpoint);
    if (!send_state) {
      status = iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                                "loopback send operation slots are exhausted");
    }
  }

  if (iree_status_is_ok(status)) {
    send_state->payload_length = payload_length;
    send_state->completion_callback = params->completion_callback;
    const bool fits_scatter_gather =
        iree_const_byte_span_is_empty(params->copied_prefix) &&
        params->data.count < endpoint->max_wire_spans &&
        params->data.count + 1 <= IREE_NET_LOOPBACK_INLINE_WIRE_SPAN_COUNT;
    if (fits_scatter_gather) {
      iree_net_loopback_encode_frame_header(send_state->header, frame_length);
      iree_async_span_t wire_spans[IREE_NET_LOOPBACK_INLINE_WIRE_SPAN_COUNT];
      wire_spans[0] = iree_async_span_from_ptr(
          send_state->header, IREE_NET_LOOPBACK_FRAME_HEADER_SIZE);
      memcpy(&wire_spans[1], params->data.values,
             params->data.count * sizeof(params->data.values[0]));
      send_state->phase = IREE_NET_LOOPBACK_SEND_STATE_PHASE_IN_FLIGHT;
      iree_net_message_endpoint_send_params_t wire_params = {
          .data = iree_async_span_list_make(wire_spans, params->data.count + 1),
          .completion_callback =
              {
                  .fn = iree_net_loopback_send_complete,
                  .user_data = send_state,
              },
      };
      status =
          iree_net_message_endpoint_send(endpoint->wire_endpoint, &wire_params);
      if (!iree_status_is_ok(status)) {
        iree_net_loopback_release_send_state_locked(endpoint, send_state);
      }
    } else {
      status = iree_net_loopback_send_contiguous_locked(endpoint, send_state,
                                                        params, frame_length);
      if (!iree_status_is_ok(status) &&
          send_state->phase != IREE_NET_LOOPBACK_SEND_STATE_PHASE_FREE) {
        iree_net_loopback_release_send_state_locked(endpoint, send_state);
      }
    }
  }
  iree_slim_mutex_unlock(&endpoint->mutex);
  return status;
}

static iree_net_carrier_send_budget_t iree_net_loopback_query_send_budget(
    void* self) {
  iree_net_loopback_framed_endpoint_t* endpoint =
      (iree_net_loopback_framed_endpoint_t*)self;
  iree_slim_mutex_lock(&endpoint->mutex);
  iree_net_carrier_send_budget_t budget = {0};
  if (endpoint->state == IREE_NET_LOOPBACK_FRAMED_ENDPOINT_STATE_ACTIVE &&
      endpoint->free_send_state_count > 0) {
    budget =
        iree_net_message_endpoint_query_send_budget(endpoint->wire_endpoint);
    budget.slots = iree_min(budget.slots, endpoint->free_send_state_count);
    if (budget.bytes != IREE_HOST_SIZE_MAX) {
      budget.bytes = budget.bytes > IREE_NET_LOOPBACK_FRAME_HEADER_SIZE
                         ? budget.bytes - IREE_NET_LOOPBACK_FRAME_HEADER_SIZE
                         : 0;
    }
  }
  iree_slim_mutex_unlock(&endpoint->mutex);
  return budget;
}

static iree_status_t iree_net_loopback_begin_send(
    void* self, iree_host_size_t size, void** out_ptr,
    iree_net_carrier_send_handle_t* out_handle) {
  iree_net_loopback_framed_endpoint_t* endpoint =
      (iree_net_loopback_framed_endpoint_t*)self;
  uint32_t frame_length = 0;
  IREE_RETURN_IF_ERROR(
      iree_net_loopback_calculate_frame_length(size, &frame_length));

  iree_slim_mutex_lock(&endpoint->mutex);
  iree_status_t status = iree_ok_status();
  iree_net_loopback_send_state_t* send_state = NULL;
  if (endpoint->state != IREE_NET_LOOPBACK_FRAMED_ENDPOINT_STATE_ACTIVE) {
    status = iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                              "loopback endpoint is not active");
  } else {
    send_state = iree_net_loopback_acquire_send_state_locked(endpoint);
    if (!send_state) {
      status = iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                                "loopback send operation slots are exhausted");
    }
  }

  void* frame_data = NULL;
  iree_net_carrier_send_handle_t carrier_handle = 0;
  if (iree_status_is_ok(status)) {
    status = iree_net_message_endpoint_begin_send(
        endpoint->wire_endpoint, frame_length, &frame_data, &carrier_handle);
  }
  if (iree_status_is_ok(status)) {
    send_state->phase = IREE_NET_LOOPBACK_SEND_STATE_PHASE_RESERVED;
    send_state->carrier_handle = carrier_handle;
    send_state->payload_length = size;
    iree_net_loopback_encode_frame_header((uint8_t*)frame_data, frame_length);
    *out_ptr = (uint8_t*)frame_data + IREE_NET_LOOPBACK_FRAME_HEADER_SIZE;
    *out_handle = ((uint64_t)send_state->generation << 32) | send_state->index;
  } else if (send_state) {
    iree_net_loopback_release_send_state_locked(endpoint, send_state);
  }
  iree_slim_mutex_unlock(&endpoint->mutex);
  return status;
}

static iree_status_t iree_net_loopback_commit_send(
    void* self, iree_net_carrier_send_handle_t handle,
    iree_net_send_completion_callback_t completion_callback) {
  iree_net_loopback_framed_endpoint_t* endpoint =
      (iree_net_loopback_framed_endpoint_t*)self;
  iree_slim_mutex_lock(&endpoint->mutex);
  iree_net_loopback_send_state_t* send_state =
      iree_net_loopback_lookup_reservation_locked(endpoint, handle);
  iree_status_t status = iree_ok_status();
  if (!send_state ||
      endpoint->state != IREE_NET_LOOPBACK_FRAMED_ENDPOINT_STATE_ACTIVE) {
    status = iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                              "loopback send reservation is no longer valid");
  } else {
    send_state->phase = IREE_NET_LOOPBACK_SEND_STATE_PHASE_IN_FLIGHT;
    send_state->completion_callback = completion_callback;
    status = iree_net_message_endpoint_commit_send(
        endpoint->wire_endpoint, send_state->carrier_handle,
        (iree_net_send_completion_callback_t){
            .fn = iree_net_loopback_send_complete,
            .user_data = send_state,
        });
    if (!iree_status_is_ok(status)) {
      iree_net_loopback_release_send_state_locked(endpoint, send_state);
    }
  }
  iree_slim_mutex_unlock(&endpoint->mutex);
  return status;
}

static void iree_net_loopback_abort_send(
    void* self, iree_net_carrier_send_handle_t handle) {
  iree_net_loopback_framed_endpoint_t* endpoint =
      (iree_net_loopback_framed_endpoint_t*)self;
  iree_slim_mutex_lock(&endpoint->mutex);
  iree_net_loopback_send_state_t* send_state =
      iree_net_loopback_lookup_reservation_locked(endpoint, handle);
  IREE_ASSERT(send_state, "loopback send reservation is no longer valid");
  if (send_state) {
    const iree_net_carrier_send_handle_t carrier_handle =
        send_state->carrier_handle;
    iree_net_loopback_release_send_state_locked(endpoint, send_state);
    iree_net_message_endpoint_abort_send(endpoint->wire_endpoint,
                                         carrier_handle);
  }
  iree_slim_mutex_unlock(&endpoint->mutex);
}

static const iree_net_message_endpoint_vtable_t
    iree_net_loopback_framed_endpoint_vtable = {
        .set_callbacks = iree_net_loopback_set_callbacks,
        .activate = iree_net_loopback_activate,
        .deactivate = iree_net_loopback_deactivate,
        .send = iree_net_loopback_send,
        .query_send_budget = iree_net_loopback_query_send_budget,
        .begin_send = iree_net_loopback_begin_send,
        .commit_send = iree_net_loopback_commit_send,
        .abort_send = iree_net_loopback_abort_send,
};

iree_status_t iree_net_loopback_framed_endpoint_allocate(
    iree_net_carrier_t* carrier, uint32_t max_send_operations,
    iree_net_endpoint_deactivation_barrier_t* connection_barrier,
    iree_allocator_t host_allocator,
    iree_net_loopback_framed_endpoint_t** out_endpoint) {
  IREE_ASSERT_ARGUMENT(out_endpoint);
  *out_endpoint = NULL;
  if (!carrier || max_send_operations == 0) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "carrier and nonzero send operation limit are required");
  }

  iree_host_size_t allocation_size = 0;
  IREE_RETURN_IF_ERROR(IREE_STRUCT_LAYOUT(
      sizeof(iree_net_loopback_framed_endpoint_t), &allocation_size,
      IREE_STRUCT_FIELD_FAM(max_send_operations,
                            iree_net_loopback_send_state_t)));
  iree_net_loopback_framed_endpoint_t* endpoint = NULL;
  IREE_RETURN_IF_ERROR(iree_allocator_malloc(host_allocator, allocation_size,
                                             (void**)&endpoint));
  memset(endpoint, 0, allocation_size);
  iree_slim_mutex_initialize(&endpoint->mutex);
  endpoint->state = IREE_NET_LOOPBACK_FRAMED_ENDPOINT_STATE_CREATED;
  endpoint->host_allocator = host_allocator;
  endpoint->max_wire_spans = iree_net_carrier_max_send_spans(carrier);
  endpoint->send_state_count = max_send_operations;
  endpoint->free_send_state_count = max_send_operations;
  endpoint->free_send_state_head = 0;
  for (uint32_t i = 0; i < max_send_operations; ++i) {
    iree_net_loopback_send_state_t* send_state = &endpoint->send_states[i];
    send_state->endpoint = endpoint;
    send_state->index = i;
    send_state->next_free =
        i + 1 < max_send_operations ? i + 1 : IREE_NET_LOOPBACK_SEND_STATE_NONE;
  }

  iree_net_frame_length_callback_t frame_length = {
      .fn = iree_net_loopback_resolve_frame_length,
      .user_data = NULL,
      .max_header_size = IREE_NET_LOOPBACK_FRAME_HEADER_SIZE,
  };
  iree_status_t status = iree_net_framing_adapter_allocate(
      carrier, frame_length, UINT32_MAX, connection_barrier, host_allocator,
      &endpoint->framing_adapter);
  if (iree_status_is_ok(status)) {
    endpoint->wire_endpoint =
        iree_net_framing_adapter_as_endpoint(endpoint->framing_adapter);
    iree_net_message_endpoint_set_callbacks(
        endpoint->wire_endpoint,
        (iree_net_message_endpoint_callbacks_t){
            .on_message = iree_net_loopback_on_wire_message,
            .on_error = iree_net_loopback_on_wire_error,
            .user_data = endpoint,
        });
    *out_endpoint = endpoint;
  } else {
    iree_slim_mutex_deinitialize(&endpoint->mutex);
    iree_allocator_free(host_allocator, endpoint);
  }
  return status;
}

void iree_net_loopback_framed_endpoint_free(
    iree_net_loopback_framed_endpoint_t* endpoint) {
  if (!endpoint) {
    return;
  }
  IREE_ASSERT(endpoint->state != IREE_NET_LOOPBACK_FRAMED_ENDPOINT_STATE_ACTIVE,
              "active loopback endpoint cannot be freed");
  IREE_ASSERT(endpoint->free_send_state_count == endpoint->send_state_count,
              "loopback endpoint freed with owned send state");
  iree_allocator_t host_allocator = endpoint->host_allocator;
  iree_net_framing_adapter_free(endpoint->framing_adapter);
  iree_slim_mutex_deinitialize(&endpoint->mutex);
  iree_allocator_free(host_allocator, endpoint);
}

void iree_net_loopback_framed_endpoint_join_deactivation(
    iree_net_loopback_framed_endpoint_t* endpoint) {
  IREE_ASSERT_ARGUMENT(endpoint);
  iree_slim_mutex_lock(&endpoint->mutex);
  if (endpoint->state == IREE_NET_LOOPBACK_FRAMED_ENDPOINT_STATE_CREATED ||
      endpoint->state == IREE_NET_LOOPBACK_FRAMED_ENDPOINT_STATE_ACTIVE) {
    endpoint->state = IREE_NET_LOOPBACK_FRAMED_ENDPOINT_STATE_DRAINING;
    iree_net_loopback_clear_reservations_locked(endpoint);
  }
  iree_slim_mutex_unlock(&endpoint->mutex);
  iree_net_framing_adapter_join_deactivation(endpoint->framing_adapter);
}

iree_net_message_endpoint_t
iree_net_loopback_framed_endpoint_as_message_endpoint(
    iree_net_loopback_framed_endpoint_t* endpoint) {
  IREE_ASSERT_ARGUMENT(endpoint);
  return (iree_net_message_endpoint_t){
      .self = endpoint,
      .vtable = &iree_net_loopback_framed_endpoint_vtable,
  };
}
