// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/channel/control/control_channel.h"

#include <string.h>

#include "iree/base/alignment.h"
#include "iree/net/status_wire.h"

#if !defined(IREE_ENDIANNESS_LITTLE) || !IREE_ENDIANNESS_LITTLE
#error "IREE control channels require little-endian hosts"
#endif  // !IREE_ENDIANNESS_LITTLE

enum {
  IREE_NET_CONTROL_MESSAGE_VERSION_OFFSET = 0,
  IREE_NET_CONTROL_MESSAGE_TYPE_OFFSET = 1,
  IREE_NET_CONTROL_MESSAGE_FLAGS_OFFSET = 2,
  IREE_NET_CONTROL_MESSAGE_RESERVED_OFFSET = 3,
  IREE_NET_CONTROL_MESSAGE_VALUE_OFFSET = 4,
};

struct iree_net_control_channel_t {
  // Borrowed endpoint used for receive callback dispatch and message sends.
  iree_net_message_endpoint_t endpoint;
  // Application callbacks installed during protocol handoff.
  iree_net_control_channel_callbacks_t callbacks;
  // Host allocator used to free this channel.
  iree_allocator_t host_allocator;
};

static void iree_net_control_message_encode_header(
    iree_net_control_message_type_t type, uint8_t flags, uint32_t value,
    uint8_t out_header[IREE_NET_CONTROL_MESSAGE_HEADER_SIZE]) {
  out_header[IREE_NET_CONTROL_MESSAGE_VERSION_OFFSET] =
      IREE_NET_CONTROL_MESSAGE_VERSION;
  out_header[IREE_NET_CONTROL_MESSAGE_TYPE_OFFSET] = (uint8_t)type;
  out_header[IREE_NET_CONTROL_MESSAGE_FLAGS_OFFSET] = flags;
  out_header[IREE_NET_CONTROL_MESSAGE_RESERVED_OFFSET] = 0;
  iree_unaligned_store_le_u32(
      out_header + IREE_NET_CONTROL_MESSAGE_VALUE_OFFSET, value);
}

static iree_status_t iree_net_control_message_decode_header(
    iree_const_byte_span_t message, uint8_t* out_type, uint8_t* out_flags,
    uint32_t* out_value, iree_const_byte_span_t* out_payload) {
  if (message.data_length < IREE_NET_CONTROL_MESSAGE_HEADER_SIZE) {
    return iree_make_status(
        IREE_STATUS_DATA_LOSS,
        "control message has %" PRIhsz " bytes; expected at least %u",
        message.data_length, IREE_NET_CONTROL_MESSAGE_HEADER_SIZE);
  }
  const uint8_t* header = message.data;
  const uint8_t version = header[IREE_NET_CONTROL_MESSAGE_VERSION_OFFSET];
  if (version != IREE_NET_CONTROL_MESSAGE_VERSION) {
    return iree_make_status(IREE_STATUS_DATA_LOSS,
                            "unsupported control message version %u", version);
  }
  if (header[IREE_NET_CONTROL_MESSAGE_RESERVED_OFFSET] != 0) {
    return iree_make_status(IREE_STATUS_DATA_LOSS,
                            "control message reserved byte must be zero");
  }

  *out_type = header[IREE_NET_CONTROL_MESSAGE_TYPE_OFFSET];
  *out_flags = header[IREE_NET_CONTROL_MESSAGE_FLAGS_OFFSET];
  *out_value = iree_unaligned_load_le_u32(
      header + IREE_NET_CONTROL_MESSAGE_VALUE_OFFSET);
  *out_payload = iree_make_const_byte_span(
      message.data + IREE_NET_CONTROL_MESSAGE_HEADER_SIZE,
      message.data_length - IREE_NET_CONTROL_MESSAGE_HEADER_SIZE);
  return iree_ok_status();
}

static iree_status_t iree_net_control_channel_on_message(
    void* user_data, iree_const_byte_span_t message,
    iree_async_buffer_lease_t* lease) {
  iree_net_control_channel_t* channel = (iree_net_control_channel_t*)user_data;
  uint8_t type = 0;
  uint8_t flags = 0;
  uint32_t value = 0;
  iree_const_byte_span_t payload = iree_const_byte_span_empty();
  IREE_RETURN_IF_ERROR(iree_net_control_message_decode_header(
      message, &type, &flags, &value, &payload));

  switch (type) {
    case IREE_NET_CONTROL_MESSAGE_TYPE_DATA:
      if (value != 0) {
        return iree_make_status(IREE_STATUS_DATA_LOSS,
                                "DATA message value must be zero");
      }
      return channel->callbacks.on_data(channel->callbacks.user_data, flags,
                                        payload, lease);
    case IREE_NET_CONTROL_MESSAGE_TYPE_GOAWAY:
      if (flags != 0 || payload.data_length != 0) {
        return iree_make_status(
            IREE_STATUS_DATA_LOSS,
            "GOAWAY message flags and payload must be empty");
      }
      channel->callbacks.on_goaway(channel->callbacks.user_data, value);
      return iree_ok_status();
    case IREE_NET_CONTROL_MESSAGE_TYPE_ERROR: {
      if (flags != 0 || value != 0) {
        return iree_make_status(IREE_STATUS_DATA_LOSS,
                                "ERROR message flags and value must be zero");
      }
      iree_status_t remote_status = iree_ok_status();
      iree_status_t parse_status =
          iree_net_status_wire_deserialize(payload, &remote_status);
      if (!iree_status_is_ok(parse_status)) {
        return iree_status_join(
            iree_make_status(IREE_STATUS_DATA_LOSS,
                             "ERROR message status payload is malformed"),
            parse_status);
      }
      if (iree_status_is_ok(remote_status)) {
        return iree_make_status(IREE_STATUS_DATA_LOSS,
                                "ERROR message contains an OK status");
      }
      return remote_status;
    }
    default:
      return iree_make_status(IREE_STATUS_DATA_LOSS,
                              "unknown control message type 0x%02X", type);
  }
}

static void iree_net_control_channel_on_error(void* user_data,
                                              iree_status_t status) {
  iree_net_control_channel_t* channel = (iree_net_control_channel_t*)user_data;
  channel->callbacks.on_error(channel->callbacks.user_data, status);
}

iree_status_t iree_net_control_channel_allocate(
    iree_net_message_endpoint_t endpoint,
    iree_net_control_channel_callbacks_t callbacks,
    iree_allocator_t host_allocator, iree_net_control_channel_t** out_channel) {
  if (!out_channel) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "out_channel is required");
  }
  *out_channel = NULL;
  if (!endpoint.self || !endpoint.vtable) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "message endpoint is required");
  }
  if (!callbacks.on_data || !callbacks.on_goaway || !callbacks.on_error) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "DATA, GOAWAY, and error callbacks are required");
  }

  iree_net_control_channel_t* channel = NULL;
  IREE_RETURN_IF_ERROR(iree_allocator_malloc(host_allocator, sizeof(*channel),
                                             (void**)&channel));
  channel->endpoint = endpoint;
  channel->callbacks = callbacks;
  channel->host_allocator = host_allocator;
  *out_channel = channel;
  return iree_ok_status();
}

void iree_net_control_channel_free(iree_net_control_channel_t* channel) {
  if (!channel) return;
  iree_allocator_free(channel->host_allocator, channel);
}

void iree_net_control_channel_attach(iree_net_control_channel_t* channel) {
  IREE_ASSERT_ARGUMENT(channel);
  iree_net_message_endpoint_set_callbacks(
      channel->endpoint, (iree_net_message_endpoint_callbacks_t){
                             .on_message = iree_net_control_channel_on_message,
                             .on_error = iree_net_control_channel_on_error,
                             .user_data = channel,
                         });
}

static iree_status_t iree_net_control_channel_validate_flags(
    iree_net_control_data_flags_t flags) {
  if (flags > UINT8_MAX) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "DATA flags 0x%08X exceed the wire field", flags);
  }
  return iree_ok_status();
}

static iree_status_t iree_net_control_channel_validate_completion(
    iree_net_send_completion_callback_t completion_callback) {
  if (!completion_callback.fn) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "send completion callback is required");
  }
  return iree_ok_status();
}

iree_status_t iree_net_control_channel_send_data(
    iree_net_control_channel_t* channel, iree_net_control_data_flags_t flags,
    iree_async_span_list_t payload,
    iree_net_send_completion_callback_t completion_callback) {
  IREE_ASSERT_ARGUMENT(channel);
  IREE_RETURN_IF_ERROR(iree_net_control_channel_validate_flags(flags));

  uint8_t header[IREE_NET_CONTROL_MESSAGE_HEADER_SIZE];
  iree_net_control_message_encode_header(IREE_NET_CONTROL_MESSAGE_TYPE_DATA,
                                         (uint8_t)flags, 0, header);
  const iree_net_message_endpoint_send_params_t params = {
      .copied_prefix = iree_make_const_byte_span(header, sizeof(header)),
      .data = payload,
      .completion_callback = completion_callback,
  };
  return iree_net_message_endpoint_send(channel->endpoint, &params);
}

static iree_status_t iree_net_control_channel_measure_copy_payload(
    iree_async_span_list_t payload, iree_host_size_t* out_message_size) {
  if (payload.count > 0 && !payload.values) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "DATA span list has null storage");
  }
  iree_host_size_t message_size = IREE_NET_CONTROL_MESSAGE_HEADER_SIZE;
  for (iree_host_size_t i = 0; i < payload.count; ++i) {
    const iree_async_span_t span = payload.values[i];
    if (!iree_async_span_is_cpu_accessible(span)) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "DATA span %" PRIhsz " is not CPU-accessible", i);
    }
    if (span.region && (span.offset > span.region->length ||
                        span.length > span.region->length - span.offset)) {
      return iree_make_status(
          IREE_STATUS_OUT_OF_RANGE,
          "DATA span %" PRIhsz " exceeds its registered region", i);
    }
    if (span.length > 0 && !iree_async_span_ptr(span)) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "DATA span %" PRIhsz " has null storage", i);
    }
    if (!iree_host_size_checked_add(message_size, span.length, &message_size)) {
      return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                              "DATA message length overflow");
    }
  }
  *out_message_size = message_size;
  return iree_ok_status();
}

iree_status_t iree_net_control_channel_send_data_copy(
    iree_net_control_channel_t* channel, iree_net_control_data_flags_t flags,
    iree_async_span_list_t payload,
    iree_net_send_completion_callback_t completion_callback) {
  IREE_ASSERT_ARGUMENT(channel);
  IREE_RETURN_IF_ERROR(iree_net_control_channel_validate_flags(flags));
  IREE_RETURN_IF_ERROR(
      iree_net_control_channel_validate_completion(completion_callback));

  iree_host_size_t message_size = 0;
  IREE_RETURN_IF_ERROR(
      iree_net_control_channel_measure_copy_payload(payload, &message_size));

  void* message_storage = NULL;
  iree_net_carrier_send_handle_t send_handle = 0;
  IREE_RETURN_IF_ERROR(iree_net_message_endpoint_begin_send(
      channel->endpoint, message_size, &message_storage, &send_handle));

  uint8_t* message = (uint8_t*)message_storage;
  iree_net_control_message_encode_header(IREE_NET_CONTROL_MESSAGE_TYPE_DATA,
                                         (uint8_t)flags, 0, message);
  uint8_t* target = message + IREE_NET_CONTROL_MESSAGE_HEADER_SIZE;
  for (iree_host_size_t i = 0; i < payload.count; ++i) {
    const iree_async_span_t span = payload.values[i];
    if (span.length > 0) {
      memcpy(target, iree_async_span_ptr(span), span.length);
      target += span.length;
    }
  }
  return iree_net_message_endpoint_commit_send(channel->endpoint, send_handle,
                                               completion_callback);
}

iree_status_t iree_net_control_channel_send_goaway(
    iree_net_control_channel_t* channel, uint32_t reason_code,
    iree_net_send_completion_callback_t completion_callback) {
  IREE_ASSERT_ARGUMENT(channel);
  uint8_t header[IREE_NET_CONTROL_MESSAGE_HEADER_SIZE];
  iree_net_control_message_encode_header(IREE_NET_CONTROL_MESSAGE_TYPE_GOAWAY,
                                         0, reason_code, header);
  const iree_net_message_endpoint_send_params_t params = {
      .copied_prefix = iree_make_const_byte_span(header, sizeof(header)),
      .data = iree_async_span_list_empty(),
      .completion_callback = completion_callback,
  };
  return iree_net_message_endpoint_send(channel->endpoint, &params);
}

iree_status_t iree_net_control_channel_send_error(
    iree_net_control_channel_t* channel, iree_status_t error_status,
    iree_net_send_completion_callback_t completion_callback) {
  IREE_ASSERT_ARGUMENT(channel);
  if (iree_status_is_ok(error_status)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "ERROR message requires a non-OK status");
  }

  iree_status_t status =
      iree_net_control_channel_validate_completion(completion_callback);
  iree_host_size_t status_wire_size = 0;
  if (iree_status_is_ok(status)) {
    status =
        iree_net_status_wire_calculate_size(error_status, &status_wire_size);
  }
  iree_host_size_t message_size = 0;
  if (iree_status_is_ok(status) &&
      !iree_host_size_checked_add(IREE_NET_CONTROL_MESSAGE_HEADER_SIZE,
                                  status_wire_size, &message_size)) {
    status = iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                              "ERROR message length overflow");
  }

  void* message_storage = NULL;
  iree_net_carrier_send_handle_t send_handle = 0;
  if (iree_status_is_ok(status)) {
    status = iree_net_message_endpoint_begin_send(
        channel->endpoint, message_size, &message_storage, &send_handle);
  }
  if (iree_status_is_ok(status)) {
    uint8_t* message = (uint8_t*)message_storage;
    iree_net_control_message_encode_header(IREE_NET_CONTROL_MESSAGE_TYPE_ERROR,
                                           0, 0, message);
    status = iree_net_status_wire_serialize(
        error_status,
        iree_make_byte_span(message + IREE_NET_CONTROL_MESSAGE_HEADER_SIZE,
                            status_wire_size));
    if (!iree_status_is_ok(status)) {
      iree_net_message_endpoint_abort_send(channel->endpoint, send_handle);
    }
  }

  iree_status_free(error_status);
  if (!iree_status_is_ok(status)) return status;
  return iree_net_message_endpoint_commit_send(channel->endpoint, send_handle,
                                               completion_callback);
}
