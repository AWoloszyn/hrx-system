// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Session bootstrap state and peer-policy validation.
//
// This component owns one creation-time encoding of the local HELLO or
// HELLO_ACK and validates the corresponding peer message. It has no transport
// or session-lifecycle behavior. Both peers advertise their local capabilities
// and independently compute the same negotiated intersection.

#ifndef IREE_NET_SESSION_BOOTSTRAP_H_
#define IREE_NET_SESSION_BOOTSTRAP_H_

#include "iree/base/api.h"
#include "iree/net/bootstrap.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Role selecting the bootstrap message order.
typedef enum iree_net_session_bootstrap_role_e {
  // Sends HELLO and then receives HELLO_ACK or REJECT.
  IREE_NET_SESSION_BOOTSTRAP_ROLE_CLIENT = 0,
  // Receives HELLO and then sends HELLO_ACK or REJECT.
  IREE_NET_SESSION_BOOTSTRAP_ROLE_SERVER = 1,
} iree_net_session_bootstrap_role_t;

// Bootstrap protocol phase.
typedef enum iree_net_session_bootstrap_phase_e {
  // Client has an encoded HELLO ready for send admission.
  IREE_NET_SESSION_BOOTSTRAP_PHASE_CLIENT_SEND_HELLO = 0,
  // Client is waiting for HELLO_ACK or REJECT.
  IREE_NET_SESSION_BOOTSTRAP_PHASE_CLIENT_WAIT_ACK = 1,
  // Server is waiting for HELLO.
  IREE_NET_SESSION_BOOTSTRAP_PHASE_SERVER_WAIT_HELLO = 2,
  // Server has an encoded HELLO_ACK ready for send admission.
  IREE_NET_SESSION_BOOTSTRAP_PHASE_SERVER_SEND_ACK = 3,
  // Bootstrap completed successfully.
  IREE_NET_SESSION_BOOTSTRAP_PHASE_COMPLETE = 4,
} iree_net_session_bootstrap_phase_t;

// Bootstrap state embedded in a session.
typedef struct iree_net_session_bootstrap_t {
  // Current role-specific protocol phase.
  iree_net_session_bootstrap_phase_t phase;
  // Capabilities advertised by the local peer.
  iree_net_bootstrap_capabilities_t local_capabilities;
  // Capabilities required in the negotiated intersection.
  iree_net_bootstrap_capabilities_t required_capabilities;
  // Exact application endpoint count required from the peer.
  uint32_t application_endpoint_count;
  // Owned encoded HELLO or HELLO_ACK awaiting send admission.
  iree_byte_span_t outbound_message;
  // Host allocator owning |outbound_message|.
  iree_allocator_t host_allocator;
} iree_net_session_bootstrap_t;

// Initializes bootstrap state and serializes the complete local peer message.
//
// All storage referenced by |local_peer| is needed only for this call. The
// encoded message remains owned by |out_bootstrap| until it is consumed or the
// state is deinitialized.
IREE_API_EXPORT iree_status_t iree_net_session_bootstrap_initialize(
    iree_net_session_bootstrap_role_t role,
    const iree_net_bootstrap_peer_info_t* local_peer,
    iree_net_bootstrap_capabilities_t required_capabilities,
    iree_allocator_t host_allocator,
    iree_net_session_bootstrap_t* out_bootstrap);

// Releases any encoded outbound message still owned by |bootstrap|.
IREE_API_EXPORT void iree_net_session_bootstrap_deinitialize(
    iree_net_session_bootstrap_t* bootstrap);

// Returns the encoded message awaiting send admission.
//
// Valid only in CLIENT_SEND_HELLO or SERVER_SEND_ACK. The returned storage is
// owned by |bootstrap| and remains valid until consume_outbound_message.
IREE_API_EXPORT iree_const_byte_span_t
iree_net_session_bootstrap_outbound_message(
    const iree_net_session_bootstrap_t* bootstrap);

// Consumes an outbound message after its send has been accepted.
//
// The endpoint generated-prefix contract guarantees that the encoded storage
// is no longer needed when send returns OK. Advances the client to WAIT_ACK or
// the server to COMPLETE.
IREE_API_EXPORT void iree_net_session_bootstrap_consume_outbound_message(
    iree_net_session_bootstrap_t* bootstrap);

// Parses and validates the peer message expected in the current phase.
//
// On success, |out_peer| borrows |message| and |out_negotiated_capabilities|
// contains the intersection of both advertised capability sets. A server
// advances to SERVER_SEND_ACK; a client advances to COMPLETE. On failure both
// outputs are zeroed and the phase is unchanged.
IREE_API_EXPORT iree_status_t iree_net_session_bootstrap_process_message(
    iree_net_session_bootstrap_t* bootstrap, iree_const_byte_span_t message,
    iree_net_bootstrap_peer_info_view_t* out_peer,
    iree_net_bootstrap_capabilities_t* out_negotiated_capabilities);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_NET_SESSION_BOOTSTRAP_H_
