// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Credit-bounded bulk transfers over a borrowed message endpoint.
//
// A bulk channel carries large application payloads independently from queue
// and control traffic. START and COMPLETE delimit a transfer, DATA carries one
// borrowed payload extent, ABORT terminates a transfer with optional diagnostic
// bytes, and CREDIT bounds the number of DATA messages a peer may retain.
//
// ## Wire format
//
// All integers are little-endian. Each message begins with:
//
//   byte 0:      version (currently 1)
//   byte 1:      type (START, DATA, COMPLETE, ABORT, or CREDIT)
//   bytes 2-7:   reserved (must be zero)
//   bytes 8-15:  transfer ID
//   bytes 16-23: type-specific value
//
// START uses the value as the total transfer length. DATA uses it as the byte
// offset of its nonempty trailing payload. COMPLETE requires a zero value and
// no payload. ABORT requires a zero value and may carry opaque diagnostic
// bytes. CREDIT requires a zero transfer ID, uses the value as the cumulative
// DATA-message grant, and has no payload.
//
// Bulk channels require a reliable ordered endpoint. One transfer producer
// serializes its START, DATA, and COMPLETE sends; independent transfers may
// interleave. CREDIT is cumulative so duplicate or reordered older grants are
// harmless.
//
// ## Ownership
//
// The channel borrows its endpoint. Allocate it before protocol handoff, call
// attach before endpoint activation or from the final bootstrap callback, and
// free it only after endpoint deactivation has completed. The channel owns no
// endpoint lifecycle, transfer table, send tracker, or completion storage.
//
// DATA and ABORT payload spans remain caller-owned until the native endpoint
// completion callback fires. Received payloads borrow endpoint storage and may
// be retained by moving the callback lease.

#ifndef IREE_NET_CHANNEL_BULK_BULK_CHANNEL_H_
#define IREE_NET_CHANNEL_BULK_BULK_CHANNEL_H_

#include "iree/async/api.h"
#include "iree/base/api.h"
#include "iree/net/message_endpoint.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

#define IREE_NET_BULK_MESSAGE_VERSION 1u
#define IREE_NET_BULK_MESSAGE_HEADER_SIZE 24u

// Bulk message type values encoded on the wire.
typedef enum iree_net_bulk_message_type_e {
  // Announces one transfer and its total byte length.
  IREE_NET_BULK_MESSAGE_TYPE_START = 0x01,
  // Carries one nonempty extent at a byte offset in the transfer.
  IREE_NET_BULK_MESSAGE_TYPE_DATA = 0x02,
  // Marks successful completion of one transfer direction.
  IREE_NET_BULK_MESSAGE_TYPE_COMPLETE = 0x03,
  // Terminates one transfer with optional opaque diagnostic bytes.
  IREE_NET_BULK_MESSAGE_TYPE_ABORT = 0x04,
  // Advertises a cumulative DATA-message receive grant.
  IREE_NET_BULK_MESSAGE_TYPE_CREDIT = 0x05,
} iree_net_bulk_message_type_t;

typedef struct iree_net_bulk_channel_t iree_net_bulk_channel_t;

// Handles one received START message.
//
// Transfer IDs are nonzero and selected by the application protocol. The
// channel does not allocate IDs or reject duplicates.
typedef iree_status_t(IREE_API_PTR* iree_net_bulk_channel_start_fn_t)(
    void* user_data, uint64_t transfer_id, uint64_t total_length);

// Handles one received DATA message after one local credit is consumed.
//
// |payload| points into endpoint receive storage. Move |lease| by copying it
// and clearing the callback's lease value to retain the bytes after return.
// Returning a non-OK status terminates the endpoint and does not restore the
// consumed credit.
typedef iree_status_t(IREE_API_PTR* iree_net_bulk_channel_data_fn_t)(
    void* user_data, uint64_t transfer_id, uint64_t offset,
    iree_const_byte_span_t payload, iree_async_buffer_lease_t* lease);

// Handles one received COMPLETE message.
typedef iree_status_t(IREE_API_PTR* iree_net_bulk_channel_complete_fn_t)(
    void* user_data, uint64_t transfer_id);

// Handles one received ABORT message.
//
// |detail| is opaque borrowed endpoint storage and may be empty. Its lease
// contract matches DATA.
typedef iree_status_t(IREE_API_PTR* iree_net_bulk_channel_abort_fn_t)(
    void* user_data, uint64_t transfer_id, iree_const_byte_span_t detail,
    iree_async_buffer_lease_t* lease);

// Handles a newly observed cumulative peer credit grant.
//
// |credit_delta| is the increase over the previous cumulative limit and
// |available_credit_count| is an advisory snapshot after publication. DATA
// send admission remains authoritative under concurrent senders.
typedef iree_status_t(IREE_API_PTR* iree_net_bulk_channel_credit_fn_t)(
    void* user_data, uint64_t credit_delta, uint64_t available_credit_count);

// Handles the endpoint's one terminal error.
//
// Malformed input, application callback failure, and transport failure all
// converge here. Status ownership transfers to the callback.
typedef void(IREE_API_PTR* iree_net_bulk_channel_error_fn_t)(
    void* user_data, iree_status_t status);

// Application callbacks installed on a bulk channel.
typedef struct iree_net_bulk_channel_callbacks_t {
  // Required callback for START messages.
  iree_net_bulk_channel_start_fn_t on_start;
  // Required callback for DATA messages.
  iree_net_bulk_channel_data_fn_t on_data;
  // Required callback for COMPLETE messages.
  iree_net_bulk_channel_complete_fn_t on_complete;
  // Required callback for ABORT messages.
  iree_net_bulk_channel_abort_fn_t on_abort;
  // Required callback for newly observed CREDIT grants.
  iree_net_bulk_channel_credit_fn_t on_credit;
  // Required callback for the terminal endpoint error.
  iree_net_bulk_channel_error_fn_t on_error;
  // Opaque pointer passed to every callback.
  void* user_data;
} iree_net_bulk_channel_callbacks_t;

// Allocates a bulk channel over |endpoint| without changing its callbacks.
//
// The endpoint and callback user data must outlive the channel. All callbacks
// are required. Call iree_net_bulk_channel_attach to perform protocol handoff.
IREE_API_EXPORT iree_status_t iree_net_bulk_channel_allocate(
    iree_net_message_endpoint_t endpoint,
    iree_net_bulk_channel_callbacks_t callbacks,
    iree_allocator_t host_allocator, iree_net_bulk_channel_t** out_channel);

// Frees a bulk channel after its endpoint has fully deactivated.
//
// This does not operate on the borrowed endpoint or clear its callbacks.
IREE_API_EXPORT void iree_net_bulk_channel_free(
    iree_net_bulk_channel_t* channel);

// Installs the bulk channel callback bundle on its borrowed endpoint.
//
// Call before endpoint activation or from inside the final bootstrap message
// callback. Serialized endpoint delivery guarantees all later messages use the
// bulk channel callbacks.
IREE_API_EXPORT void iree_net_bulk_channel_attach(
    iree_net_bulk_channel_t* channel);

// Returns the endpoint's advisory send admission budget.
//
// A later terminal send completion is the readiness edge after transport
// backpressure. The send operation remains the authoritative admission check.
IREE_API_EXPORT iree_net_carrier_send_budget_t
iree_net_bulk_channel_query_send_budget(iree_net_bulk_channel_t* channel);

// Returns the advisory number of DATA messages admitted by peer credit.
//
// Concurrent DATA senders may consume credit after this snapshot. A zero value
// means DATA cannot be sent until a later on_credit callback.
IREE_API_EXPORT uint64_t iree_net_bulk_channel_remote_credit_count(
    const iree_net_bulk_channel_t* channel);

// Sends one START message.
//
// |transfer_id| must be nonzero. An OK return guarantees exactly one terminal
// completion; a non-OK return guarantees none.
IREE_API_EXPORT iree_status_t iree_net_bulk_channel_send_start(
    iree_net_bulk_channel_t* channel, uint64_t transfer_id,
    uint64_t total_length,
    iree_net_send_completion_callback_t completion_callback);

// Sends one credit-bounded DATA message.
//
// |transfer_id| must be nonzero and |payload| must contain at least one byte.
// The complete message may use the full 32-bit wire extent. Payload spans are
// sent without a channel copy and remain caller-owned through completion.
// Credit is consumed only after endpoint admission. If no credit remains, the
// send is accepted and its terminal completion reports RESOURCE_EXHAUSTED with
// zero transferred bytes. A synchronous non-OK return occurs before admission,
// consumes no credit, and suppresses the completion callback.
IREE_API_EXPORT iree_status_t iree_net_bulk_channel_send_data(
    iree_net_bulk_channel_t* channel, uint64_t transfer_id, uint64_t offset,
    iree_async_span_list_t payload,
    iree_net_send_completion_callback_t completion_callback);

// Sends one header-only COMPLETE message.
IREE_API_EXPORT iree_status_t iree_net_bulk_channel_send_complete(
    iree_net_bulk_channel_t* channel, uint64_t transfer_id,
    iree_net_send_completion_callback_t completion_callback);

// Sends one ABORT message with optional opaque diagnostic bytes.
//
// |detail| spans follow the same zero-copy ownership contract as DATA.
IREE_API_EXPORT iree_status_t iree_net_bulk_channel_send_abort(
    iree_net_bulk_channel_t* channel, uint64_t transfer_id,
    iree_async_span_list_t detail,
    iree_net_send_completion_callback_t completion_callback);

// Grants the peer additional DATA-message receive credit.
//
// |credit_delta| must be nonzero. The channel atomically adds it to the local
// cumulative grant and encodes the resulting limit after endpoint admission.
// The owner should call this only after it has capacity for the corresponding
// number of retained receive leases. Initial credit uses the same operation.
IREE_API_EXPORT iree_status_t iree_net_bulk_channel_send_credit(
    iree_net_bulk_channel_t* channel, uint64_t credit_delta,
    iree_net_send_completion_callback_t completion_callback);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_NET_CHANNEL_BULK_BULK_CHANNEL_H_
