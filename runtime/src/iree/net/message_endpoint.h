// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Message endpoint: unified interface for sending and receiving discrete
// messages.
//
// This abstraction sits between carriers and message protocols. A
// connection-facing endpoint exposes payload messages and hides any transport
// framing needed to preserve their boundaries. Transport-internal endpoints
// may instead use complete wire frames as their endpoint-defined messages.
//
// ## Borrowed View Semantics
//
// IMPORTANT: Message endpoints are BORROWED VIEWS, not owned objects. Callers
// never free an endpoint directly. The endpoint is valid only while the
// underlying endpoint implementation is alive.
//
// Conversion functions like framing_adapter_as_endpoint() return stack-copyable
// structs that point into the underlying object. When that object is freed,
// all endpoint references become invalid.
//
// ## Protocol Handoff
//
// During connection bootstrap, ownership of an endpoint transfers from the
// bootstrap handler to the operational protocol. Use set_callbacks() to
// atomically swap both message and error handlers. Message callbacks for one
// endpoint are serialized in delivery order. A swap performed from inside a
// message callback therefore takes effect before any later message callback,
// ensuring no later message is delivered to the stale handler.

#ifndef IREE_NET_MESSAGE_ENDPOINT_H_
#define IREE_NET_MESSAGE_ENDPOINT_H_

#include "iree/async/api.h"
#include "iree/base/api.h"
#include "iree/net/carrier.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

//===----------------------------------------------------------------------===//
// Callbacks and parameters
//===----------------------------------------------------------------------===//

// Message handler invoked when a complete message is received.
//
// Called on the proactor thread for each complete message. Calls for one
// endpoint are serialized in delivery order and never overlap; one call returns
// before the next begins. This serialization applies only to message callbacks;
// terminal-error and send-completion callbacks remain independently
// asynchronous.
//
// The handler receives a view of the message data and a lease to the backing
// storage. The lease is always valid (non-NULL) whether the message came from a
// recv buffer or was reassembled from fragments.
//
// To keep the message data valid beyond the callback, move the lease by copying
// it and clearing the callback's lease value. Release the moved lease when
// done.
//
// Return iree_ok_status() to continue receiving. Returning an error triggers
// the endpoint's error handler and may cause deactivation.
typedef iree_status_t(IREE_API_PTR* iree_net_message_endpoint_message_fn_t)(
    void* user_data, iree_const_byte_span_t message,
    iree_async_buffer_lease_t* lease);

// Error handler invoked for the first terminal endpoint error.
//
// Called on the proactor thread after receive progress has stopped. Subsequent
// operations fail with the recorded terminal status. Status ownership is
// transferred to the handler, which must propagate or release it. Outstanding
// sends retain their own completion ownership and may finish afterward.
typedef void(IREE_API_PTR* iree_net_message_endpoint_error_fn_t)(
    void* user_data, iree_status_t status);

// Deactivation complete callback invoked when graceful shutdown finishes.
//
// After this callback fires, the endpoint is fully drained and operations will
// fail. The underlying object can now be safely freed.
typedef void(IREE_API_PTR* iree_net_message_endpoint_deactivate_fn_t)(
    void* user_data);

// Bundled message and error handlers for atomic handoff.
//
// During protocol transitions (e.g., bootstrap to operational), callbacks must
// change atomically to prevent messages from being delivered to a stale
// handler. The shared user_data ensures consistency across the bundle.
typedef struct iree_net_message_endpoint_callbacks_t {
  // Function invoked for each complete received message.
  iree_net_message_endpoint_message_fn_t on_message;

  // Function invoked when the endpoint reports a transport error.
  iree_net_message_endpoint_error_fn_t on_error;

  // Opaque user data passed to both callbacks in this bundle.
  void* user_data;
} iree_net_message_endpoint_callbacks_t;

// Parameters for send operations.
typedef struct iree_net_message_endpoint_send_params_t {
  // Transient leading bytes copied before the send call returns.
  iree_const_byte_span_t copied_prefix;

  // Scatter-gather message data borrowed through terminal completion.
  iree_async_span_list_t data;

  // Required callback invoked when the send completes.
  iree_net_send_completion_callback_t completion_callback;
} iree_net_message_endpoint_send_params_t;

//===----------------------------------------------------------------------===//
// iree_net_message_endpoint_t
//===----------------------------------------------------------------------===//

typedef struct iree_net_message_endpoint_vtable_t
    iree_net_message_endpoint_vtable_t;

// A borrowed view into a message-oriented transport endpoint.
//
// This is a lightweight handle (two pointers) that can be copied by value.
// The endpoint is valid only while the underlying object is alive. There is
// no retain/release - when the underlying object is freed, the endpoint
// becomes invalid.
typedef struct iree_net_message_endpoint_t {
  // Opaque endpoint implementation pointer passed to vtable methods.
  void* self;
  // Vtable implementing endpoint operations.
  const iree_net_message_endpoint_vtable_t* vtable;
} iree_net_message_endpoint_t;

struct iree_net_message_endpoint_vtable_t {
  // Atomically replaces the message and terminal-error callback bundle.
  void (*set_callbacks)(void* self,
                        iree_net_message_endpoint_callbacks_t callbacks);
  // Activates receive progress for the endpoint.
  iree_status_t (*activate)(void* self);
  // Begins asynchronous endpoint deactivation.
  iree_status_t (*deactivate)(
      void* self, iree_net_message_endpoint_deactivate_fn_t callback,
      void* user_data);
  // Submits one callback-completed message send.
  iree_status_t (*send)(void* self,
                        const iree_net_message_endpoint_send_params_t* params);
  // Queries current message send admission capacity.
  iree_net_carrier_send_budget_t (*query_send_budget)(void* self);

  // Direct-write send mode: caller writes into transport buffer.
  iree_status_t (*begin_send)(void* self, iree_host_size_t size, void** out_ptr,
                              iree_net_carrier_send_handle_t* out_handle);
  iree_status_t (*commit_send)(
      void* self, iree_net_carrier_send_handle_t handle,
      iree_net_send_completion_callback_t completion_callback);
  void (*abort_send)(void* self, iree_net_carrier_send_handle_t handle);
};

// Sets message and error handlers atomically.
//
// Used for protocol handoff (e.g., bootstrap completes, operational channel
// takes over). Both handlers and user_data change in a single operation. When
// called from an on_message handler, all later messages use the new bundle.
//
// Must be called on the proactor thread after activation, or from any thread
// before activation.
static inline void iree_net_message_endpoint_set_callbacks(
    iree_net_message_endpoint_t endpoint,
    iree_net_message_endpoint_callbacks_t callbacks) {
  endpoint.vtable->set_callbacks(endpoint.self, callbacks);
}

// Activates the endpoint, enabling message receipt.
//
// After activation, the endpoint auto-receives and delivers complete messages
// to the on_message handler. Both on_message and on_error must be set first.
//
// Returns IREE_STATUS_FAILED_PRECONDITION if callbacks are not set.
static inline iree_status_t iree_net_message_endpoint_activate(
    iree_net_message_endpoint_t endpoint) {
  return endpoint.vtable->activate(endpoint.self);
}

// Begins graceful deactivation of the endpoint.
//
// This drains outstanding operations and stops receiving new messages. The
// optional callback fires when deactivation completes and the endpoint is safe
// to abandon. After the callback, operations on this endpoint will fail.
//
// Deactivation does not block waiting for pending work. If no work remains,
// the callback may fire synchronously from this call and invalidate the
// endpoint owner before the call returns.
//
// An OK return accepts the drain, which then completes even if transport
// cleanup fails; cleanup failures use the endpoint error handler. A non-OK
// return rejects the request and does not invoke the callback.
static inline iree_status_t iree_net_message_endpoint_deactivate(
    iree_net_message_endpoint_t endpoint,
    iree_net_message_endpoint_deactivate_fn_t callback, void* user_data) {
  return endpoint.vtable->deactivate(endpoint.self, callback, user_data);
}

// Sends a message via the endpoint.
//
// |params->copied_prefix| followed by |params->data| comprises one
// endpoint-defined message. The prefix may reference transient storage and is
// captured before this call returns. Data buffers remain caller-owned until the
// completion callback fires. Either part may be empty, but the complete message
// must contain at least one byte.
//
// Connection-facing endpoints preserve the message boundary while hiding any
// transport framing they add. Lower-level transport endpoints may define a
// message as a complete wire frame and send the bytes unchanged.
//
// An OK return guarantees exactly one terminal completion through
// |params->completion_callback|. The callback may race with the return on
// another proactor thread. Its byte count covers the complete logical message,
// including the copied prefix. A non-OK return means the callback will not
// fire.
//
// The prefix and span descriptor array are needed only for the duration of this
// call. The data buffers must remain valid until the completion callback fires.
static inline iree_status_t iree_net_message_endpoint_send(
    iree_net_message_endpoint_t endpoint,
    const iree_net_message_endpoint_send_params_t* params) {
  if (!params) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "send parameters are required");
  }
  if (!params->completion_callback.fn) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "send completion callback is required");
  }
  if (params->copied_prefix.data_length > 0 && !params->copied_prefix.data) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "copied send prefix has null storage");
  }
  if (params->data.count > 0 && !params->data.values) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "send span list has null storage");
  }
  iree_host_size_t total_length = params->copied_prefix.data_length;
  for (iree_host_size_t i = 0; i < params->data.count; ++i) {
    if (!iree_host_size_checked_add(total_length, params->data.values[i].length,
                                    &total_length)) {
      return iree_make_status(IREE_STATUS_OUT_OF_RANGE,
                              "send payload length overflow");
    }
  }
  if (total_length == 0) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "send payload must be non-empty");
  }
  return endpoint.vtable->send(endpoint.self, params);
}

// Queries send budget for backpressure management.
//
// Returns both byte budget and operation slot budget. When either reaches
// zero, the endpoint is backpressured. A later send completion indicates that
// callers may query the budget and attempt admission again. This is an
// advisory snapshot; send remains the authoritative admission check.
static inline iree_net_carrier_send_budget_t
iree_net_message_endpoint_query_send_budget(
    iree_net_message_endpoint_t endpoint) {
  return endpoint.vtable->query_send_budget(endpoint.self);
}

// Reserves space for a contiguous send of |size| bytes.
//
// On success, |*out_ptr| points to a buffer of at least |size| bytes where the
// caller writes directly. |*out_handle| receives an opaque handle that must be
// passed to either commit_send (to publish the data) or abort_send (to discard
// the reservation). |*out_ptr| is aligned to
// IREE_NET_SEND_RESERVATION_ALIGNMENT.
//
// The caller always writes exactly |size| bytes of endpoint-defined message
// data. Implementations may reserve additional hidden transport framing and
// return a pointer offset past it.
//
// Between begin_send and commit/abort, the caller holds endpoint-specific
// resources. The caller must call commit_send or abort_send promptly.
// Deactivation invalidates uncommitted reservations. Callers must externally
// synchronize writes through reservation pointers and terminal commit/abort
// operations against endpoint or owning-connection deactivation.
//
// |size| must be > 0.
//
// Returns RESOURCE_EXHAUSTED if the transport buffer is full.
// Returns FAILED_PRECONDITION if the endpoint is not activated.
static inline iree_status_t iree_net_message_endpoint_begin_send(
    iree_net_message_endpoint_t endpoint, iree_host_size_t size, void** out_ptr,
    iree_net_carrier_send_handle_t* out_handle) {
  if (!out_ptr || !out_handle) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "send reservation requires output storage");
  }
  *out_ptr = NULL;
  *out_handle = 0;
  if (size == 0) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "send reservation size must be nonzero");
  }
  return endpoint.vtable->begin_send(endpoint.self, size, out_ptr, out_handle);
}

// Publishes a previously reserved send, making the data visible to the peer.
//
// The data written into the buffer returned by begin_send is committed to the
// transport. This call always consumes |handle| unless it rejects a missing
// completion callback. After an OK return the callback fires exactly once when
// the send reaches a terminal state and transport resources are reusable. The
// callback may race with the return on another proactor thread. A non-OK return
// means the callback will not fire.
static inline iree_status_t iree_net_message_endpoint_commit_send(
    iree_net_message_endpoint_t endpoint, iree_net_carrier_send_handle_t handle,
    iree_net_send_completion_callback_t completion_callback) {
  if (!completion_callback.fn) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "send completion callback is required");
  }
  return endpoint.vtable->commit_send(endpoint.self, handle,
                                      completion_callback);
}

// Discards a previously reserved send without publishing any data.
//
// The reserved resources are released. No data is sent to the peer.
static inline void iree_net_message_endpoint_abort_send(
    iree_net_message_endpoint_t endpoint,
    iree_net_carrier_send_handle_t handle) {
  endpoint.vtable->abort_send(endpoint.self, handle);
}

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_NET_MESSAGE_ENDPOINT_H_
