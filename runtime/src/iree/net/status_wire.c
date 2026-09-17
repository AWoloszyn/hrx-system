// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/status_wire.h"

#include <limits.h>
#include <string.h>

#include "iree/base/alignment.h"

#if !defined(IREE_ENDIANNESS_LITTLE) || !IREE_ENDIANNESS_LITTLE
#error "IREE networking status serialization requires little-endian hosts"
#endif  // !IREE_ENDIANNESS_LITTLE

enum {
  IREE_NET_STATUS_WIRE_HEADER_VERSION_OFFSET = 0,
  IREE_NET_STATUS_WIRE_HEADER_STATUS_CODE_OFFSET = 1,
  IREE_NET_STATUS_WIRE_HEADER_ENTRY_COUNT_OFFSET = 2,
  IREE_NET_STATUS_WIRE_HEADER_TOTAL_SIZE_OFFSET = 4,
  IREE_NET_STATUS_WIRE_ENTRY_TYPE_OFFSET = 0,
  IREE_NET_STATUS_WIRE_ENTRY_FLAGS_OFFSET = 1,
  IREE_NET_STATUS_WIRE_ENTRY_RESERVED_0_OFFSET = 2,
  IREE_NET_STATUS_WIRE_ENTRY_TEXT_LENGTH_OFFSET = 4,
  IREE_NET_STATUS_WIRE_ENTRY_AUX_OFFSET = 8,
  IREE_NET_STATUS_WIRE_ENTRY_RESERVED_1_OFFSET = 12,
};

typedef struct iree_net_status_wire_layout_t {
  iree_host_size_t total_size;
  uint16_t entry_count;
} iree_net_status_wire_layout_t;

static iree_status_t iree_net_status_wire_calculate_entry_layout(
    iree_host_size_t text_length, iree_host_size_t* out_storage_length,
    iree_host_size_t* out_entry_size) {
  *out_storage_length = 0;
  *out_entry_size = 0;
  if (IREE_UNLIKELY(text_length == 0)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "status wire entries must not be empty");
  }
  if (IREE_UNLIKELY(text_length > UINT32_MAX)) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "status wire entry length exceeds UINT32_MAX");
  }

  iree_host_size_t terminated_length = 0;
  iree_host_size_t storage_length = 0;
  iree_host_size_t entry_size = 0;
  if (IREE_UNLIKELY(
          !iree_host_size_checked_add(text_length, 1, &terminated_length) ||
          !iree_host_size_checked_align(terminated_length,
                                        IREE_NET_STATUS_WIRE_ALIGNMENT,
                                        &storage_length) ||
          !iree_host_size_checked_add(IREE_NET_STATUS_WIRE_ENTRY_HEADER_SIZE,
                                      storage_length, &entry_size))) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "status wire entry layout overflow");
  }

  *out_storage_length = storage_length;
  *out_entry_size = entry_size;
  return iree_ok_status();
}

static iree_status_t iree_net_status_wire_layout_add_entry(
    iree_host_size_t text_length, iree_net_status_wire_layout_t* layout) {
  if (IREE_UNLIKELY(layout->entry_count == UINT16_MAX)) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "status wire entry count exceeds UINT16_MAX");
  }

  iree_host_size_t storage_length = 0;
  iree_host_size_t entry_size = 0;
  IREE_RETURN_IF_ERROR(iree_net_status_wire_calculate_entry_layout(
      text_length, &storage_length, &entry_size));
  (void)storage_length;

  iree_host_size_t total_size = 0;
  if (IREE_UNLIKELY(!iree_host_size_checked_add(layout->total_size, entry_size,
                                                &total_size) ||
                    total_size > UINT32_MAX)) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "serialized status exceeds UINT32_MAX bytes");
  }
  layout->total_size = total_size;
  ++layout->entry_count;
  return iree_ok_status();
}

static iree_status_t iree_net_status_wire_size_payload(
    void* user_data, const iree_status_payload_t* payload) {
  iree_host_size_t text_length = 0;
  iree_status_payload_format(payload, 0, NULL, &text_length);
  if (text_length == 0) {
    return iree_ok_status();
  }
  if (IREE_UNLIKELY(text_length > INT_MAX)) {
    return iree_make_status(
        IREE_STATUS_OUT_OF_RANGE,
        "formatted status diagnostic exceeds INT_MAX bytes");
  }
  return iree_net_status_wire_layout_add_entry(
      text_length, (iree_net_status_wire_layout_t*)user_data);
}

iree_status_t iree_net_status_wire_calculate_size(const iree_status_t status,
                                                  iree_host_size_t* out_size) {
  if (IREE_UNLIKELY(!out_size)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "out_size is required");
  }
  *out_size = 0;

  iree_net_status_wire_layout_t layout = {
      .total_size = IREE_NET_STATUS_WIRE_HEADER_SIZE,
      .entry_count = 0,
  };
  if (!iree_status_is_ok(status)) {
    iree_status_source_location_t source_location =
        iree_status_source_location(status);
    if (source_location.file && source_location.file[0]) {
      IREE_RETURN_IF_ERROR(iree_net_status_wire_layout_add_entry(
          strlen(source_location.file), &layout));
    }

    iree_string_view_t message = iree_status_message(status);
    if (!iree_string_view_is_empty(message)) {
      IREE_RETURN_IF_ERROR(
          iree_net_status_wire_layout_add_entry(message.size, &layout));
    }

    IREE_RETURN_IF_ERROR(iree_status_enumerate_payloads(
        status, iree_net_status_wire_size_payload, &layout));
  }

  *out_size = layout.total_size;
  return iree_ok_status();
}

typedef struct iree_net_status_wire_serialize_context_t {
  uint8_t* data;
  iree_host_size_t capacity;
  iree_host_size_t offset;
  uint16_t entry_count;
} iree_net_status_wire_serialize_context_t;

static iree_status_t iree_net_status_wire_begin_entry(
    iree_net_status_wire_entry_type_t type, uint32_t aux,
    iree_host_size_t text_length,
    iree_net_status_wire_serialize_context_t* context, uint8_t** out_text,
    iree_host_size_t* out_storage_length) {
  *out_text = NULL;
  *out_storage_length = 0;
  if (IREE_UNLIKELY(context->entry_count == UINT16_MAX)) {
    return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                            "status wire entry count exceeds UINT16_MAX");
  }

  iree_host_size_t storage_length = 0;
  iree_host_size_t entry_size = 0;
  IREE_RETURN_IF_ERROR(iree_net_status_wire_calculate_entry_layout(
      text_length, &storage_length, &entry_size));
  if (IREE_UNLIKELY(entry_size > context->capacity - context->offset)) {
    return iree_make_status(IREE_STATUS_DATA_LOSS,
                            "status changed while it was being serialized");
  }

  uint8_t* entry = context->data + context->offset;
  entry[IREE_NET_STATUS_WIRE_ENTRY_TYPE_OFFSET] = (uint8_t)type;
  iree_unaligned_store_le_u32(
      entry + IREE_NET_STATUS_WIRE_ENTRY_TEXT_LENGTH_OFFSET,
      (uint32_t)text_length);
  iree_unaligned_store_le_u32(entry + IREE_NET_STATUS_WIRE_ENTRY_AUX_OFFSET,
                              aux);
  *out_text = entry + IREE_NET_STATUS_WIRE_ENTRY_HEADER_SIZE;
  *out_storage_length = storage_length;
  return iree_ok_status();
}

static void iree_net_status_wire_commit_entry(
    iree_host_size_t storage_length,
    iree_net_status_wire_serialize_context_t* context) {
  context->offset += IREE_NET_STATUS_WIRE_ENTRY_HEADER_SIZE + storage_length;
  ++context->entry_count;
}

static iree_status_t iree_net_status_wire_write_text_entry(
    iree_net_status_wire_entry_type_t type, uint32_t aux,
    iree_string_view_t text,
    iree_net_status_wire_serialize_context_t* context) {
  if (IREE_UNLIKELY(memchr(text.data, 0, text.size) != NULL)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "status wire text contains an embedded NUL");
  }

  uint8_t* output_text = NULL;
  iree_host_size_t storage_length = 0;
  IREE_RETURN_IF_ERROR(iree_net_status_wire_begin_entry(
      type, aux, text.size, context, &output_text, &storage_length));
  memcpy(output_text, text.data, text.size);
  iree_net_status_wire_commit_entry(storage_length, context);
  return iree_ok_status();
}

static iree_status_t iree_net_status_wire_serialize_payload(
    void* user_data, const iree_status_payload_t* payload) {
  iree_host_size_t text_length = 0;
  iree_status_payload_format(payload, 0, NULL, &text_length);
  if (text_length == 0) {
    return iree_ok_status();
  }

  iree_net_status_wire_entry_type_t type =
      iree_status_payload_type(payload) == IREE_STATUS_PAYLOAD_TYPE_STACK_TRACE
          ? IREE_NET_STATUS_WIRE_ENTRY_TYPE_STACK_TRACE
          : IREE_NET_STATUS_WIRE_ENTRY_TYPE_ANNOTATION;
  iree_net_status_wire_serialize_context_t* context =
      (iree_net_status_wire_serialize_context_t*)user_data;
  uint8_t* output_text = NULL;
  iree_host_size_t storage_length = 0;
  IREE_RETURN_IF_ERROR(iree_net_status_wire_begin_entry(
      type, 0, text_length, context, &output_text, &storage_length));

  iree_host_size_t written_length = 0;
  iree_status_payload_format(payload, storage_length, (char*)output_text,
                             &written_length);
  if (IREE_UNLIKELY(written_length != text_length)) {
    return iree_make_status(
        IREE_STATUS_DATA_LOSS,
        "status payload length changed while serializing: %" PRIhsz
        " != %" PRIhsz,
        written_length, text_length);
  }
  if (IREE_UNLIKELY(memchr(output_text, 0, text_length) != NULL)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "status payload contains an embedded NUL");
  }
  memset(output_text + text_length, 0, storage_length - text_length);

  iree_net_status_wire_commit_entry(storage_length, context);
  return iree_ok_status();
}

iree_status_t iree_net_status_wire_serialize(const iree_status_t status,
                                             iree_byte_span_t buffer) {
  iree_host_size_t required_size = 0;
  IREE_RETURN_IF_ERROR(
      iree_net_status_wire_calculate_size(status, &required_size));
  if (IREE_UNLIKELY(!buffer.data)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "status wire output buffer is required");
  }
  if (IREE_UNLIKELY(buffer.data_length < required_size)) {
    return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                            "status wire output buffer is too small: %" PRIhsz
                            " < %" PRIhsz,
                            buffer.data_length, required_size);
  }

  memset(buffer.data, 0, required_size);
  iree_net_status_wire_serialize_context_t context = {
      .data = buffer.data,
      .capacity = required_size,
      .offset = IREE_NET_STATUS_WIRE_HEADER_SIZE,
      .entry_count = 0,
  };

  if (!iree_status_is_ok(status)) {
    iree_status_source_location_t source_location =
        iree_status_source_location(status);
    if (source_location.file && source_location.file[0]) {
      IREE_RETURN_IF_ERROR(iree_net_status_wire_write_text_entry(
          IREE_NET_STATUS_WIRE_ENTRY_TYPE_SOURCE_LOCATION, source_location.line,
          iree_make_cstring_view(source_location.file), &context));
    }

    iree_string_view_t message = iree_status_message(status);
    if (!iree_string_view_is_empty(message)) {
      IREE_RETURN_IF_ERROR(iree_net_status_wire_write_text_entry(
          IREE_NET_STATUS_WIRE_ENTRY_TYPE_MESSAGE, 0, message, &context));
    }

    IREE_RETURN_IF_ERROR(iree_status_enumerate_payloads(
        status, iree_net_status_wire_serialize_payload, &context));
  }

  if (IREE_UNLIKELY(context.offset != required_size)) {
    return iree_make_status(
        IREE_STATUS_DATA_LOSS,
        "status changed while it was being serialized: %" PRIhsz " != %" PRIhsz,
        context.offset, required_size);
  }

  buffer.data[IREE_NET_STATUS_WIRE_HEADER_VERSION_OFFSET] =
      IREE_NET_STATUS_WIRE_VERSION;
  buffer.data[IREE_NET_STATUS_WIRE_HEADER_STATUS_CODE_OFFSET] =
      (uint8_t)iree_status_code(status);
  iree_unaligned_store_le_u16(
      buffer.data + IREE_NET_STATUS_WIRE_HEADER_ENTRY_COUNT_OFFSET,
      context.entry_count);
  iree_unaligned_store_le_u32(
      buffer.data + IREE_NET_STATUS_WIRE_HEADER_TOTAL_SIZE_OFFSET,
      (uint32_t)required_size);
  return iree_ok_status();
}

typedef struct iree_net_status_wire_parse_result_t {
  iree_status_code_t status_code;
  uint16_t entry_count;
  iree_string_view_t source_file;
  uint32_t source_line;
  iree_string_view_t message;
} iree_net_status_wire_parse_result_t;

static iree_status_t iree_net_status_wire_validate(
    iree_const_byte_span_t data,
    iree_net_status_wire_parse_result_t* out_result) {
  memset(out_result, 0, sizeof(*out_result));
  if (IREE_UNLIKELY(data.data_length < IREE_NET_STATUS_WIRE_HEADER_SIZE)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "status wire header is truncated");
  }
  if (IREE_UNLIKELY(!data.data)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "status wire data is required");
  }

  const uint8_t version = data.data[IREE_NET_STATUS_WIRE_HEADER_VERSION_OFFSET];
  if (IREE_UNLIKELY(version != IREE_NET_STATUS_WIRE_VERSION)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "unsupported status wire version %u", version);
  }
  const uint8_t status_code =
      data.data[IREE_NET_STATUS_WIRE_HEADER_STATUS_CODE_OFFSET];
  if (IREE_UNLIKELY(status_code > IREE_STATUS_CODE_MASK)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "invalid status code %u", status_code);
  }
  const uint16_t entry_count = iree_unaligned_load_le_u16(
      data.data + IREE_NET_STATUS_WIRE_HEADER_ENTRY_COUNT_OFFSET);
  const uint32_t total_size = iree_unaligned_load_le_u32(
      data.data + IREE_NET_STATUS_WIRE_HEADER_TOTAL_SIZE_OFFSET);
  if (IREE_UNLIKELY(total_size != data.data_length)) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "status wire size does not match its buffer: %" PRIu32 " != %" PRIhsz,
        total_size, data.data_length);
  }
  if (IREE_UNLIKELY(total_size % IREE_NET_STATUS_WIRE_ALIGNMENT != 0)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "status wire size is not 8-byte aligned");
  }
  if (IREE_UNLIKELY(status_code == IREE_STATUS_OK && entry_count != 0)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "OK status wire contains diagnostic entries");
  }

  bool has_source = false;
  bool has_message = false;
  bool has_diagnostics = false;
  iree_host_size_t offset = IREE_NET_STATUS_WIRE_HEADER_SIZE;
  for (uint16_t i = 0; i < entry_count; ++i) {
    if (IREE_UNLIKELY(offset > total_size ||
                      IREE_NET_STATUS_WIRE_ENTRY_HEADER_SIZE >
                          total_size - offset)) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "status wire entry %u header is truncated", i);
    }
    const uint8_t* entry = data.data + offset;
    const uint8_t type = entry[IREE_NET_STATUS_WIRE_ENTRY_TYPE_OFFSET];
    const uint8_t flags = entry[IREE_NET_STATUS_WIRE_ENTRY_FLAGS_OFFSET];
    const uint16_t reserved_0 = iree_unaligned_load_le_u16(
        entry + IREE_NET_STATUS_WIRE_ENTRY_RESERVED_0_OFFSET);
    const uint32_t text_length = iree_unaligned_load_le_u32(
        entry + IREE_NET_STATUS_WIRE_ENTRY_TEXT_LENGTH_OFFSET);
    const uint32_t aux = iree_unaligned_load_le_u32(
        entry + IREE_NET_STATUS_WIRE_ENTRY_AUX_OFFSET);
    const uint32_t reserved_1 = iree_unaligned_load_le_u32(
        entry + IREE_NET_STATUS_WIRE_ENTRY_RESERVED_1_OFFSET);
    if (IREE_UNLIKELY(flags != 0 || reserved_0 != 0 || reserved_1 != 0)) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "status wire entry %u has reserved bits set", i);
    }

    iree_host_size_t storage_length = 0;
    iree_host_size_t entry_size = 0;
    iree_status_t layout_status = iree_net_status_wire_calculate_entry_layout(
        text_length, &storage_length, &entry_size);
    if (!iree_status_is_ok(layout_status)) {
      return iree_status_join(
          iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                           "status wire entry %u has an invalid length", i),
          layout_status);
    }
    if (IREE_UNLIKELY(entry_size > total_size - offset)) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "status wire entry %u data is truncated", i);
    }

    const uint8_t* text = entry + IREE_NET_STATUS_WIRE_ENTRY_HEADER_SIZE;
    if (IREE_UNLIKELY(memchr(text, 0, text_length) != NULL)) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "status wire entry %u contains an embedded NUL",
                              i);
    }
    for (iree_host_size_t j = text_length; j < storage_length; ++j) {
      if (IREE_UNLIKELY(text[j] != 0)) {
        return iree_make_status(
            IREE_STATUS_INVALID_ARGUMENT,
            "status wire entry %u has nonzero terminator or padding", i);
      }
    }

    iree_string_view_t text_view =
        iree_make_string_view((const char*)text, text_length);
    switch ((iree_net_status_wire_entry_type_t)type) {
      case IREE_NET_STATUS_WIRE_ENTRY_TYPE_SOURCE_LOCATION:
        if (IREE_UNLIKELY(has_source || has_message || has_diagnostics)) {
          return iree_make_status(
              IREE_STATUS_INVALID_ARGUMENT,
              "status wire entry %u has a misplaced or duplicate source", i);
        }
        has_source = true;
        out_result->source_file = text_view;
        out_result->source_line = aux;
        break;
      case IREE_NET_STATUS_WIRE_ENTRY_TYPE_MESSAGE:
        if (IREE_UNLIKELY(aux != 0 || has_message || has_diagnostics)) {
          return iree_make_status(
              IREE_STATUS_INVALID_ARGUMENT,
              "status wire entry %u has an invalid or duplicate message", i);
        }
        has_message = true;
        out_result->message = text_view;
        break;
      case IREE_NET_STATUS_WIRE_ENTRY_TYPE_ANNOTATION:
      case IREE_NET_STATUS_WIRE_ENTRY_TYPE_STACK_TRACE:
        if (IREE_UNLIKELY(aux != 0 || text_length > INT_MAX)) {
          return iree_make_status(
              IREE_STATUS_INVALID_ARGUMENT,
              "status wire entry %u has an invalid diagnostic", i);
        }
        has_diagnostics = true;
        break;
      default:
        return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                                "status wire entry %u has unknown type %u", i,
                                type);
    }

    offset += entry_size;
  }
  if (IREE_UNLIKELY(offset != total_size)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "status wire has unclaimed trailing data");
  }

  out_result->status_code = (iree_status_code_t)status_code;
  out_result->entry_count = entry_count;
  return iree_ok_status();
}

iree_status_t iree_net_status_wire_deserialize(iree_const_byte_span_t data,
                                               iree_status_t* out_status) {
  if (IREE_UNLIKELY(!out_status)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "out_status is required");
  }
  *out_status = iree_ok_status();

  iree_net_status_wire_parse_result_t parse_result;
  IREE_RETURN_IF_ERROR(iree_net_status_wire_validate(data, &parse_result));
  if (parse_result.status_code == IREE_STATUS_OK) {
    return iree_ok_status();
  }

  iree_status_t result = iree_status_allocate_copy(
      parse_result.status_code, parse_result.source_file,
      parse_result.source_line, parse_result.message);
  iree_host_size_t offset = IREE_NET_STATUS_WIRE_HEADER_SIZE;
  for (uint16_t i = 0; i < parse_result.entry_count; ++i) {
    const uint8_t* entry = data.data + offset;
    const iree_net_status_wire_entry_type_t type =
        (iree_net_status_wire_entry_type_t)
            entry[IREE_NET_STATUS_WIRE_ENTRY_TYPE_OFFSET];
    const uint32_t text_length = iree_unaligned_load_le_u32(
        entry + IREE_NET_STATUS_WIRE_ENTRY_TEXT_LENGTH_OFFSET);
    const iree_host_size_t entry_size =
        IREE_NET_STATUS_WIRE_ENTRY_HEADER_SIZE +
        iree_host_align((iree_host_size_t)text_length + 1,
                        IREE_NET_STATUS_WIRE_ALIGNMENT);
    if (type == IREE_NET_STATUS_WIRE_ENTRY_TYPE_ANNOTATION ||
        type == IREE_NET_STATUS_WIRE_ENTRY_TYPE_STACK_TRACE) {
      const char* text =
          (const char*)entry + IREE_NET_STATUS_WIRE_ENTRY_HEADER_SIZE;
      result = iree_status_annotate_f(result, "%.*s", (int)text_length, text);
    }
    offset += entry_size;
  }

  *out_status = result;
  return iree_ok_status();
}
