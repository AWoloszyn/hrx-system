// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/xdna/umd/mcdm/adapter_info.h"

#include <cstring>

#include "gtest/gtest.h"

namespace {

struct QueryState {
  // Native reserved word and hardware kind, neither used for admission.
  uint32_t private_info[2] = {0, 3};
  // Number of reply bytes populated by the native dependency.
  uint32_t reply_byte_length = 12;
  // Native kernel-buffer allocation policy.
  uint8_t unshared_kernel_buffers = 0;
  // Native failure injected at the query boundary.
  NTSTATUS status = 0;
  // Number of private queries; there must be no driver-version query.
  uint32_t query_count = 0;
};

QueryState* query_state = nullptr;

NTSTATUS APIENTRY Query(const D3DKMT_QUERYADAPTERINFO* query) {
  ++query_state->query_count;
  EXPECT_EQ(query->hAdapter, 1u);
  EXPECT_EQ(query->Type, KMTQAITYPE_UMDRIVERPRIVATE);
  EXPECT_EQ(query->PrivateDriverDataSize, 12u);
  if (query_state->status < 0) return query_state->status;
  if (query_state->reply_byte_length >= 8) {
    std::memcpy(query->pPrivateDriverData, query_state->private_info,
                sizeof(query_state->private_info));
  }
  if (query_state->reply_byte_length == 12) {
    static_cast<uint8_t*>(query->pPrivateDriverData)[8] =
        query_state->unshared_kernel_buffers;
  }
  return 0;
}

class WindowsXdnaAdapterInfoTest : public ::testing::Test {
 protected:
  void SetUp() override {
    query_state = &state_;
    kmt_.query_adapter_info = Query;
  }
  void TearDown() override { query_state = nullptr; }
  // Per-test native query responses and observations.
  QueryState state_;
  // Existing native API dependency, with no production test platform.
  amdf_kmt_api_t kmt_ = {};
};

TEST_F(WindowsXdnaAdapterInfoTest,
       UsesAllocationPolicyWithoutIdentityAdmission) {
  for (uint32_t identity : {0u, 3u, UINT32_MAX}) {
    state_.private_info[0] = identity;
    state_.private_info[1] = identity;
    for (uint8_t unshared : {uint8_t{0}, uint8_t{1}}) {
      state_.unshared_kernel_buffers = unshared;
      state_.query_count = 0;
      amdf_windows_xdna_adapter_info_t info = {};
      ASSERT_EQ(amdf_windows_xdna_adapter_info_query(&kmt_, 1, &info),
                AMDF_STATUS_OK);
      EXPECT_EQ(info.shared_kernel_buffers, unshared == 0);
      EXPECT_EQ(state_.query_count, 1u);
    }
  }
}

TEST_F(WindowsXdnaAdapterInfoTest, RejectsAbsentPolicyWithoutPublishing) {
  for (uint32_t reply_byte_length : {0u, 8u}) {
    state_.reply_byte_length = reply_byte_length;
    state_.query_count = 0;
    amdf_windows_xdna_adapter_info_t info = {true};
    EXPECT_EQ(
        amdf_status_code(amdf_windows_xdna_adapter_info_query(&kmt_, 1, &info)),
        AMDF_STATUS_CODE_UNSUPPORTED);
    EXPECT_TRUE(info.shared_kernel_buffers);
    EXPECT_EQ(state_.query_count, 1u);
  }
}

TEST_F(WindowsXdnaAdapterInfoTest, PropagatesNativeQueryFailureWithoutRetry) {
  for (uint32_t native_status : {0xC0000001u, 0xC0000023u}) {
    state_.status = static_cast<NTSTATUS>(native_status);
    state_.query_count = 0;
    amdf_windows_xdna_adapter_info_t info = {true};
    EXPECT_EQ(amdf_windows_xdna_adapter_info_query(&kmt_, 1, &info),
              amdf_kmt_make_status(state_.status));
    EXPECT_TRUE(info.shared_kernel_buffers);
    EXPECT_EQ(state_.query_count, 1u);
  }
}

TEST_F(WindowsXdnaAdapterInfoTest, RejectsInvalidPolicyWithoutPublishing) {
  for (uint8_t policy : {uint8_t{2}, uint8_t{UINT8_MAX}}) {
    state_.unshared_kernel_buffers = policy;
    state_.query_count = 0;
    amdf_windows_xdna_adapter_info_t info = {true};
    EXPECT_EQ(
        amdf_status_code(amdf_windows_xdna_adapter_info_query(&kmt_, 1, &info)),
        AMDF_STATUS_CODE_UNSUPPORTED);
    EXPECT_TRUE(info.shared_kernel_buffers);
    EXPECT_EQ(state_.query_count, 1u);
  }
}

}  // namespace
