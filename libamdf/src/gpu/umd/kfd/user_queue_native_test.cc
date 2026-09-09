// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/gpu/umd/kfd/user_queue_native.h"

#include <cerrno>

#include "gtest/gtest.h"

namespace {

TEST(KfdUserQueueNativeTest, ClassifiesDestroyIdentifierOwnership) {
  amdf_gpu_kfd_user_queue_destroy_result_t result =
      amdf_gpu_kfd_user_queue_classify_destroy_status(AMDF_STATUS_OK);
  EXPECT_EQ(result.status, AMDF_STATUS_OK);
  EXPECT_TRUE(result.identifier_consumed);

  const amdf_status_t retained =
      amdf_make_status(AMDF_STATUS_DOMAIN_ERRNO, EBUSY);
  result = amdf_gpu_kfd_user_queue_classify_destroy_status(retained);
  EXPECT_EQ(result.status, retained);
  EXPECT_FALSE(result.identifier_consumed);

  for (const int error : {ETIME, EIO}) {
    const amdf_status_t consumed =
        amdf_make_status(AMDF_STATUS_DOMAIN_ERRNO, error);
    result = amdf_gpu_kfd_user_queue_classify_destroy_status(consumed);
    EXPECT_EQ(result.status, consumed);
    EXPECT_TRUE(result.identifier_consumed);
  }

  const amdf_status_t portable_failure =
      amdf_make_api_status(AMDF_STATUS_CODE_DEVICE_LOST);
  result = amdf_gpu_kfd_user_queue_classify_destroy_status(portable_failure);
  EXPECT_EQ(result.status, portable_failure);
  EXPECT_FALSE(result.identifier_consumed);
}

}  // namespace
