// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Structured serialization of iree_status_t for network transport.
//
// Statuses are encoded as a fixed header followed by zero or more typed text
// entries. All integers are little-endian and all records are 8-byte aligned.
// Reserved bytes and entry padding are zero and are validated when parsing.
//
// Header (8 bytes):
//   uint8_t  version
//   uint8_t  status_code
//   uint16_t entry_count
//   uint32_t total_size
//
// Entry (16-byte header followed by aligned text storage):
//   uint8_t  type
//   uint8_t  flags (reserved, zero)
//   uint16_t reserved (zero)
//   uint32_t text_length
//   uint32_t aux (source line for SOURCE_LOCATION, zero otherwise)
//   uint32_t reserved (zero)
//   uint8_t  text[text_length]
//   uint8_t  terminator_and_padding[align8(text_length + 1) - text_length]
//
// Text entries are non-empty and contain no embedded NUL bytes. Stack traces
// and opaque status payloads are transported as formatted diagnostic text and
// reconstructed as owned annotations because iree_status_t has no public API
// for recreating arbitrary payload types.

#ifndef IREE_NET_STATUS_WIRE_H_
#define IREE_NET_STATUS_WIRE_H_

#include "iree/base/api.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

#define IREE_NET_STATUS_WIRE_VERSION 1u
#define IREE_NET_STATUS_WIRE_HEADER_SIZE 8u
#define IREE_NET_STATUS_WIRE_ENTRY_HEADER_SIZE 16u
#define IREE_NET_STATUS_WIRE_ALIGNMENT 8u

// Identifies the structured text stored in an entry.
typedef enum iree_net_status_wire_entry_type_e {
  // Source filename with the one-based source line in the auxiliary field.
  IREE_NET_STATUS_WIRE_ENTRY_TYPE_SOURCE_LOCATION = 1,
  // Primary status message.
  IREE_NET_STATUS_WIRE_ENTRY_TYPE_MESSAGE = 2,
  // Formatted annotation or opaque status payload.
  IREE_NET_STATUS_WIRE_ENTRY_TYPE_ANNOTATION = 3,
  // Formatted stack trace payload.
  IREE_NET_STATUS_WIRE_ENTRY_TYPE_STACK_TRACE = 4,
} iree_net_status_wire_entry_type_t;

// Calculates the exact number of bytes required to serialize |status|.
//
// Returns IREE_STATUS_OUT_OF_RANGE if the status cannot be represented without
// truncation. |out_size| is set to zero on failure.
IREE_API_EXPORT iree_status_t iree_net_status_wire_calculate_size(
    const iree_status_t status, iree_host_size_t* out_size);

// Serializes |status| into the beginning of |buffer|.
//
// The buffer must have at least the size returned by
// iree_net_status_wire_calculate_size. Bytes beyond the encoded status are not
// modified. The status remains owned by the caller.
IREE_API_EXPORT iree_status_t iree_net_status_wire_serialize(
    const iree_status_t status, iree_byte_span_t buffer);

// Deserializes exactly one status from |data| into owned status storage.
//
// The input must contain exactly the byte count declared by its header;
// concatenated or trailing data is rejected. On success, the caller owns
// |*out_status| and must eventually free or consume it. On failure,
// |*out_status| is set to iree_ok_status().
IREE_API_EXPORT iree_status_t iree_net_status_wire_deserialize(
    iree_const_byte_span_t data, iree_status_t* out_status);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_NET_STATUS_WIRE_H_
