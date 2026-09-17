// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/bootstrap.h"

#include <string.h>

#include "iree/base/alignment.h"

#if !defined(IREE_ENDIANNESS_LITTLE) || !IREE_ENDIANNESS_LITTLE
#error "IREE session bootstrap requires little-endian hosts"
#endif  // !IREE_ENDIANNESS_LITTLE

enum {
  IREE_NET_BOOTSTRAP_HEADER_TYPE_OFFSET = 0,
  IREE_NET_BOOTSTRAP_HEADER_VERSION_OFFSET = 1,
  IREE_NET_BOOTSTRAP_HEADER_FLAGS_OFFSET = 2,
  IREE_NET_BOOTSTRAP_HEADER_TOTAL_SIZE_OFFSET = 4,

  IREE_NET_BOOTSTRAP_PEER_CAPABILITIES_OFFSET = 8,
  IREE_NET_BOOTSTRAP_PEER_APPLICATION_ENDPOINT_COUNT_OFFSET = 12,
  IREE_NET_BOOTSTRAP_PEER_AXIS_COUNT_OFFSET = 16,
  IREE_NET_BOOTSTRAP_PEER_APPLICATION_DATA_LENGTH_OFFSET = 20,
  IREE_NET_BOOTSTRAP_PEER_MACHINE_INDEX_OFFSET = 24,
  IREE_NET_BOOTSTRAP_PEER_SESSION_EPOCH_OFFSET = 25,
  IREE_NET_BOOTSTRAP_PEER_RESERVED_0_OFFSET = 26,
  IREE_NET_BOOTSTRAP_PEER_RESERVED_1_OFFSET = 28,

  IREE_NET_BOOTSTRAP_AXIS_OFFSET = 0,
  IREE_NET_BOOTSTRAP_AXIS_CURRENT_EPOCH_OFFSET = 8,

  IREE_NET_BOOTSTRAP_REJECT_STATUS_CODE_OFFSET = 8,
  IREE_NET_BOOTSTRAP_REJECT_RESERVED_OFFSET = 9,
  IREE_NET_BOOTSTRAP_REJECT_REASON_LENGTH_OFFSET = 12,
};

typedef struct iree_net_bootstrap_layout_t {
  iree_host_size_t variable_data_offset;
  iree_host_size_t variable_data_length;
  iree_host_size_t total_size;
} iree_net_bootstrap_layout_t;

static bool iree_net_bootstrap_try_calculate_layout(
    iree_host_size_t fixed_size, iree_host_size_t prefix_count,
    iree_host_size_t prefix_element_size, iree_host_size_t variable_data_length,
    iree_net_bootstrap_layout_t* out_layout) {
  memset(out_layout, 0, sizeof(*out_layout));

  iree_host_size_t prefix_size = 0;
  iree_host_size_t variable_data_offset = 0;
  iree_host_size_t unaligned_total_size = 0;
  iree_host_size_t total_size = 0;
  if (!iree_host_size_checked_mul(prefix_count, prefix_element_size,
                                  &prefix_size) ||
      !iree_host_size_checked_add(fixed_size, prefix_size,
                                  &variable_data_offset) ||
      !iree_host_size_checked_add(variable_data_offset, variable_data_length,
                                  &unaligned_total_size) ||
      !iree_host_size_checked_align(
          unaligned_total_size, IREE_NET_BOOTSTRAP_ALIGNMENT, &total_size) ||
      total_size > UINT32_MAX) {
    return false;
  }

  out_layout->variable_data_offset = variable_data_offset;
  out_layout->variable_data_length = variable_data_length;
  out_layout->total_size = total_size;
  return true;
}

static iree_status_t iree_net_bootstrap_validate_capabilities(
    iree_net_bootstrap_capabilities_t capabilities) {
  const iree_net_bootstrap_capabilities_t unrecognized_capabilities =
      capabilities & ~IREE_NET_BOOTSTRAP_CAPABILITY_ALL_RECOGNIZED;
  if (IREE_UNLIKELY(unrecognized_capabilities != 0)) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "bootstrap capabilities contain unrecognized bits 0x%08" PRIx32,
        unrecognized_capabilities);
  }
  return iree_ok_status();
}

static iree_status_t iree_net_bootstrap_validate_axis(iree_async_axis_t axis,
                                                      uint8_t machine_index,
                                                      uint8_t session_epoch,
                                                      uint32_t index) {
  const iree_async_causal_domain_t domain = iree_async_axis_domain(axis);
  switch (domain) {
    case IREE_ASYNC_CAUSAL_DOMAIN_QUEUE:
    case IREE_ASYNC_CAUSAL_DOMAIN_COLLECTIVE:
    case IREE_ASYNC_CAUSAL_DOMAIN_EPILOGUE:
    case IREE_ASYNC_CAUSAL_DOMAIN_HOST:
      break;
    default:
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "bootstrap axis %" PRIu32
                              " has unrecognized causal domain %u",
                              index, (unsigned)domain);
  }
  if (IREE_UNLIKELY(iree_async_axis_machine(axis) != machine_index)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "bootstrap axis %" PRIu32
                            " machine index %u does not match peer %u",
                            index, (unsigned)iree_async_axis_machine(axis),
                            (unsigned)machine_index);
  }
  if (IREE_UNLIKELY(iree_async_axis_session(axis) != session_epoch)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "bootstrap axis %" PRIu32
                            " session epoch %u does not match peer %u",
                            index, (unsigned)iree_async_axis_session(axis),
                            (unsigned)session_epoch);
  }
  return iree_ok_status();
}

static iree_status_t iree_net_bootstrap_calculate_peer_layout(
    const iree_net_bootstrap_peer_info_t* peer,
    iree_net_bootstrap_layout_t* out_layout) {
  IREE_RETURN_IF_ERROR(
      iree_net_bootstrap_validate_capabilities(peer->capabilities));
  if (IREE_UNLIKELY(peer->axis_count > 0 && !peer->axes)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "bootstrap axes are required when axis_count > 0");
  }
  if (IREE_UNLIKELY(peer->application_data.data_length > UINT32_MAX)) {
    return iree_make_status(
        IREE_STATUS_OUT_OF_RANGE,
        "bootstrap application data length exceeds UINT32_MAX");
  }
  if (IREE_UNLIKELY(peer->application_data.data_length > 0 &&
                    !peer->application_data.data)) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "bootstrap application data is required when its length is nonzero");
  }
  if (IREE_UNLIKELY(!iree_net_bootstrap_try_calculate_layout(
          IREE_NET_BOOTSTRAP_PEER_INFO_SIZE, peer->axis_count,
          IREE_NET_BOOTSTRAP_AXIS_ENTRY_SIZE,
          peer->application_data.data_length, out_layout))) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "bootstrap peer information exceeds UINT32_MAX");
  }
  for (uint32_t i = 0; i < peer->axis_count; ++i) {
    IREE_RETURN_IF_ERROR(iree_net_bootstrap_validate_axis(
        peer->axes[i].axis, peer->machine_index, peer->session_epoch, i));
  }
  return iree_ok_status();
}

static iree_status_t iree_net_bootstrap_calculate_reject_layout(
    const iree_net_bootstrap_reject_t* reject,
    iree_net_bootstrap_layout_t* out_layout) {
  const uint32_t status_code = (uint32_t)reject->status_code;
  if (IREE_UNLIKELY(status_code == IREE_STATUS_OK ||
                    status_code > IREE_STATUS_CODE_MASK)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "bootstrap rejection status code %u is invalid",
                            status_code);
  }
  if (IREE_UNLIKELY(reject->reason.size > UINT32_MAX)) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "bootstrap rejection reason exceeds UINT32_MAX");
  }
  if (IREE_UNLIKELY(reject->reason.size > 0 && !reject->reason.data)) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "bootstrap rejection reason is required when its length is nonzero");
  }
  if (IREE_UNLIKELY(reject->reason.size > 0 &&
                    memchr(reject->reason.data, 0, reject->reason.size))) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "bootstrap rejection reason contains a NUL byte");
  }
  if (IREE_UNLIKELY(!iree_net_bootstrap_try_calculate_layout(
          IREE_NET_BOOTSTRAP_REJECT_SIZE, 0, 0, reject->reason.size,
          out_layout))) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "bootstrap rejection exceeds UINT32_MAX");
  }
  return iree_ok_status();
}

static iree_status_t iree_net_bootstrap_message_calculate_layout(
    const iree_net_bootstrap_message_t* message,
    iree_net_bootstrap_layout_t* out_layout) {
  memset(out_layout, 0, sizeof(*out_layout));
  if (IREE_UNLIKELY(!message)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "bootstrap message is required");
  }
  switch (message->type) {
    case IREE_NET_BOOTSTRAP_TYPE_HELLO:
      return iree_net_bootstrap_calculate_peer_layout(&message->value.hello,
                                                      out_layout);
    case IREE_NET_BOOTSTRAP_TYPE_HELLO_ACK:
      return iree_net_bootstrap_calculate_peer_layout(&message->value.hello_ack,
                                                      out_layout);
    case IREE_NET_BOOTSTRAP_TYPE_REJECT:
      return iree_net_bootstrap_calculate_reject_layout(&message->value.reject,
                                                        out_layout);
    default:
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "bootstrap message type %u is invalid",
                              (unsigned)message->type);
  }
}

iree_async_frontier_entry_t iree_net_bootstrap_axis_list_get(
    const iree_net_bootstrap_axis_list_t* axis_list, uint32_t index) {
  IREE_ASSERT_ARGUMENT(axis_list);
  IREE_ASSERT(index < axis_list->count);
  const uint8_t* entry =
      axis_list->encoded_entries.data +
      (iree_host_size_t)index * IREE_NET_BOOTSTRAP_AXIS_ENTRY_SIZE;
  iree_async_frontier_entry_t result = {
      .axis =
          iree_unaligned_load_le_u64(entry + IREE_NET_BOOTSTRAP_AXIS_OFFSET),
      .epoch = iree_unaligned_load_le_u64(
          entry + IREE_NET_BOOTSTRAP_AXIS_CURRENT_EPOCH_OFFSET),
  };
  return result;
}

iree_status_t iree_net_bootstrap_message_calculate_size(
    const iree_net_bootstrap_message_t* message, iree_host_size_t* out_size) {
  if (IREE_UNLIKELY(!out_size)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "out_size is required");
  }
  *out_size = 0;

  iree_net_bootstrap_layout_t layout;
  IREE_RETURN_IF_ERROR(
      iree_net_bootstrap_message_calculate_layout(message, &layout));
  *out_size = layout.total_size;
  return iree_ok_status();
}

static void iree_net_bootstrap_serialize_header(iree_net_bootstrap_type_t type,
                                                iree_host_size_t total_size,
                                                uint8_t* output) {
  output[IREE_NET_BOOTSTRAP_HEADER_TYPE_OFFSET] = (uint8_t)type;
  output[IREE_NET_BOOTSTRAP_HEADER_VERSION_OFFSET] =
      IREE_NET_BOOTSTRAP_PROTOCOL_VERSION;
  iree_unaligned_store_le_u32(
      output + IREE_NET_BOOTSTRAP_HEADER_TOTAL_SIZE_OFFSET,
      (uint32_t)total_size);
}

static void iree_net_bootstrap_serialize_peer(
    const iree_net_bootstrap_peer_info_t* peer,
    const iree_net_bootstrap_layout_t* layout, uint8_t* output) {
  iree_unaligned_store_le_u32(
      output + IREE_NET_BOOTSTRAP_PEER_CAPABILITIES_OFFSET, peer->capabilities);
  iree_unaligned_store_le_u32(
      output + IREE_NET_BOOTSTRAP_PEER_APPLICATION_ENDPOINT_COUNT_OFFSET,
      peer->application_endpoint_count);
  iree_unaligned_store_le_u32(
      output + IREE_NET_BOOTSTRAP_PEER_AXIS_COUNT_OFFSET, peer->axis_count);
  iree_unaligned_store_le_u32(
      output + IREE_NET_BOOTSTRAP_PEER_APPLICATION_DATA_LENGTH_OFFSET,
      (uint32_t)peer->application_data.data_length);
  output[IREE_NET_BOOTSTRAP_PEER_MACHINE_INDEX_OFFSET] = peer->machine_index;
  output[IREE_NET_BOOTSTRAP_PEER_SESSION_EPOCH_OFFSET] = peer->session_epoch;

  uint8_t* encoded_axis = output + IREE_NET_BOOTSTRAP_PEER_INFO_SIZE;
  for (uint32_t i = 0; i < peer->axis_count; ++i) {
    iree_unaligned_store_le_u64(encoded_axis + IREE_NET_BOOTSTRAP_AXIS_OFFSET,
                                peer->axes[i].axis);
    iree_unaligned_store_le_u64(
        encoded_axis + IREE_NET_BOOTSTRAP_AXIS_CURRENT_EPOCH_OFFSET,
        peer->axes[i].epoch);
    encoded_axis += IREE_NET_BOOTSTRAP_AXIS_ENTRY_SIZE;
  }
  if (peer->application_data.data_length > 0) {
    memcpy(output + layout->variable_data_offset, peer->application_data.data,
           peer->application_data.data_length);
  }
}

static void iree_net_bootstrap_serialize_reject(
    const iree_net_bootstrap_reject_t* reject,
    const iree_net_bootstrap_layout_t* layout, uint8_t* output) {
  output[IREE_NET_BOOTSTRAP_REJECT_STATUS_CODE_OFFSET] =
      (uint8_t)reject->status_code;
  iree_unaligned_store_le_u32(
      output + IREE_NET_BOOTSTRAP_REJECT_REASON_LENGTH_OFFSET,
      (uint32_t)reject->reason.size);
  if (reject->reason.size > 0) {
    memcpy(output + layout->variable_data_offset, reject->reason.data,
           reject->reason.size);
  }
}

iree_status_t iree_net_bootstrap_message_serialize(
    const iree_net_bootstrap_message_t* message, iree_byte_span_t buffer) {
  iree_net_bootstrap_layout_t layout;
  IREE_RETURN_IF_ERROR(
      iree_net_bootstrap_message_calculate_layout(message, &layout));
  if (IREE_UNLIKELY(!buffer.data)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "bootstrap output buffer is required");
  }
  if (IREE_UNLIKELY(buffer.data_length < layout.total_size)) {
    return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                            "bootstrap output buffer is too small: %" PRIhsz
                            " < %" PRIhsz,
                            buffer.data_length, layout.total_size);
  }

  memset(buffer.data, 0, layout.total_size);
  iree_net_bootstrap_serialize_header(message->type, layout.total_size,
                                      buffer.data);
  switch (message->type) {
    case IREE_NET_BOOTSTRAP_TYPE_HELLO:
      iree_net_bootstrap_serialize_peer(&message->value.hello, &layout,
                                        buffer.data);
      break;
    case IREE_NET_BOOTSTRAP_TYPE_HELLO_ACK:
      iree_net_bootstrap_serialize_peer(&message->value.hello_ack, &layout,
                                        buffer.data);
      break;
    case IREE_NET_BOOTSTRAP_TYPE_REJECT:
      iree_net_bootstrap_serialize_reject(&message->value.reject, &layout,
                                          buffer.data);
      break;
    default:
      IREE_ASSERT_UNREACHABLE("bootstrap message type must be validated");
      break;
  }
  return iree_ok_status();
}

static iree_status_t iree_net_bootstrap_validate_zero_padding(
    iree_const_byte_span_t data, const iree_net_bootstrap_layout_t* layout) {
  const iree_host_size_t padding_offset =
      layout->variable_data_offset + layout->variable_data_length;
  for (iree_host_size_t i = padding_offset; i < layout->total_size; ++i) {
    if (IREE_UNLIKELY(data.data[i] != 0)) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "bootstrap alignment padding byte %" PRIhsz
                              " is nonzero",
                              i - padding_offset);
    }
  }
  return iree_ok_status();
}

static iree_status_t iree_net_bootstrap_parse_peer(
    iree_const_byte_span_t data,
    iree_net_bootstrap_peer_info_view_t* out_peer) {
  memset(out_peer, 0, sizeof(*out_peer));
  if (IREE_UNLIKELY(data.data_length < IREE_NET_BOOTSTRAP_PEER_INFO_SIZE)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "bootstrap peer information is truncated");
  }

  const iree_net_bootstrap_capabilities_t capabilities =
      iree_unaligned_load_le_u32(data.data +
                                 IREE_NET_BOOTSTRAP_PEER_CAPABILITIES_OFFSET);
  IREE_RETURN_IF_ERROR(iree_net_bootstrap_validate_capabilities(capabilities));
  const uint32_t application_endpoint_count = iree_unaligned_load_le_u32(
      data.data + IREE_NET_BOOTSTRAP_PEER_APPLICATION_ENDPOINT_COUNT_OFFSET);
  const uint32_t axis_count = iree_unaligned_load_le_u32(
      data.data + IREE_NET_BOOTSTRAP_PEER_AXIS_COUNT_OFFSET);
  const uint32_t application_data_length = iree_unaligned_load_le_u32(
      data.data + IREE_NET_BOOTSTRAP_PEER_APPLICATION_DATA_LENGTH_OFFSET);
  const uint8_t machine_index =
      data.data[IREE_NET_BOOTSTRAP_PEER_MACHINE_INDEX_OFFSET];
  const uint8_t session_epoch =
      data.data[IREE_NET_BOOTSTRAP_PEER_SESSION_EPOCH_OFFSET];
  const uint16_t reserved_0 = iree_unaligned_load_le_u16(
      data.data + IREE_NET_BOOTSTRAP_PEER_RESERVED_0_OFFSET);
  const uint32_t reserved_1 = iree_unaligned_load_le_u32(
      data.data + IREE_NET_BOOTSTRAP_PEER_RESERVED_1_OFFSET);
  if (IREE_UNLIKELY(reserved_0 != 0 || reserved_1 != 0)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "bootstrap peer reserved fields are nonzero");
  }

  iree_net_bootstrap_layout_t layout;
  if (IREE_UNLIKELY(!iree_net_bootstrap_try_calculate_layout(
          IREE_NET_BOOTSTRAP_PEER_INFO_SIZE, axis_count,
          IREE_NET_BOOTSTRAP_AXIS_ENTRY_SIZE, application_data_length,
          &layout))) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "bootstrap peer information layout overflows");
  }
  if (IREE_UNLIKELY(layout.total_size != data.data_length)) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "bootstrap peer information size does not match its message: %" PRIhsz
        " != %" PRIhsz,
        layout.total_size, data.data_length);
  }

  iree_net_bootstrap_axis_list_t axes = {
      .encoded_entries = iree_make_const_byte_span(
          data.data + IREE_NET_BOOTSTRAP_PEER_INFO_SIZE,
          layout.variable_data_offset - IREE_NET_BOOTSTRAP_PEER_INFO_SIZE),
      .count = axis_count,
  };
  for (uint32_t i = 0; i < axis_count; ++i) {
    const iree_async_frontier_entry_t entry =
        iree_net_bootstrap_axis_list_get(&axes, i);
    IREE_RETURN_IF_ERROR(iree_net_bootstrap_validate_axis(
        entry.axis, machine_index, session_epoch, i));
  }
  IREE_RETURN_IF_ERROR(iree_net_bootstrap_validate_zero_padding(data, &layout));

  out_peer->capabilities = capabilities;
  out_peer->application_endpoint_count = application_endpoint_count;
  out_peer->axes = axes;
  out_peer->application_data = iree_make_const_byte_span(
      data.data + layout.variable_data_offset, application_data_length);
  out_peer->machine_index = machine_index;
  out_peer->session_epoch = session_epoch;
  return iree_ok_status();
}

static iree_status_t iree_net_bootstrap_parse_reject(
    iree_const_byte_span_t data, iree_net_bootstrap_reject_view_t* out_reject) {
  memset(out_reject, 0, sizeof(*out_reject));
  if (IREE_UNLIKELY(data.data_length < IREE_NET_BOOTSTRAP_REJECT_SIZE)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "bootstrap rejection is truncated");
  }

  const uint8_t status_code =
      data.data[IREE_NET_BOOTSTRAP_REJECT_STATUS_CODE_OFFSET];
  if (IREE_UNLIKELY(status_code == IREE_STATUS_OK ||
                    status_code > IREE_STATUS_CODE_MASK)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "bootstrap rejection status code %u is invalid",
                            status_code);
  }
  if (IREE_UNLIKELY(
          data.data[IREE_NET_BOOTSTRAP_REJECT_RESERVED_OFFSET] != 0 ||
          data.data[IREE_NET_BOOTSTRAP_REJECT_RESERVED_OFFSET + 1] != 0 ||
          data.data[IREE_NET_BOOTSTRAP_REJECT_RESERVED_OFFSET + 2] != 0)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "bootstrap rejection reserved fields are nonzero");
  }
  const uint32_t reason_length = iree_unaligned_load_le_u32(
      data.data + IREE_NET_BOOTSTRAP_REJECT_REASON_LENGTH_OFFSET);

  iree_net_bootstrap_layout_t layout;
  if (IREE_UNLIKELY(!iree_net_bootstrap_try_calculate_layout(
          IREE_NET_BOOTSTRAP_REJECT_SIZE, 0, 0, reason_length, &layout))) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "bootstrap rejection layout overflows");
  }
  if (IREE_UNLIKELY(layout.total_size != data.data_length)) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "bootstrap rejection size does not match its message: %" PRIhsz
        " != %" PRIhsz,
        layout.total_size, data.data_length);
  }
  const char* reason = (const char*)data.data + layout.variable_data_offset;
  if (IREE_UNLIKELY(reason_length > 0 && memchr(reason, 0, reason_length))) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "bootstrap rejection reason contains a NUL byte");
  }
  IREE_RETURN_IF_ERROR(iree_net_bootstrap_validate_zero_padding(data, &layout));

  out_reject->status_code = (iree_status_code_t)status_code;
  out_reject->reason = iree_make_string_view(reason, reason_length);
  return iree_ok_status();
}

iree_status_t iree_net_bootstrap_message_parse(
    iree_const_byte_span_t data,
    iree_net_bootstrap_message_view_t* out_message) {
  if (IREE_UNLIKELY(!out_message)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "out_message is required");
  }
  memset(out_message, 0, sizeof(*out_message));
  if (IREE_UNLIKELY(data.data_length < IREE_NET_BOOTSTRAP_HEADER_SIZE)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "bootstrap message header is truncated");
  }
  if (IREE_UNLIKELY(!data.data)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "bootstrap message data is required");
  }

  const uint8_t type = data.data[IREE_NET_BOOTSTRAP_HEADER_TYPE_OFFSET];
  const uint8_t version = data.data[IREE_NET_BOOTSTRAP_HEADER_VERSION_OFFSET];
  if (IREE_UNLIKELY(version != IREE_NET_BOOTSTRAP_PROTOCOL_VERSION)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "unsupported bootstrap protocol version %u",
                            version);
  }
  const uint16_t flags = iree_unaligned_load_le_u16(
      data.data + IREE_NET_BOOTSTRAP_HEADER_FLAGS_OFFSET);
  if (IREE_UNLIKELY(flags != 0)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "bootstrap header flags are nonzero");
  }
  const uint32_t total_size = iree_unaligned_load_le_u32(
      data.data + IREE_NET_BOOTSTRAP_HEADER_TOTAL_SIZE_OFFSET);
  if (IREE_UNLIKELY(total_size != data.data_length)) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "bootstrap message size does not match its buffer: %" PRIu32
        " != %" PRIhsz,
        total_size, data.data_length);
  }
  if (IREE_UNLIKELY(total_size % IREE_NET_BOOTSTRAP_ALIGNMENT != 0)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "bootstrap message size is not 8-byte aligned");
  }

  iree_net_bootstrap_message_view_t message;
  memset(&message, 0, sizeof(message));
  iree_status_t status = iree_ok_status();
  switch ((iree_net_bootstrap_type_t)type) {
    case IREE_NET_BOOTSTRAP_TYPE_HELLO:
      status = iree_net_bootstrap_parse_peer(data, &message.value.hello);
      break;
    case IREE_NET_BOOTSTRAP_TYPE_HELLO_ACK:
      status = iree_net_bootstrap_parse_peer(data, &message.value.hello_ack);
      break;
    case IREE_NET_BOOTSTRAP_TYPE_REJECT:
      status = iree_net_bootstrap_parse_reject(data, &message.value.reject);
      break;
    default:
      status =
          iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                           "bootstrap message type %u is unrecognized", type);
      break;
  }
  if (iree_status_is_ok(status)) {
    message.type = (iree_net_bootstrap_type_t)type;
    *out_message = message;
  }
  return status;
}
