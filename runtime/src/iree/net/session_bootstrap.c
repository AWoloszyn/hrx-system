// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/session_bootstrap.h"

#include <string.h>

#define IREE_NET_SESSION_REJECT_DIAGNOSTIC_LIMIT 1024

static iree_status_t iree_net_session_bootstrap_validate_peer(
    const iree_net_session_bootstrap_t* bootstrap,
    const iree_net_bootstrap_peer_info_view_t* peer,
    iree_net_bootstrap_capabilities_t* out_negotiated_capabilities) {
  if (peer->application_endpoint_count !=
      bootstrap->application_endpoint_count) {
    return iree_make_status(
        IREE_STATUS_FAILED_PRECONDITION,
        "peer requires %" PRIu32
        " application endpoints but the local session requires %" PRIu32,
        peer->application_endpoint_count,
        bootstrap->application_endpoint_count);
  }

  const iree_net_bootstrap_capabilities_t negotiated_capabilities =
      peer->capabilities & bootstrap->local_capabilities;
  const iree_net_bootstrap_capabilities_t missing_capabilities =
      bootstrap->required_capabilities & ~negotiated_capabilities;
  if (missing_capabilities != IREE_NET_BOOTSTRAP_CAPABILITY_NONE) {
    return iree_make_status(IREE_STATUS_UNAVAILABLE,
                            "required session capabilities were not "
                            "negotiated: required=0x%08" PRIx32
                            " negotiated=0x%08" PRIx32 " missing=0x%08" PRIx32,
                            bootstrap->required_capabilities,
                            negotiated_capabilities, missing_capabilities);
  }

  *out_negotiated_capabilities = negotiated_capabilities;
  return iree_ok_status();
}

iree_status_t iree_net_session_bootstrap_initialize(
    iree_net_session_bootstrap_role_t role,
    const iree_net_bootstrap_peer_info_t* local_peer,
    iree_net_bootstrap_capabilities_t required_capabilities,
    iree_allocator_t host_allocator,
    iree_net_session_bootstrap_t* out_bootstrap) {
  if (!out_bootstrap) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "bootstrap output is required");
  }
  memset(out_bootstrap, 0, sizeof(*out_bootstrap));
  if (!local_peer) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "local peer information is required");
  }
  if (role != IREE_NET_SESSION_BOOTSTRAP_ROLE_CLIENT &&
      role != IREE_NET_SESSION_BOOTSTRAP_ROLE_SERVER) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "session bootstrap role %d is invalid", (int)role);
  }
  if ((required_capabilities & ~local_peer->capabilities) != 0) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "required capabilities must be locally advertised: "
                            "advertised=0x%08" PRIx32 " required=0x%08" PRIx32,
                            local_peer->capabilities, required_capabilities);
  }
  if (local_peer->application_endpoint_count == UINT32_MAX) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "application endpoint count leaves no control endpoint ordinal");
  }

  iree_net_bootstrap_message_t message;
  memset(&message, 0, sizeof(message));
  if (role == IREE_NET_SESSION_BOOTSTRAP_ROLE_CLIENT) {
    message.type = IREE_NET_BOOTSTRAP_TYPE_HELLO;
    message.value.hello = *local_peer;
  } else {
    message.type = IREE_NET_BOOTSTRAP_TYPE_HELLO_ACK;
    message.value.hello_ack = *local_peer;
  }

  iree_host_size_t message_size = 0;
  IREE_RETURN_IF_ERROR(
      iree_net_bootstrap_message_calculate_size(&message, &message_size));
  uint8_t* message_data = NULL;
  IREE_RETURN_IF_ERROR(iree_allocator_malloc(host_allocator, message_size,
                                             (void**)&message_data));
  iree_status_t status = iree_net_bootstrap_message_serialize(
      &message, iree_make_byte_span(message_data, message_size));
  if (!iree_status_is_ok(status)) {
    iree_allocator_free(host_allocator, message_data);
    return status;
  }

  out_bootstrap->phase =
      role == IREE_NET_SESSION_BOOTSTRAP_ROLE_CLIENT
          ? IREE_NET_SESSION_BOOTSTRAP_PHASE_CLIENT_SEND_HELLO
          : IREE_NET_SESSION_BOOTSTRAP_PHASE_SERVER_WAIT_HELLO;
  out_bootstrap->local_capabilities = local_peer->capabilities;
  out_bootstrap->required_capabilities = required_capabilities;
  out_bootstrap->application_endpoint_count =
      local_peer->application_endpoint_count;
  out_bootstrap->outbound_message =
      iree_make_byte_span(message_data, message_size);
  out_bootstrap->host_allocator = host_allocator;
  return iree_ok_status();
}

void iree_net_session_bootstrap_deinitialize(
    iree_net_session_bootstrap_t* bootstrap) {
  if (!bootstrap) {
    return;
  }
  iree_allocator_free(bootstrap->host_allocator,
                      bootstrap->outbound_message.data);
  memset(bootstrap, 0, sizeof(*bootstrap));
}

iree_const_byte_span_t iree_net_session_bootstrap_outbound_message(
    const iree_net_session_bootstrap_t* bootstrap) {
  IREE_ASSERT(
      bootstrap->phase == IREE_NET_SESSION_BOOTSTRAP_PHASE_CLIENT_SEND_HELLO ||
      bootstrap->phase == IREE_NET_SESSION_BOOTSTRAP_PHASE_SERVER_SEND_ACK);
  IREE_ASSERT(bootstrap->outbound_message.data);
  return iree_make_const_byte_span(bootstrap->outbound_message.data,
                                   bootstrap->outbound_message.data_length);
}

void iree_net_session_bootstrap_consume_outbound_message(
    iree_net_session_bootstrap_t* bootstrap) {
  IREE_ASSERT(bootstrap->outbound_message.data);
  if (bootstrap->phase == IREE_NET_SESSION_BOOTSTRAP_PHASE_CLIENT_SEND_HELLO) {
    bootstrap->phase = IREE_NET_SESSION_BOOTSTRAP_PHASE_CLIENT_WAIT_ACK;
  } else {
    IREE_ASSERT(bootstrap->phase ==
                IREE_NET_SESSION_BOOTSTRAP_PHASE_SERVER_SEND_ACK);
    bootstrap->phase = IREE_NET_SESSION_BOOTSTRAP_PHASE_COMPLETE;
  }
  iree_allocator_free(bootstrap->host_allocator,
                      bootstrap->outbound_message.data);
  bootstrap->outbound_message = iree_byte_span_empty();
}

iree_status_t iree_net_session_bootstrap_process_message(
    iree_net_session_bootstrap_t* bootstrap, iree_const_byte_span_t message,
    iree_net_bootstrap_peer_info_view_t* out_peer,
    iree_net_bootstrap_capabilities_t* out_negotiated_capabilities) {
  if (!out_peer || !out_negotiated_capabilities) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "bootstrap result outputs are required");
  }
  memset(out_peer, 0, sizeof(*out_peer));
  *out_negotiated_capabilities = IREE_NET_BOOTSTRAP_CAPABILITY_NONE;

  iree_net_bootstrap_message_view_t parsed_message;
  memset(&parsed_message, 0, sizeof(parsed_message));
  IREE_RETURN_IF_ERROR(
      iree_net_bootstrap_message_parse(message, &parsed_message));

  const iree_net_bootstrap_peer_info_view_t* peer = NULL;
  switch (bootstrap->phase) {
    case IREE_NET_SESSION_BOOTSTRAP_PHASE_CLIENT_WAIT_ACK:
      if (parsed_message.type == IREE_NET_BOOTSTRAP_TYPE_REJECT) {
        const iree_net_bootstrap_reject_view_t* reject =
            &parsed_message.value.reject;
        const int reason_length = (int)iree_min(
            reject->reason.size,
            (iree_host_size_t)IREE_NET_SESSION_REJECT_DIAGNOSTIC_LIMIT);
        const char* reason_data =
            reject->reason.data ? reject->reason.data : "";
        return iree_make_status(reject->status_code,
                                "remote session rejected bootstrap: %.*s",
                                reason_length, reason_data);
      }
      if (parsed_message.type != IREE_NET_BOOTSTRAP_TYPE_HELLO_ACK) {
        return iree_make_status(
            IREE_STATUS_FAILED_PRECONDITION,
            "client expected HELLO_ACK or REJECT but received type %u",
            (unsigned)parsed_message.type);
      }
      peer = &parsed_message.value.hello_ack;
      break;
    case IREE_NET_SESSION_BOOTSTRAP_PHASE_SERVER_WAIT_HELLO:
      if (parsed_message.type != IREE_NET_BOOTSTRAP_TYPE_HELLO) {
        return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                                "server expected HELLO but received type %u",
                                (unsigned)parsed_message.type);
      }
      peer = &parsed_message.value.hello;
      break;
    default:
      return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                              "bootstrap phase %d does not accept messages",
                              (int)bootstrap->phase);
  }

  iree_net_bootstrap_capabilities_t negotiated_capabilities =
      IREE_NET_BOOTSTRAP_CAPABILITY_NONE;
  IREE_RETURN_IF_ERROR(iree_net_session_bootstrap_validate_peer(
      bootstrap, peer, &negotiated_capabilities));

  *out_peer = *peer;
  *out_negotiated_capabilities = negotiated_capabilities;
  if (bootstrap->phase == IREE_NET_SESSION_BOOTSTRAP_PHASE_CLIENT_WAIT_ACK) {
    bootstrap->phase = IREE_NET_SESSION_BOOTSTRAP_PHASE_COMPLETE;
  } else {
    bootstrap->phase = IREE_NET_SESSION_BOOTSTRAP_PHASE_SERVER_SEND_ACK;
  }
  return iree_ok_status();
}
