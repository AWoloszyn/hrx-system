// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/format/location.h"

#include <string.h>

#include "iree/base/alignment.h"

static const uint8_t* loom_location_value_node(loom_location_value_t value,
                                               uint32_t node) {
  return value.bytes.data + LOOM_LOCATION_VALUE_HEADER_LENGTH +
         (iree_host_size_t)node * LOOM_LOCATION_VALUE_NODE_LENGTH;
}

static const uint8_t* loom_location_value_payload(loom_location_value_t value,
                                                  uint32_t node) {
  return value.bytes.data +
         iree_unaligned_load_le_u32(loom_location_value_node(value, node) + 4);
}

static iree_const_byte_span_t loom_location_value_span(
    loom_location_value_t value, const uint8_t* reference) {
  return iree_make_const_byte_span(
      value.bytes.data + iree_unaligned_load_le_u32(reference),
      iree_unaligned_load_le_u32(reference + 4));
}

static loom_location_value_range_t loom_location_value_range(
    const uint8_t* data) {
  return (loom_location_value_range_t){
      .start_line = iree_unaligned_load_le_u32(data),
      .start_column = iree_unaligned_load_le_u32(data + 4),
      .end_line = iree_unaligned_load_le_u32(data + 8),
      .end_column = iree_unaligned_load_le_u32(data + 12),
  };
}

static bool loom_location_value_has_span(iree_const_byte_span_t bytes,
                                         uint32_t table_end, uint32_t offset,
                                         uint64_t length) {
  return offset >= table_end && offset <= bytes.data_length &&
         length <= bytes.data_length - offset;
}

static bool loom_location_value_has_reference(iree_const_byte_span_t bytes,
                                              uint32_t table_end,
                                              const uint8_t* reference) {
  return loom_location_value_has_span(
      bytes, table_end, iree_unaligned_load_le_u32(reference),
      iree_unaligned_load_le_u32(reference + 4));
}

iree_status_t loom_location_value_parse(iree_const_byte_span_t bytes,
                                        loom_location_value_t* out_value) {
  *out_value = (loom_location_value_t){0};
  if (bytes.data_length < LOOM_LOCATION_VALUE_HEADER_LENGTH ||
      memcmp(bytes.data, "LLOC", 4) != 0 ||
      iree_unaligned_load_le_u32(bytes.data + 4) !=
          LOOM_LOCATION_VALUE_VERSION ||
      iree_unaligned_load_le_u32(bytes.data + 8) != bytes.data_length) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "invalid captured location header");
  }
  const uint32_t node_count = iree_unaligned_load_le_u32(bytes.data + 12);
  if (!node_count ||
      node_count > (bytes.data_length - LOOM_LOCATION_VALUE_HEADER_LENGTH) /
                       LOOM_LOCATION_VALUE_NODE_LENGTH) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "invalid captured location node table");
  }
  const uint32_t table_end = LOOM_LOCATION_VALUE_HEADER_LENGTH +
                             node_count * LOOM_LOCATION_VALUE_NODE_LENGTH;
  const loom_location_value_t value = {bytes, node_count};
  iree_status_t status = iree_ok_status();
  for (uint32_t i = 0; i < node_count && iree_status_is_ok(status); ++i) {
    const uint8_t* node = loom_location_value_node(value, i);
    const uint32_t offset = iree_unaligned_load_le_u32(node + 4);
    const uint32_t length = iree_unaligned_load_le_u32(node + 8);
    bool valid = iree_unaligned_load_le_u16(node + 2) == 0 &&
                 iree_unaligned_load_le_u32(node + 12) == 0;
    if (node[0] == LOOM_LOCATION_VALUE_UNKNOWN) {
      valid = valid && offset == 0 && length == 0;
    } else {
      valid = valid &&
              loom_location_value_has_span(bytes, table_end, offset, length);
      if (valid) {
        const uint8_t* payload = bytes.data + offset;
        switch (node[0]) {
          case LOOM_LOCATION_VALUE_FILE: {
            valid = length == LOOM_LOCATION_VALUE_FILE_LENGTH;
            if (valid) {
              valid = loom_location_value_has_reference(bytes, table_end,
                                                        payload) &&
                      loom_location_value_has_span(
                          bytes, table_end,
                          iree_unaligned_load_le_u32(payload + 24),
                          (uint64_t)iree_unaligned_load_le_u32(payload + 28) *
                              LOOM_LOCATION_VALUE_FIELD_LENGTH);
              const uint32_t text_offset =
                  iree_unaligned_load_le_u32(payload + 32);
              const uint32_t text_length =
                  iree_unaligned_load_le_u32(payload + 36);
              valid = valid && (text_offset ? loom_location_value_has_span(
                                                  bytes, table_end, text_offset,
                                                  text_length)
                                            : text_length == 0);
            }
            break;
          }
          case LOOM_LOCATION_VALUE_FUSED:
            valid = length % sizeof(uint32_t) == 0;
            for (uint32_t j = 0; j < length / sizeof(uint32_t) && valid; ++j) {
              valid = iree_unaligned_load_le_u32(payload + j * 4) < i;
            }
            break;
          case LOOM_LOCATION_VALUE_OPAQUE:
            valid =
                length == LOOM_LOCATION_VALUE_OPAQUE_LENGTH &&
                loom_location_value_has_reference(bytes, table_end, payload) &&
                loom_location_value_has_reference(bytes, table_end,
                                                  payload + 8);
            break;
          case LOOM_LOCATION_VALUE_TAGGED:
            valid = length == LOOM_LOCATION_VALUE_TAGGED_LENGTH;
            if (valid) {
              const uint32_t child = iree_unaligned_load_le_u32(payload + 4);
              valid = iree_unaligned_load_le_u32(payload) != 0 &&
                      (child == UINT32_MAX || child < i) &&
                      loom_location_value_has_reference(bytes, table_end,
                                                        payload + 8);
            }
            break;
          default:
            valid = false;
            break;
        }
      }
    }
    if (!valid) {
      status = iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                                "invalid captured location node %u", i);
    }
  }
  if (iree_status_is_ok(status)) {
    *out_value = value;
  }
  return status;
}

loom_location_value_kind_t loom_location_value_kind(loom_location_value_t value,
                                                    uint32_t node) {
  return (loom_location_value_kind_t)loom_location_value_node(value, node)[0];
}

loom_location_value_flags_t loom_location_value_flags(
    loom_location_value_t value, uint32_t node) {
  return loom_location_value_node(value, node)[1];
}

loom_location_value_file_t loom_location_value_file(loom_location_value_t value,
                                                    uint32_t node) {
  const uint8_t* payload = loom_location_value_payload(value, node);
  const iree_const_byte_span_t source =
      loom_location_value_span(value, payload);
  return (loom_location_value_file_t){
      .source =
          iree_make_string_view((const char*)source.data, source.data_length),
      .range = loom_location_value_range(payload + 8),
      .text = loom_location_value_span(value, payload + 32),
      .has_text = iree_unaligned_load_le_u32(payload + 32) != 0,
      .field_count = iree_unaligned_load_le_u32(payload + 28),
  };
}

loom_location_value_field_t loom_location_value_field(
    loom_location_value_t value, uint32_t node, uint32_t field) {
  const uint8_t* payload = loom_location_value_payload(value, node);
  const uint8_t* data =
      value.bytes.data + iree_unaligned_load_le_u32(payload + 24) +
      (iree_host_size_t)field * LOOM_LOCATION_VALUE_FIELD_LENGTH;
  return (loom_location_value_field_t){
      .kind = iree_unaligned_load_le_u32(data),
      .index = iree_unaligned_load_le_u32(data + 4),
      .range = loom_location_value_range(data + 8),
  };
}

uint32_t loom_location_value_child_count(loom_location_value_t value,
                                         uint32_t node) {
  return iree_unaligned_load_le_u32(loom_location_value_node(value, node) + 8) /
         sizeof(uint32_t);
}

uint32_t loom_location_value_child(loom_location_value_t value, uint32_t node,
                                   uint32_t child) {
  return iree_unaligned_load_le_u32(loom_location_value_payload(value, node) +
                                    (iree_host_size_t)child * sizeof(uint32_t));
}

loom_location_value_opaque_t loom_location_value_opaque(
    loom_location_value_t value, uint32_t node) {
  const uint8_t* payload = loom_location_value_payload(value, node);
  const iree_const_byte_span_t source =
      loom_location_value_span(value, payload);
  return (loom_location_value_opaque_t){
      .source =
          iree_make_string_view((const char*)source.data, source.data_length),
      .data = loom_location_value_span(value, payload + 8),
  };
}

loom_location_value_tagged_t loom_location_value_tagged(
    loom_location_value_t value, uint32_t node) {
  const uint8_t* payload = loom_location_value_payload(value, node);
  return (loom_location_value_tagged_t){
      .tag = iree_unaligned_load_le_u32(payload),
      .child = iree_unaligned_load_le_u32(payload + 4),
      .data = loom_location_value_span(value, payload + 8),
  };
}
