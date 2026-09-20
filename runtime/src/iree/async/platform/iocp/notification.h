// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef IREE_ASYNC_PLATFORM_IOCP_NOTIFICATION_H_
#define IREE_ASYNC_PLATFORM_IOCP_NOTIFICATION_H_

#include "iree/async/notification.h"
#include "iree/async/operations/scheduling.h"
#include "iree/async/relay.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct iree_async_proactor_iocp_t iree_async_proactor_iocp_t;

// Native packets carry the notification until the poll owner consumes them.
#define IREE_ASYNC_IOCP_SHARED_NOTIFICATION_COMPLETION_KEY ((uintptr_t)2)

iree_status_t iree_async_iocp_notification_create(
    iree_async_proactor_t* proactor, iree_async_notification_flags_t flags,
    iree_async_notification_t** out_notification);
iree_status_t iree_async_iocp_notification_create_shared(
    iree_async_proactor_t* proactor, iree_async_notification_native_t* native,
    iree_async_notification_t** out_notification);
void iree_async_iocp_notification_destroy(
    iree_async_proactor_t* proactor, iree_async_notification_t* notification);
void iree_async_iocp_notification_signal(
    iree_async_proactor_t* proactor, iree_async_notification_t* notification,
    int32_t wake_count);
bool iree_async_iocp_notification_wait(iree_async_proactor_t* proactor,
                                       iree_async_notification_t* notification,
                                       uint32_t wait_token,
                                       iree_timeout_t timeout);

// Attaches an accepted wait on the poll owner. Submission already retains its
// notification and captures the token before entering the pending queue.
void iree_async_iocp_notification_attach_wait(
    iree_async_proactor_iocp_t* proactor,
    iree_async_notification_wait_operation_t* wait);

// Retires one native association without rearming or dispatching user code.
void iree_async_iocp_notification_wake(iree_async_notification_t* notification);

// Classifies consumers, manages reusable native monitoring, and finishes source
// bookkeeping before returning terminal consumer ownership. Poll-owner only.
iree_status_t iree_async_iocp_notification_poll(
    iree_async_proactor_iocp_t* proactor, iree_host_size_t* completed_count);

iree_status_t iree_async_iocp_notification_register_relay(
    iree_async_proactor_t* proactor, iree_async_relay_source_t source,
    iree_async_relay_sink_t sink, iree_async_relay_flags_t flags,
    iree_async_relay_error_callback_t error_callback,
    iree_async_relay_t** out_relay);
void iree_async_iocp_notification_unregister_relay(
    iree_async_proactor_t* proactor, iree_async_relay_t* relay,
    iree_async_relay_unregistered_callback_t callback);

// Joins native notification observers and remaining relays during destruction.
// Event sources must already be retired; user operations must be drained.
void iree_async_iocp_notification_deinitialize_relays(
    iree_async_proactor_iocp_t* proactor);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // IREE_ASYNC_PLATFORM_IOCP_NOTIFICATION_H_
