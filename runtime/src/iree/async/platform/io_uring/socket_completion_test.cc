// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/async/platform/io_uring/socket_completion.h"

#include <cerrno>

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace {

static iree_io_uring_cqe_t MakeCqe(int32_t result, uint32_t flags) {
  return {/*user_data=*/0, /*res=*/result, /*flags=*/flags};
}

TEST(SocketSendCompletionTest, DefersPrimaryFailureUntilNotification) {
  iree_async_socket_send_operation_t operation = {};
  iree_io_uring_cqe_t primary_cqe = MakeCqe(-EPIPE, IREE_IORING_CQE_F_MORE);
  iree_async_io_uring_socket_send_completion_t completion =
      iree_async_io_uring_socket_process_send_cqe(&primary_cqe, &operation);
  EXPECT_FALSE(completion.is_terminal);
  IREE_EXPECT_OK(completion.status);
  EXPECT_EQ(operation.bytes_sent, 0u);

  iree_io_uring_cqe_t notification_cqe = MakeCqe(0, IREE_IORING_CQE_F_NOTIF);
  completion = iree_async_io_uring_socket_process_send_cqe(&notification_cqe,
                                                           &operation);
  EXPECT_TRUE(completion.is_terminal);
  IREE_EXPECT_STATUS_IS(iree_status_code_from_errno(EPIPE), completion.status);
  EXPECT_EQ(completion.flags, IREE_ASYNC_COMPLETION_FLAG_NONE);
}

TEST(SocketSendCompletionTest, PreservesBytesThroughCopiedNotification) {
  iree_async_socket_send_operation_t operation = {};
  iree_io_uring_cqe_t primary_cqe = MakeCqe(17, IREE_IORING_CQE_F_MORE);
  iree_async_io_uring_socket_send_completion_t completion =
      iree_async_io_uring_socket_process_send_cqe(&primary_cqe, &operation);
  EXPECT_FALSE(completion.is_terminal);
  EXPECT_EQ(operation.bytes_sent, 17u);

  iree_io_uring_cqe_t notification_cqe =
      MakeCqe(static_cast<int32_t>(IREE_IORING_NOTIF_USAGE_ZC_COPIED),
              IREE_IORING_CQE_F_NOTIF);
  completion = iree_async_io_uring_socket_process_send_cqe(&notification_cqe,
                                                           &operation);
  EXPECT_TRUE(completion.is_terminal);
  IREE_EXPECT_OK(completion.status);
  EXPECT_EQ(completion.flags, IREE_ASYNC_COMPLETION_FLAG_NONE);
  EXPECT_EQ(operation.bytes_sent, 17u);
}

TEST(SocketSendCompletionTest, ReportsZeroCopyAfterOwnershipNotification) {
  iree_async_socket_sendto_operation_t operation = {};
  iree_io_uring_cqe_t primary_cqe = MakeCqe(23, IREE_IORING_CQE_F_MORE);
  iree_async_io_uring_socket_send_completion_t completion =
      iree_async_io_uring_socket_process_sendto_cqe(&primary_cqe, &operation);
  EXPECT_FALSE(completion.is_terminal);
  EXPECT_EQ(operation.bytes_sent, 23u);

  iree_io_uring_cqe_t notification_cqe = MakeCqe(0, IREE_IORING_CQE_F_NOTIF);
  completion = iree_async_io_uring_socket_process_sendto_cqe(&notification_cqe,
                                                             &operation);
  EXPECT_TRUE(completion.is_terminal);
  IREE_EXPECT_OK(completion.status);
  EXPECT_EQ(completion.flags, IREE_ASYNC_COMPLETION_FLAG_ZERO_COPY_ACHIEVED);
  EXPECT_EQ(operation.bytes_sent, 23u);
}

TEST(SocketSendCompletionTest, CompletesOrdinarySendFromPrimaryCqe) {
  iree_async_socket_send_operation_t operation = {};
  iree_io_uring_cqe_t cqe = MakeCqe(29, 0);
  iree_async_io_uring_socket_send_completion_t completion =
      iree_async_io_uring_socket_process_send_cqe(&cqe, &operation);
  EXPECT_TRUE(completion.is_terminal);
  IREE_EXPECT_OK(completion.status);
  EXPECT_EQ(completion.flags, IREE_ASYNC_COMPLETION_FLAG_NONE);
  EXPECT_EQ(operation.bytes_sent, 29u);
}

TEST(SocketSendCompletionTest, FailsOrdinarySendFromPrimaryCqe) {
  iree_async_socket_send_operation_t operation = {};
  iree_io_uring_cqe_t cqe = MakeCqe(-ECONNRESET, 0);
  iree_async_io_uring_socket_send_completion_t completion =
      iree_async_io_uring_socket_process_send_cqe(&cqe, &operation);
  EXPECT_TRUE(completion.is_terminal);
  IREE_EXPECT_STATUS_IS(iree_status_code_from_errno(ECONNRESET),
                        completion.status);
  EXPECT_EQ(completion.flags, IREE_ASYNC_COMPLETION_FLAG_NONE);
  EXPECT_EQ(operation.bytes_sent, 0u);
}

}  // namespace
