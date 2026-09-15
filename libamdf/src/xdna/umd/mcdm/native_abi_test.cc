// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/xdna/umd/mcdm/native_abi.h"

#include <cstring>

#include "gtest/gtest.h"

namespace {

struct QueryState {
  // KMD build returned through the standard adapter query.
  uint64_t version = UINT64_C(0x0020000000CB00F0);
  // Private response returned by the retained miniport ABI.
  uint32_t private_info[2] = {0, 3};
  // Whether the miniport actually writes a private-query response.
  bool writes_private_info = true;
  // Current native reply requires the additional allocation-policy byte.
  bool current_protocol = false;
  // Whether current kernel buffers may omit a shared KMT resource.
  uint8_t unshared_kernel_buffers = 0;
  // Native failure injected at the query boundary.
  NTSTATUS status = 0;
  // Number of private queries issued after the standard query.
  uint32_t private_query_count = 0;
};

QueryState* query_state = nullptr;

NTSTATUS APIENTRY Query(const D3DKMT_QUERYADAPTERINFO* query) {
  if (query_state->status < 0) return query_state->status;
  if (query->Type == KMTQAITYPE_KMD_DRIVER_VERSION) {
    EXPECT_EQ(query->PrivateDriverDataSize, sizeof(D3DKMT_KMD_DRIVER_VERSION));
    auto* version =
        static_cast<D3DKMT_KMD_DRIVER_VERSION*>(query->pPrivateDriverData);
    version->DriverVersion.QuadPart = query_state->version;
  } else {
    EXPECT_EQ(query->Type, KMTQAITYPE_UMDRIVERPRIVATE);
    ++query_state->private_query_count;
    if (query_state->current_protocol && query->PrivateDriverDataSize < 12) {
      return static_cast<NTSTATUS>(0xC0000023u);
    }
    if (query_state->writes_private_info) {
      std::memcpy(query->pPrivateDriverData, query_state->private_info,
                  sizeof(query_state->private_info));
      if (query_state->current_protocol) {
        static_cast<uint8_t*>(query->pPrivateDriverData)[8] =
            query_state->unshared_kernel_buffers;
      }
    }
  }
  return 0;
}

class WindowsXdnaNativeAbiTest : public ::testing::Test {
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

TEST_F(WindowsXdnaNativeAbiTest, AdmitsExactBuildWithoutUnwrittenPrivateQuery) {
  state_.writes_private_info = false;
  amdf_windows_xdna_native_abi_t abi = {};
  ASSERT_EQ(amdf_windows_xdna_native_abi_query(&kmt_, 1, &abi), AMDF_STATUS_OK);
  EXPECT_EQ(abi.context_encoding, AMDF_WINDOWS_XDNA_CONTEXT_ENCODING_METADATA);
  EXPECT_EQ(abi.context_cookie_byte_offset, 0x30u);
  EXPECT_EQ(abi.submission_header_byte_length, 88u);
  EXPECT_EQ(state_.private_query_count, 0u);
}

TEST_F(WindowsXdnaNativeAbiTest, PreservesLegacyReplyWithoutAsicAdmission) {
  state_.version = 0;
  state_.private_info[1] = 4;
  amdf_windows_xdna_native_abi_t abi = {};
  ASSERT_EQ(amdf_windows_xdna_native_abi_query(&kmt_, 1, &abi), AMDF_STATUS_OK);
  EXPECT_EQ(abi.context_encoding, AMDF_WINDOWS_XDNA_CONTEXT_ENCODING_XCLBIN);
  EXPECT_EQ(abi.context_cookie_byte_offset, 0x40u);
  EXPECT_EQ(abi.submission_header_byte_length, 104u);
  EXPECT_EQ(state_.private_query_count, 1u);
}

TEST_F(WindowsXdnaNativeAbiTest, RejectsUnwrittenReplyWithoutPublishing) {
  ++state_.version;
  state_.writes_private_info = false;
  amdf_windows_xdna_native_abi_t abi;
  std::memset(&abi, 0xA5, sizeof(abi));
  const auto original = abi;
  EXPECT_EQ(
      amdf_status_code(amdf_windows_xdna_native_abi_query(&kmt_, 1, &abi)),
      AMDF_STATUS_CODE_UNSUPPORTED);
  EXPECT_EQ(std::memcmp(&abi, &original, sizeof(abi)), 0);
  EXPECT_EQ(state_.private_query_count, 1u);
}

TEST_F(WindowsXdnaNativeAbiTest, PropagatesNativeQueryFailure) {
  state_.status = static_cast<NTSTATUS>(0xC0000001u);
  amdf_windows_xdna_native_abi_t abi = {};
  EXPECT_EQ(amdf_windows_xdna_native_abi_query(&kmt_, 1, &abi),
            amdf_kmt_make_status(state_.status));
  EXPECT_EQ(abi.submission_header_byte_length, 0u);
  EXPECT_EQ(state_.private_query_count, 0u);
}

TEST_F(WindowsXdnaNativeAbiTest,
       ResolvesCurrentAllocationPolicyAcrossHardware) {
  state_.version = 0;
  state_.current_protocol = true;
  for (uint32_t hardware_kind : {1u, 2u, 3u, 4u}) {
    state_.private_info[1] = hardware_kind;
    for (uint8_t unshared : {uint8_t{0}, uint8_t{1}}) {
      state_.unshared_kernel_buffers = unshared;
      state_.private_query_count = 0;
      amdf_windows_xdna_native_abi_t abi = {};
      ASSERT_EQ(amdf_windows_xdna_native_abi_query(&kmt_, 1, &abi),
                AMDF_STATUS_OK);
      EXPECT_EQ(abi.context_encoding,
                AMDF_WINDOWS_XDNA_CONTEXT_ENCODING_DIRECT);
      EXPECT_EQ(abi.shared_kernel_buffers, unshared == 0);
      EXPECT_EQ(state_.private_query_count, 2u);
    }
  }
}

TEST_F(WindowsXdnaNativeAbiTest, RejectsMissingCurrentPolicyWithoutPublishing) {
  state_.version = 0;
  state_.current_protocol = true;
  state_.unshared_kernel_buffers = UINT8_MAX;
  amdf_windows_xdna_native_abi_t abi;
  std::memset(&abi, 0xA5, sizeof(abi));
  const auto original = abi;
  EXPECT_EQ(
      amdf_status_code(amdf_windows_xdna_native_abi_query(&kmt_, 1, &abi)),
      AMDF_STATUS_CODE_UNSUPPORTED);
  EXPECT_EQ(std::memcmp(&abi, &original, sizeof(abi)), 0);
  EXPECT_EQ(state_.private_query_count, 2u);
}

}  // namespace
