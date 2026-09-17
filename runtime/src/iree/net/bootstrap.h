// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Session bootstrap wire codec.
//
// Bootstrap messages establish the protocol and topology information shared by
// one connection. The codec owns only the wire representation: it does not
// negotiate policy, create synchronization objects, or retain parsed storage.
//
// All integers are little-endian and all messages are 8-byte aligned. Parsed
// axis lists and application data borrow the input message storage.
//
// Common header (8 bytes):
//   uint8_t  type
//   uint8_t  version
//   uint16_t flags (reserved, zero)
//   uint32_t total_size
//
// HELLO and HELLO_ACK peer information (32-byte fixed prefix):
//   common header
//   uint32_t capabilities
//   uint32_t application_endpoint_count
//   uint32_t axis_count
//   uint32_t application_data_length
//   uint8_t  machine_index
//   uint8_t  session_epoch
//   uint16_t reserved (zero)
//   uint32_t reserved (zero)
//   axis_entry_t axes[axis_count]
//   uint8_t application_data[application_data_length]
//   uint8_t padding[align8(application_data_length) - application_data_length]
//
// REJECT (16-byte fixed prefix):
//   common header
//   uint8_t  status_code
//   uint8_t  reserved[3] (zero)
//   uint32_t reason_length
//   uint8_t  reason[reason_length]
//   uint8_t  padding[align8(reason_length) - reason_length]

#ifndef IREE_NET_BOOTSTRAP_H_
#define IREE_NET_BOOTSTRAP_H_

#include "iree/async/frontier.h"
#include "iree/base/api.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

#define IREE_NET_BOOTSTRAP_PROTOCOL_VERSION 3u
#define IREE_NET_BOOTSTRAP_HEADER_SIZE 8u
#define IREE_NET_BOOTSTRAP_PEER_INFO_SIZE 32u
#define IREE_NET_BOOTSTRAP_REJECT_SIZE 16u
#define IREE_NET_BOOTSTRAP_AXIS_ENTRY_SIZE 16u
#define IREE_NET_BOOTSTRAP_ALIGNMENT 8u

// Identifies a session bootstrap message.
typedef enum iree_net_bootstrap_type_e {
  // Client request containing offered capabilities and local peer information.
  IREE_NET_BOOTSTRAP_TYPE_HELLO = 1,
  // Server acceptance containing negotiated capabilities and peer information.
  IREE_NET_BOOTSTRAP_TYPE_HELLO_ACK = 2,
  // Server rejection containing a stable status code and optional diagnostic.
  IREE_NET_BOOTSTRAP_TYPE_REJECT = 3,
} iree_net_bootstrap_type_t;

// Feature bits exchanged during session establishment.
typedef enum iree_net_bootstrap_capability_bits_e {
  IREE_NET_BOOTSTRAP_CAPABILITY_NONE = 0u,
  // Dedicated bulk endpoints are available for large transfers.
  IREE_NET_BOOTSTRAP_CAPABILITY_BULK_TRANSFER = 1u << 0,
  // One-sided RDMA operations are available to the application protocol.
  IREE_NET_BOOTSTRAP_CAPABILITY_RDMA = 1u << 1,
  // All capability bits recognized by this protocol version.
  IREE_NET_BOOTSTRAP_CAPABILITY_ALL_RECOGNIZED =
      IREE_NET_BOOTSTRAP_CAPABILITY_BULK_TRANSFER |
      IREE_NET_BOOTSTRAP_CAPABILITY_RDMA,
} iree_net_bootstrap_capability_bits_t;
typedef uint32_t iree_net_bootstrap_capabilities_t;

// Host-side peer information serialized in HELLO and HELLO_ACK messages.
//
// This is a value description and not a wire-layout struct. The pointed-to
// axes and application data need only remain valid for the sizing and
// serialization calls that consume them.
typedef struct iree_net_bootstrap_peer_info_t {
  // Offered capabilities in HELLO or negotiated capabilities in HELLO_ACK.
  iree_net_bootstrap_capabilities_t capabilities;
  // Number of application endpoints, excluding the bootstrap/control endpoint.
  uint32_t application_endpoint_count;
  // Array of local axes and their current epochs.
  const iree_async_frontier_entry_t* axes;
  // Number of entries in |axes|.
  uint32_t axis_count;
  // Opaque application bytes advertised with the peer information.
  iree_const_byte_span_t application_data;
  // Machine index encoded in every advertised axis.
  uint8_t machine_index;
  // Session epoch encoded in every advertised axis.
  uint8_t session_epoch;
} iree_net_bootstrap_peer_info_t;

// Host-side rejection information serialized in a REJECT message.
typedef struct iree_net_bootstrap_reject_t {
  // Stable non-OK status code describing why the session was rejected.
  iree_status_code_t status_code;
  // Optional diagnostic bytes. Embedded NUL bytes are not permitted.
  iree_string_view_t reason;
} iree_net_bootstrap_reject_t;

// Host-side value for one message to be sized or serialized.
typedef struct iree_net_bootstrap_message_t {
  // Message type selecting the active union member.
  iree_net_bootstrap_type_t type;
  // Message-specific host value.
  union {
    // Client peer information when |type| is HELLO.
    iree_net_bootstrap_peer_info_t hello;
    // Server peer information when |type| is HELLO_ACK.
    iree_net_bootstrap_peer_info_t hello_ack;
    // Rejection information when |type| is REJECT.
    iree_net_bootstrap_reject_t reject;
  } value;
} iree_net_bootstrap_message_t;

// Borrowed encoded axis entries from a structurally validated message.
typedef struct iree_net_bootstrap_axis_list_t {
  // Complete encoded entries in their original wire storage.
  iree_const_byte_span_t encoded_entries;
  // Number of fixed-size entries in |encoded_entries|.
  uint32_t count;
} iree_net_bootstrap_axis_list_t;

// Returns an aligned host value for axis entry |index|.
//
// |axis_list| must come from a successfully parsed message and |index| must be
// less than |axis_list->count|.
IREE_API_EXPORT iree_async_frontier_entry_t iree_net_bootstrap_axis_list_get(
    const iree_net_bootstrap_axis_list_t* axis_list, uint32_t index);

// Structurally validated borrowed peer information.
typedef struct iree_net_bootstrap_peer_info_view_t {
  // Offered capabilities in HELLO or negotiated capabilities in HELLO_ACK.
  iree_net_bootstrap_capabilities_t capabilities;
  // Number of application endpoints, excluding the bootstrap/control endpoint.
  uint32_t application_endpoint_count;
  // Borrowed encoded axes and current epochs.
  iree_net_bootstrap_axis_list_t axes;
  // Borrowed opaque application bytes.
  iree_const_byte_span_t application_data;
  // Machine index encoded in every advertised axis.
  uint8_t machine_index;
  // Session epoch encoded in every advertised axis.
  uint8_t session_epoch;
} iree_net_bootstrap_peer_info_view_t;

// Structurally validated borrowed rejection information.
typedef struct iree_net_bootstrap_reject_view_t {
  // Stable non-OK status code received from the peer.
  iree_status_code_t status_code;
  // Borrowed optional diagnostic bytes.
  iree_string_view_t reason;
} iree_net_bootstrap_reject_view_t;

// Structurally validated borrowed view of one complete bootstrap message.
typedef struct iree_net_bootstrap_message_view_t {
  // Parsed message type selecting the active union member.
  iree_net_bootstrap_type_t type;
  // Message-specific borrowed view.
  union {
    // Client peer information when |type| is HELLO.
    iree_net_bootstrap_peer_info_view_t hello;
    // Server peer information when |type| is HELLO_ACK.
    iree_net_bootstrap_peer_info_view_t hello_ack;
    // Rejection information when |type| is REJECT.
    iree_net_bootstrap_reject_view_t reject;
  } value;
} iree_net_bootstrap_message_view_t;

// Calculates the exact number of bytes required to serialize |message|.
//
// Returns IREE_STATUS_OUT_OF_RANGE if the message cannot be represented in the
// 32-bit wire extent without truncation. |out_size| is zero on failure.
IREE_API_EXPORT iree_status_t iree_net_bootstrap_message_calculate_size(
    const iree_net_bootstrap_message_t* message, iree_host_size_t* out_size);

// Serializes |message| into the beginning of |buffer|.
//
// The buffer must have at least the size returned by
// iree_net_bootstrap_message_calculate_size. Bytes beyond the encoded message
// are not modified. Input storage remains owned by the caller.
IREE_API_EXPORT iree_status_t iree_net_bootstrap_message_serialize(
    const iree_net_bootstrap_message_t* message, iree_byte_span_t buffer);

// Parses and structurally validates exactly one bootstrap message.
//
// Axis entries, application data, and rejection diagnostics borrow |data| and
// remain valid for the same lifetime. The output is zeroed on failure.
IREE_API_EXPORT iree_status_t iree_net_bootstrap_message_parse(
    iree_const_byte_span_t data,
    iree_net_bootstrap_message_view_t* out_message);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_NET_BOOTSTRAP_H_
