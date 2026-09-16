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
  // Terminal operation status. Owned by the caller when |is_terminal| is true.
  iree_status_t status;

  // Completion flags to publish with a terminal operation callback.
  iree_async_completion_flags_t flags;

  // True when the operation can release its payload and invoke its callback.
  bool is_terminal;
} iree_async_io_uring_socket_send_completion_t;

// Processes a CQE for a connected socket send operation.
//
// A zero-copy primary CQE records its result and returns a non-terminal result.
// The later ownership notification returns the retained primary status and
// indicates whether zero-copy was achieved.
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
