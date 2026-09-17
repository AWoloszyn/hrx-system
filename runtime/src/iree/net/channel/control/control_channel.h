// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Application-neutral control messages over a message endpoint.
//
// A control channel adds an 8-byte typed envelope to complete endpoint
// messages and dispatches DATA, GOAWAY, and ERROR semantics. It deliberately
// owns no endpoint or session lifecycle state: the enclosing session controls
// admission, graceful draining, and endpoint deactivation.
//
// ## Wire format
//
// All integers are little-endian. Each message begins with:
//
//   byte 0:    version (currently 1)
//   byte 1:    type
//   byte 2:    per-type flags
//   byte 3:    reserved (zero)
//   bytes 4-7: per-type value
//
// DATA carries application-defined flags, a zero value, and an opaque payload.
// GOAWAY carries no flags or payload and stores its reason code in value. ERROR
// carries no flags or value and its payload is an iree/net/status_wire value.
//
// ## Ownership
//
// The channel borrows its endpoint. Allocate it before protocol handoff, call
// attach before endpoint activation or from the final bootstrap message
// callback, and free it only after endpoint deactivation has completed. Attach
// does not activate the endpoint, and free does not detach callbacks.

#ifndef IREE_NET_CHANNEL_CONTROL_CONTROL_CHANNEL_H_
#define IREE_NET_CHANNEL_CONTROL_CONTROL_CHANNEL_H_

#include "iree/async/api.h"
#include "iree/base/api.h"
#include "iree/net/message_endpoint.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

#define IREE_NET_CONTROL_MESSAGE_VERSION 1u
#define IREE_NET_CONTROL_MESSAGE_HEADER_SIZE 8u

// Control message type values encoded on the wire.
typedef enum iree_net_control_message_type_e {
  // Peer-initiated graceful shutdown.
  IREE_NET_CONTROL_MESSAGE_TYPE_GOAWAY = 0x03,
  // Terminal peer error encoded with iree/net/status_wire.
  IREE_NET_CONTROL_MESSAGE_TYPE_ERROR = 0x04,
  // Opaque application data.
  IREE_NET_CONTROL_MESSAGE_TYPE_DATA = 0x80,
} iree_net_control_message_type_t;

// Application-defined DATA flag bits.
typedef enum iree_net_control_data_flag_bits_e {
  IREE_NET_CONTROL_DATA_FLAG_NONE = 0u,
} iree_net_control_data_flag_bits_t;
typedef uint32_t iree_net_control_data_flags_t;

typedef struct iree_net_control_channel_t iree_net_control_channel_t;

// Handles one received DATA message.
//
// |payload| excludes the control header. |lease| is the endpoint-provided
// backing storage lease. To retain the payload after this callback, move the
// lease by copying it and clearing the callback's lease value.
//
// Returning a non-OK status terminates the endpoint and delivers that status
// through |on_error|.
typedef iree_status_t(IREE_API_PTR* iree_net_control_channel_data_fn_t)(
    void* user_data, iree_net_control_data_flags_t flags,
    iree_const_byte_span_t payload, iree_async_buffer_lease_t* lease);

// Handles peer-initiated graceful shutdown.
//
// The enclosing session owns the resulting admission and drain transition.
typedef void(IREE_API_PTR* iree_net_control_channel_goaway_fn_t)(
    void* user_data, uint32_t reason_code);

// Handles the endpoint's one terminal error.
//
// Malformed input, DATA callback failure, a peer ERROR message, and transport
// failure all converge here. Status ownership transfers to the callback.
typedef void(IREE_API_PTR* iree_net_control_channel_error_fn_t)(
    void* user_data, iree_status_t status);

// Application callbacks installed on a control channel.
typedef struct iree_net_control_channel_callbacks_t {
  // Required callback for DATA messages.
  iree_net_control_channel_data_fn_t on_data;
  // Required callback for peer GOAWAY messages.
  iree_net_control_channel_goaway_fn_t on_goaway;
  // Required callback for the terminal endpoint error.
  iree_net_control_channel_error_fn_t on_error;
  // Opaque pointer passed to every callback.
  void* user_data;
} iree_net_control_channel_callbacks_t;

// Allocates a control channel over |endpoint| without changing its callbacks.
//
// The endpoint and callback user data must outlive the channel. All callbacks
// are required. Call iree_net_control_channel_attach to perform protocol
// handoff.
IREE_API_EXPORT iree_status_t iree_net_control_channel_allocate(
    iree_net_message_endpoint_t endpoint,
    iree_net_control_channel_callbacks_t callbacks,
    iree_allocator_t host_allocator, iree_net_control_channel_t** out_channel);

// Frees a control channel after its endpoint has fully deactivated.
//
// This does not operate on the borrowed endpoint or clear its callbacks.
IREE_API_EXPORT void iree_net_control_channel_free(
    iree_net_control_channel_t* channel);

// Installs the control channel callback bundle on its borrowed endpoint.
//
// Call before endpoint activation or from inside the final bootstrap message
// callback. Serialized endpoint delivery guarantees all later messages use the
// control channel callbacks.
IREE_API_EXPORT void iree_net_control_channel_attach(
    iree_net_control_channel_t* channel);

// Sends DATA while borrowing |payload| through terminal completion.
//
// The 8-byte control header is captured before this call returns. Payload span
// storage remains caller-owned until |completion_callback| fires. An OK return
// guarantees exactly one completion; a non-OK return guarantees none.
IREE_API_EXPORT iree_status_t iree_net_control_channel_send_data(
    iree_net_control_channel_t* channel, iree_net_control_data_flags_t flags,
    iree_async_span_list_t payload,
    iree_net_send_completion_callback_t completion_callback);

// Sends DATA after copying all payload bytes before this call returns.
//
// Payload spans must be CPU-accessible. The channel writes directly into an
// exact endpoint reservation and introduces no payload-size limit. The caller
// must serialize the reservation/commit window against endpoint deactivation,
// as required by the message endpoint contract.
IREE_API_EXPORT iree_status_t iree_net_control_channel_send_data_copy(
    iree_net_control_channel_t* channel, iree_net_control_data_flags_t flags,
    iree_async_span_list_t payload,
    iree_net_send_completion_callback_t completion_callback);

// Sends a header-only GOAWAY message.
//
// A rejected send does not publish the message or invoke the completion. The
// enclosing session should transition to draining only after an OK return.
IREE_API_EXPORT iree_status_t iree_net_control_channel_send_goaway(
    iree_net_control_channel_t* channel, uint32_t reason_code,
    iree_net_send_completion_callback_t completion_callback);

// Sends a terminal ERROR message encoded with iree/net/status_wire.
//
// Takes ownership of |error_status| and consumes it on every return path. The
// caller must serialize the reservation/commit window against endpoint
// deactivation. An OK return guarantees one terminal send completion.
IREE_API_EXPORT iree_status_t iree_net_control_channel_send_error(
    iree_net_control_channel_t* channel, iree_status_t error_status,
    iree_net_send_completion_callback_t completion_callback);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_NET_CHANNEL_CONTROL_CONTROL_CHANNEL_H_
