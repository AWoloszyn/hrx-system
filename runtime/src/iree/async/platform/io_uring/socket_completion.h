// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// io_uring socket send completion processing.

#ifndef IREE_ASYNC_PLATFORM_IO_URING_SOCKET_COMPLETION_H_
#define IREE_ASYNC_PLATFORM_IO_URING_SOCKET_COMPLETION_H_

#include "iree/async/operations/net.h"
#include "iree/async/platform/io_uring/defs.h"
#include "iree/base/api.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Result of processing one connected or unconnected socket send CQE.
typedef struct iree_async_io_uring_socket_send_completion_t {
  // Operation status transferred to the callback when |dispatch| is true.
  iree_status_t status;

  // Callback flags; MORE keeps operation and source ownership with the kernel.
  iree_async_completion_flags_t flags;

  // True when the result should be delivered to the operation callback.
  bool dispatch;
} iree_async_io_uring_socket_send_completion_t;

// Processes a CQE for a connected socket send operation.
//
// A zero-copy primary CQE records its result. Successful write progress can
// dispatch a MORE callback when requested; otherwise the result is withheld.
// The ownership notification dispatches the retained primary status without
// MORE and indicates whether zero-copy was achieved.
iree_async_io_uring_socket_send_completion_t
iree_async_io_uring_socket_process_send_cqe(
    const iree_io_uring_cqe_t* cqe,
    iree_async_socket_send_operation_t* operation);

// Processes a CQE for an unconnected socket send operation.
iree_async_io_uring_socket_send_completion_t
iree_async_io_uring_socket_process_sendto_cqe(
    const iree_io_uring_cqe_t* cqe,
    iree_async_socket_sendto_operation_t* operation);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // IREE_ASYNC_PLATFORM_IO_URING_SOCKET_COMPLETION_H_
