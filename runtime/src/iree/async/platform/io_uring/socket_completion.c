// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/async/platform/io_uring/socket_completion.h"

// Processes the shared SEND_ZC/SENDMSG_ZC two-CQE lifecycle.
static iree_async_io_uring_socket_send_completion_t
iree_async_io_uring_socket_process_send_cqe_impl(
    const iree_io_uring_cqe_t* cqe, iree_async_operation_type_t operation_type,
    iree_async_socket_send_flags_t send_flags, int32_t* primary_result,
    iree_host_size_t* bytes_sent) {
  iree_async_io_uring_socket_send_completion_t completion = {
      .status = iree_ok_status(),
      .flags = IREE_ASYNC_COMPLETION_FLAG_NONE,
      .dispatch = true,
  };

  bool is_notification = iree_any_bit_set(cqe->flags, IREE_IORING_CQE_F_NOTIF);
  if (!is_notification) {
    // The primary CQE ends the lifetime of POSIX descriptors in platform
    // storage, so the raw result can replace them until payload ownership is
    // returned by the notification.
    *primary_result = cqe->res;
    *bytes_sent = cqe->res >= 0 ? (iree_host_size_t)cqe->res : 0;
    if (iree_any_bit_set(cqe->flags, IREE_IORING_CQE_F_MORE)) {
      completion.dispatch =
          cqe->res > 0 &&
          iree_any_bit_set(send_flags,
                           IREE_ASYNC_SOCKET_SEND_FLAG_REPORT_PROGRESS);
      completion.flags = IREE_ASYNC_COMPLETION_FLAG_MORE;
      return completion;
    }
  }

  int32_t operation_result = is_notification ? *primary_result : cqe->res;
  if (operation_result < 0) {
    completion.status =
        iree_make_status(iree_status_code_from_errno(-operation_result),
                         "io_uring operation type %d failed (%d)",
                         (int)operation_type, -operation_result);
  } else if (is_notification && cqe->res == 0) {
    completion.flags |= IREE_ASYNC_COMPLETION_FLAG_ZERO_COPY_ACHIEVED;
  }
  return completion;
}

iree_async_io_uring_socket_send_completion_t
iree_async_io_uring_socket_process_send_cqe(
    const iree_io_uring_cqe_t* cqe,
    iree_async_socket_send_operation_t* operation) {
  return iree_async_io_uring_socket_process_send_cqe_impl(
      cqe, IREE_ASYNC_OPERATION_TYPE_SOCKET_SEND, operation->send_flags,
      &operation->platform.io_uring.primary_result, &operation->bytes_sent);
}

iree_async_io_uring_socket_send_completion_t
iree_async_io_uring_socket_process_sendto_cqe(
    const iree_io_uring_cqe_t* cqe,
    iree_async_socket_sendto_operation_t* operation) {
  return iree_async_io_uring_socket_process_send_cqe_impl(
      cqe, IREE_ASYNC_OPERATION_TYPE_SOCKET_SENDTO, operation->send_flags,
      &operation->platform.io_uring.primary_result, &operation->bytes_sent);
}
