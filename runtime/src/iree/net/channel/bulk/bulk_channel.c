// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/channel/bulk/bulk_channel.h"

#include <string.h>

#include "iree/base/alignment.h"
#include "iree/base/internal/atomics.h"

#if !defined(IREE_ENDIANNESS_LITTLE) || !IREE_ENDIANNESS_LITTLE
#error "IREE bulk channels require little-endian hosts"
#endif  // !IREE_ENDIANNESS_LITTLE

enum {
  IREE_NET_BULK_MESSAGE_VERSION_OFFSET = 0,
  IREE_NET_BULK_MESSAGE_TYPE_OFFSET = 1,
  IREE_NET_BULK_MESSAGE_RESERVED_OFFSET = 2,
  IREE_NET_BULK_MESSAGE_RESERVED_LENGTH = 6,
  IREE_NET_BULK_MESSAGE_TRANSFER_ID_OFFSET = 8,
  IREE_NET_BULK_MESSAGE_VALUE_OFFSET = 16,
};

struct iree_net_bulk_channel_t {
  // Borrowed endpoint used for receive callback dispatch and message sends.
  iree_net_message_endpoint_t endpoint;
  // Application callbacks installed during protocol handoff.
  iree_net_bulk_channel_callbacks_t callbacks;
  // Cumulative DATA-message receive credit published to the peer.
  iree_atomic_uint64_t local_credit_limit;
  // Peer DATA messages consumed from |local_credit_limit|.
  iree_atomic_uint64_t local_credit_consumed;
  // Cumulative DATA-message receive credit observed from the peer.
  iree_atomic_uint64_t remote_credit_limit;
  // Local DATA messages consumed from |remote_credit_limit|.
  iree_atomic_uint64_t remote_credit_consumed;
  // Host allocator used to free this channel.
  iree_allocator_t host_allocator;
};

typedef struct iree_net_bulk_send_prefix_t {
  // Channel whose credit counters may be updated after endpoint admission.
  iree_net_bulk_channel_t* channel;
  // Message type encoded in the header.
  iree_net_bulk_message_type_t type;
  // Application-owned transfer ID encoded in the header.
  uint64_t transfer_id;
  // Type-specific value or CREDIT delta before encoding.
  uint64_t value;
} iree_net_bulk_send_prefix_t;

static void iree_net_bulk_message_encode_header(
    iree_net_bulk_message_type_t type, uint64_t transfer_id, uint64_t value,
    uint8_t out_header[IREE_NET_BULK_MESSAGE_HEADER_SIZE]) {
  memset(out_header, 0, IREE_NET_BULK_MESSAGE_HEADER_SIZE);
  out_header[IREE_NET_BULK_MESSAGE_VERSION_OFFSET] =
      IREE_NET_BULK_MESSAGE_VERSION;
  out_header[IREE_NET_BULK_MESSAGE_TYPE_OFFSET] = (uint8_t)type;
  iree_unaligned_store_le_u64(
      out_header + IREE_NET_BULK_MESSAGE_TRANSFER_ID_OFFSET, transfer_id);
  iree_unaligned_store_le_u64(out_header + IREE_NET_BULK_MESSAGE_VALUE_OFFSET,
                              value);
}

static bool iree_net_bulk_channel_try_consume_credit(
    iree_atomic_uint64_t* credit_limit, iree_atomic_uint64_t* credit_consumed) {
  uint64_t consumed =
      iree_atomic_load(credit_consumed, iree_memory_order_acquire);
  while (true) {
    const uint64_t limit =
        iree_atomic_load(credit_limit, iree_memory_order_acquire);
    if (consumed >= limit) {
      return false;
    }
    if (iree_atomic_compare_exchange_weak(
            credit_consumed, &consumed, consumed + 1, iree_memory_order_acq_rel,
            iree_memory_order_acquire)) {
      return true;
    }
  }
}

static iree_status_t iree_net_bulk_channel_grant_local_credit(
    iree_net_bulk_channel_t* channel, uint64_t credit_delta,
    uint64_t* out_credit_limit) {
  uint64_t credit_limit =
      iree_atomic_load(&channel->local_credit_limit, iree_memory_order_acquire);
  while (true) {
    if (credit_delta > UINT64_MAX - credit_limit) {
      return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                              "bulk credit grant overflow");
    }
    const uint64_t updated_credit_limit = credit_limit + credit_delta;
    if (iree_atomic_compare_exchange_weak(
            &channel->local_credit_limit, &credit_limit, updated_credit_limit,
            iree_memory_order_acq_rel, iree_memory_order_acquire)) {
      *out_credit_limit = updated_credit_limit;
      return iree_ok_status();
    }
  }
}

static bool iree_net_bulk_channel_update_remote_credit(
    iree_net_bulk_channel_t* channel, uint64_t credit_limit,
    uint64_t* out_credit_delta, uint64_t* out_available_credit_count) {
  *out_credit_delta = 0;
  *out_available_credit_count = 0;
  uint64_t previous_limit = iree_atomic_load(&channel->remote_credit_limit,
                                             iree_memory_order_acquire);
  while (credit_limit > previous_limit) {
    if (iree_atomic_compare_exchange_weak(
            &channel->remote_credit_limit, &previous_limit, credit_limit,
            iree_memory_order_acq_rel, iree_memory_order_acquire)) {
      *out_credit_delta = credit_limit - previous_limit;
      const uint64_t consumed = iree_atomic_load(
          &channel->remote_credit_consumed, iree_memory_order_acquire);
      *out_available_credit_count =
          credit_limit > consumed ? credit_limit - consumed : 0;
      return true;
    }
  }
  return false;
}

static iree_status_t iree_net_bulk_channel_on_message(
    void* user_data, iree_const_byte_span_t message,
    iree_async_buffer_lease_t* lease) {
  iree_net_bulk_channel_t* channel = (iree_net_bulk_channel_t*)user_data;
  if (message.data_length < IREE_NET_BULK_MESSAGE_HEADER_SIZE) {
    return iree_make_status(
        IREE_STATUS_DATA_LOSS,
        "bulk message has %" PRIhsz " bytes; expected at least %u",
        message.data_length, IREE_NET_BULK_MESSAGE_HEADER_SIZE);
  }

  const uint8_t* header = message.data;
  const uint8_t version = header[IREE_NET_BULK_MESSAGE_VERSION_OFFSET];
  if (version != IREE_NET_BULK_MESSAGE_VERSION) {
    return iree_make_status(IREE_STATUS_DATA_LOSS,
                            "unsupported bulk message version %u", version);
  }
  for (iree_host_size_t i = 0; i < IREE_NET_BULK_MESSAGE_RESERVED_LENGTH; ++i) {
    if (header[IREE_NET_BULK_MESSAGE_RESERVED_OFFSET + i] != 0) {
      return iree_make_status(IREE_STATUS_DATA_LOSS,
                              "bulk message reserved bytes must be zero");
    }
  }

  const uint8_t type = header[IREE_NET_BULK_MESSAGE_TYPE_OFFSET];
  const uint64_t transfer_id = iree_unaligned_load_le_u64(
      header + IREE_NET_BULK_MESSAGE_TRANSFER_ID_OFFSET);
  const uint64_t value =
      iree_unaligned_load_le_u64(header + IREE_NET_BULK_MESSAGE_VALUE_OFFSET);
  const iree_const_byte_span_t payload = iree_make_const_byte_span(
      message.data + IREE_NET_BULK_MESSAGE_HEADER_SIZE,
      message.data_length - IREE_NET_BULK_MESSAGE_HEADER_SIZE);

  switch (type) {
    case IREE_NET_BULK_MESSAGE_TYPE_START:
      if (transfer_id == 0) {
        return iree_make_status(IREE_STATUS_DATA_LOSS,
                                "START transfer ID must be nonzero");
      }
      if (payload.data_length != 0) {
        return iree_make_status(IREE_STATUS_DATA_LOSS,
                                "START payload must be empty");
      }
      return channel->callbacks.on_start(channel->callbacks.user_data,
                                         transfer_id, value);
    case IREE_NET_BULK_MESSAGE_TYPE_DATA:
      if (transfer_id == 0) {
        return iree_make_status(IREE_STATUS_DATA_LOSS,
                                "DATA transfer ID must be nonzero");
      }
      if (payload.data_length == 0) {
        return iree_make_status(IREE_STATUS_DATA_LOSS,
                                "DATA payload must be nonempty");
      }
      if ((uint64_t)payload.data_length > UINT64_MAX - value) {
        return iree_make_status(IREE_STATUS_DATA_LOSS,
                                "DATA transfer range overflows uint64_t");
      }
      if (!iree_net_bulk_channel_try_consume_credit(
              &channel->local_credit_limit, &channel->local_credit_consumed)) {
        return iree_make_status(
            IREE_STATUS_DATA_LOSS,
            "peer sent DATA beyond the cumulative receive credit grant");
      }
      return channel->callbacks.on_data(channel->callbacks.user_data,
                                        transfer_id, value, payload, lease);
    case IREE_NET_BULK_MESSAGE_TYPE_COMPLETE:
      if (transfer_id == 0) {
        return iree_make_status(IREE_STATUS_DATA_LOSS,
                                "COMPLETE transfer ID must be nonzero");
      }
      if (value != 0 || payload.data_length != 0) {
        return iree_make_status(IREE_STATUS_DATA_LOSS,
                                "COMPLETE value and payload must be empty");
      }
      return channel->callbacks.on_complete(channel->callbacks.user_data,
                                            transfer_id);
    case IREE_NET_BULK_MESSAGE_TYPE_ABORT:
      if (transfer_id == 0) {
        return iree_make_status(IREE_STATUS_DATA_LOSS,
                                "ABORT transfer ID must be nonzero");
      }
      if (value != 0) {
        return iree_make_status(IREE_STATUS_DATA_LOSS,
                                "ABORT value must be zero");
      }
      return channel->callbacks.on_abort(channel->callbacks.user_data,
                                         transfer_id, payload, lease);
    case IREE_NET_BULK_MESSAGE_TYPE_CREDIT: {
      if (transfer_id != 0) {
        return iree_make_status(IREE_STATUS_DATA_LOSS,
                                "CREDIT transfer ID must be zero");
      }
      if (value == 0) {
        return iree_make_status(IREE_STATUS_DATA_LOSS,
                                "CREDIT cumulative grant must be nonzero");
      }
      if (payload.data_length != 0) {
        return iree_make_status(IREE_STATUS_DATA_LOSS,
                                "CREDIT payload must be empty");
      }
      uint64_t credit_delta = 0;
      uint64_t available_credit_count = 0;
      if (!iree_net_bulk_channel_update_remote_credit(
              channel, value, &credit_delta, &available_credit_count)) {
        return iree_ok_status();
      }
      return channel->callbacks.on_credit(channel->callbacks.user_data,
                                          credit_delta, available_credit_count);
    }
    default:
      return iree_make_status(IREE_STATUS_DATA_LOSS,
                              "unknown bulk message type 0x%02X", type);
  }
}

static void iree_net_bulk_channel_on_error(void* user_data,
                                           iree_status_t status) {
  iree_net_bulk_channel_t* channel = (iree_net_bulk_channel_t*)user_data;
  channel->callbacks.on_error(channel->callbacks.user_data, status);
}

iree_status_t iree_net_bulk_channel_allocate(
    iree_net_message_endpoint_t endpoint,
    iree_net_bulk_channel_callbacks_t callbacks,
    iree_allocator_t host_allocator, iree_net_bulk_channel_t** out_channel) {
  if (!out_channel) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "out_channel is required");
  }
  *out_channel = NULL;
  if (!endpoint.self || !endpoint.vtable) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "message endpoint is required");
  }
  if (!callbacks.on_start || !callbacks.on_data || !callbacks.on_complete ||
      !callbacks.on_abort || !callbacks.on_credit || !callbacks.on_error) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "START, DATA, COMPLETE, ABORT, CREDIT, and error callbacks are "
        "required");
  }

  iree_net_bulk_channel_t* channel = NULL;
  IREE_RETURN_IF_ERROR(iree_allocator_malloc(host_allocator, sizeof(*channel),
                                             (void**)&channel));
  channel->endpoint = endpoint;
  channel->callbacks = callbacks;
  iree_atomic_store(&channel->local_credit_limit, 0, iree_memory_order_relaxed);
  iree_atomic_store(&channel->local_credit_consumed, 0,
                    iree_memory_order_relaxed);
  iree_atomic_store(&channel->remote_credit_limit, 0,
                    iree_memory_order_relaxed);
  iree_atomic_store(&channel->remote_credit_consumed, 0,
                    iree_memory_order_relaxed);
  channel->host_allocator = host_allocator;
  *out_channel = channel;
  return iree_ok_status();
}

void iree_net_bulk_channel_free(iree_net_bulk_channel_t* channel) {
  if (!channel) {
    return;
  }
  iree_allocator_free(channel->host_allocator, channel);
}

void iree_net_bulk_channel_attach(iree_net_bulk_channel_t* channel) {
  IREE_ASSERT_ARGUMENT(channel);
  iree_net_message_endpoint_set_callbacks(
      channel->endpoint, (iree_net_message_endpoint_callbacks_t){
                             .on_message = iree_net_bulk_channel_on_message,
                             .on_error = iree_net_bulk_channel_on_error,
                             .user_data = channel,
                         });
}

iree_net_carrier_send_budget_t iree_net_bulk_channel_query_send_budget(
    iree_net_bulk_channel_t* channel) {
  IREE_ASSERT_ARGUMENT(channel);
  return iree_net_message_endpoint_query_send_budget(channel->endpoint);
}

uint64_t iree_net_bulk_channel_remote_credit_count(
    const iree_net_bulk_channel_t* channel) {
  IREE_ASSERT_ARGUMENT(channel);
  const uint64_t credit_limit = iree_atomic_load(
      &((iree_net_bulk_channel_t*)channel)->remote_credit_limit,
      iree_memory_order_acquire);
  const uint64_t consumed = iree_atomic_load(
      &((iree_net_bulk_channel_t*)channel)->remote_credit_consumed,
      iree_memory_order_acquire);
  return credit_limit > consumed ? credit_limit - consumed : 0;
}

static iree_status_t iree_net_bulk_channel_measure_payload(
    iree_async_span_list_t payload, iree_host_size_t* out_payload_length) {
  *out_payload_length = 0;
  if (payload.count > 0 && !payload.values) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "bulk payload span list has null storage");
  }
  iree_host_size_t payload_length = 0;
  for (iree_host_size_t i = 0; i < payload.count; ++i) {
    if (!iree_host_size_checked_add(payload_length, payload.values[i].length,
                                    &payload_length)) {
      return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                              "bulk payload length overflow");
    }
  }
  *out_payload_length = payload_length;
  return iree_ok_status();
}

static iree_status_t iree_net_bulk_channel_validate_send(
    iree_net_bulk_message_type_t type, uint64_t transfer_id, uint64_t value,
    iree_host_size_t payload_length,
    iree_net_send_completion_callback_t completion_callback) {
  if (!completion_callback.fn) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "send completion callback is required");
  }
  if (payload_length > UINT32_MAX - IREE_NET_BULK_MESSAGE_HEADER_SIZE) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "bulk message length exceeds the 32-bit wire "
                            "extent");
  }

  switch (type) {
    case IREE_NET_BULK_MESSAGE_TYPE_START:
      if (transfer_id == 0) {
        return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                                "START transfer ID must be nonzero");
      }
      if (payload_length != 0) {
        return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                                "START payload must be empty");
      }
      return iree_ok_status();
    case IREE_NET_BULK_MESSAGE_TYPE_DATA:
      if (transfer_id == 0) {
        return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                                "DATA transfer ID must be nonzero");
      }
      if (payload_length == 0) {
        return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                                "DATA payload must be nonempty");
      }
      if ((uint64_t)payload_length > UINT64_MAX - value) {
        return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                                "DATA transfer range overflows uint64_t");
      }
      return iree_ok_status();
    case IREE_NET_BULK_MESSAGE_TYPE_COMPLETE:
      if (transfer_id == 0) {
        return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                                "COMPLETE transfer ID must be nonzero");
      }
      if (value != 0 || payload_length != 0) {
        return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                                "COMPLETE value and payload must be empty");
      }
      return iree_ok_status();
    case IREE_NET_BULK_MESSAGE_TYPE_ABORT:
      if (transfer_id == 0) {
        return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                                "ABORT transfer ID must be nonzero");
      }
      if (value != 0) {
        return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                                "ABORT value must be zero");
      }
      return iree_ok_status();
    case IREE_NET_BULK_MESSAGE_TYPE_CREDIT:
      if (transfer_id != 0) {
        return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                                "CREDIT transfer ID must be zero");
      }
      if (value == 0) {
        return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                                "CREDIT delta must be nonzero");
      }
      if (payload_length != 0) {
        return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                                "CREDIT payload must be empty");
      }
      return iree_ok_status();
    default:
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "unknown bulk message type 0x%02X",
                              (uint8_t)type);
  }
}

static iree_status_t iree_net_bulk_channel_write_prefix(
    void* user_data, iree_byte_span_t target) {
  iree_net_bulk_send_prefix_t* prefix = (iree_net_bulk_send_prefix_t*)user_data;
  uint64_t wire_value = prefix->value;
  if (prefix->type == IREE_NET_BULK_MESSAGE_TYPE_DATA) {
    if (!iree_net_bulk_channel_try_consume_credit(
            &prefix->channel->remote_credit_limit,
            &prefix->channel->remote_credit_consumed)) {
      return iree_status_from_code(IREE_STATUS_RESOURCE_EXHAUSTED);
    }
  } else if (prefix->type == IREE_NET_BULK_MESSAGE_TYPE_CREDIT) {
    IREE_RETURN_IF_ERROR(iree_net_bulk_channel_grant_local_credit(
        prefix->channel, prefix->value, &wire_value));
  }
  iree_net_bulk_message_encode_header(prefix->type, prefix->transfer_id,
                                      wire_value, target.data);
  return iree_ok_status();
}

static iree_status_t iree_net_bulk_channel_send(
    iree_net_bulk_channel_t* channel, iree_net_bulk_message_type_t type,
    uint64_t transfer_id, uint64_t value, iree_async_span_list_t payload,
    iree_net_send_completion_callback_t completion_callback) {
  iree_host_size_t payload_length = 0;
  IREE_RETURN_IF_ERROR(
      iree_net_bulk_channel_measure_payload(payload, &payload_length));
  IREE_RETURN_IF_ERROR(iree_net_bulk_channel_validate_send(
      type, transfer_id, value, payload_length, completion_callback));

  iree_net_bulk_send_prefix_t prefix = {
      .channel = channel,
      .type = type,
      .transfer_id = transfer_id,
      .value = value,
  };
  const iree_net_message_endpoint_send_params_t endpoint_params = {
      .generated_prefix =
          {
              .length = IREE_NET_BULK_MESSAGE_HEADER_SIZE,
              .write = iree_net_bulk_channel_write_prefix,
              .user_data = &prefix,
          },
      .data = payload,
      .completion_callback = completion_callback,
  };
  return iree_net_message_endpoint_send(channel->endpoint, &endpoint_params);
}

iree_status_t iree_net_bulk_channel_send_start(
    iree_net_bulk_channel_t* channel, uint64_t transfer_id,
    uint64_t total_length,
    iree_net_send_completion_callback_t completion_callback) {
  IREE_ASSERT_ARGUMENT(channel);
  return iree_net_bulk_channel_send(
      channel, IREE_NET_BULK_MESSAGE_TYPE_START, transfer_id, total_length,
      iree_async_span_list_empty(), completion_callback);
}

iree_status_t iree_net_bulk_channel_send_data(
    iree_net_bulk_channel_t* channel, uint64_t transfer_id, uint64_t offset,
    iree_async_span_list_t payload,
    iree_net_send_completion_callback_t completion_callback) {
  IREE_ASSERT_ARGUMENT(channel);
  return iree_net_bulk_channel_send(channel, IREE_NET_BULK_MESSAGE_TYPE_DATA,
                                    transfer_id, offset, payload,
                                    completion_callback);
}

iree_status_t iree_net_bulk_channel_send_complete(
    iree_net_bulk_channel_t* channel, uint64_t transfer_id,
    iree_net_send_completion_callback_t completion_callback) {
  IREE_ASSERT_ARGUMENT(channel);
  return iree_net_bulk_channel_send(
      channel, IREE_NET_BULK_MESSAGE_TYPE_COMPLETE, transfer_id, 0,
      iree_async_span_list_empty(), completion_callback);
}

iree_status_t iree_net_bulk_channel_send_abort(
    iree_net_bulk_channel_t* channel, uint64_t transfer_id,
    iree_async_span_list_t detail,
    iree_net_send_completion_callback_t completion_callback) {
  IREE_ASSERT_ARGUMENT(channel);
  return iree_net_bulk_channel_send(channel, IREE_NET_BULK_MESSAGE_TYPE_ABORT,
                                    transfer_id, 0, detail,
                                    completion_callback);
}

iree_status_t iree_net_bulk_channel_send_credit(
    iree_net_bulk_channel_t* channel, uint64_t credit_delta,
    iree_net_send_completion_callback_t completion_callback) {
  IREE_ASSERT_ARGUMENT(channel);
  return iree_net_bulk_channel_send(
      channel, IREE_NET_BULK_MESSAGE_TYPE_CREDIT, 0, credit_delta,
      iree_async_span_list_empty(), completion_callback);
}
