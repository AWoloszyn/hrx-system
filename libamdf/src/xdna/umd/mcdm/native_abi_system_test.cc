// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "libamdf/src/allocator.h"
#include "libamdf/src/platform/windows/endpoint.h"
#include "libamdf/src/xdna/endpoint_profile.h"
#include "libamdf/src/xdna/umd/mcdm/native_abi.h"

namespace {

// Borrowed native procedure for this serial hardware test. The observer
// forwards each production query unchanged and records the native responses.
PFND3DKMT_QUERYADAPTERINFO native_query = nullptr;

NTSTATUS APIENTRY ObserveQuery(const D3DKMT_QUERYADAPTERINFO* query) {
  const NTSTATUS status = native_query(query);
  std::cout << "QueryAdapterInfo type=" << query->Type
            << " bytes=" << query->PrivateDriverDataSize << " status=0x"
            << std::hex << static_cast<uint32_t>(status) << std::dec;
  if (status >= 0 && query->Type == KMTQAITYPE_KMD_DRIVER_VERSION) {
    const auto* version = static_cast<const D3DKMT_KMD_DRIVER_VERSION*>(
        query->pPrivateDriverData);
    const uint64_t value = version->DriverVersion.QuadPart;
    const std::string driver_version = std::to_string(value >> 48) + '.' +
                                       std::to_string((value >> 32) & 0xffff) +
                                       '.' +
                                       std::to_string((value >> 16) & 0xffff) +
                                       '.' + std::to_string(value & 0xffff);
    ::testing::Test::RecordProperty("kmd_driver_version", driver_version);
    std::cout << " driver=" << driver_version;
  }
  std::cout << std::endl;
  return status;
}

class WindowsXdnaNativeAbiSystemTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(
        amdf_platform_instance_create(amdf_allocator_system(), &instance_),
        AMDF_STATUS_OK);
    native_query = instance_->kmt.query_adapter_info;
  }

  void TearDown() override {
    if (endpoint_) {
      ASSERT_EQ(amdf_platform_endpoint_close(endpoint_), AMDF_STATUS_OK);
    }
    if (instance_) {
      EXPECT_EQ(amdf_platform_instance_destroy(instance_), AMDF_STATUS_OK);
    }
    native_query = nullptr;
  }

  // Query-only instance; no logical device is created by this test.
  amdf_platform_instance_t* instance_ = nullptr;
  // Adapter retained through early assertion exits and closed before instance_.
  amdf_platform_endpoint_t* endpoint_ = nullptr;
};

TEST_F(WindowsXdnaNativeAbiSystemTest, ResolvesAbiBeforeDeviceCreation) {
  uint32_t count = 0;
  ASSERT_EQ(amdf_platform_endpoint_enumerate(instance_, 0, nullptr, &count),
            AMDF_STATUS_OK);
  std::vector<amdf_endpoint_summary_t> summaries(count);
  ASSERT_EQ(amdf_platform_endpoint_enumerate(instance_, count, summaries.data(),
                                             &count),
            AMDF_STATUS_OK);
  bool found = false;
  for (const auto& summary : summaries) {
    if (summary.engine_kind != AMDF_ENGINE_KIND_XDNA) continue;
    SCOPED_TRACE(summary.name);
    amdf_endpoint_info_t info = {};
    ASSERT_EQ(
        amdf_platform_endpoint_open(instance_, &summary.id, &endpoint_, &info),
        AMDF_STATUS_OK);
    const auto* profile = amdf_xdna_endpoint_profile_select(&info);
    if (profile &&
        (profile->execution_capabilities &
         AMDF_XDNA_EXECUTION_CAPABILITY_TRANSACTION_INTERPRETER_V1) != 0) {
      found = true;
      // ABI discovery receives only a query procedure, making any dependency
      // on device, context, paging or allocation creation visible here.
      amdf_kmt_api_t query_api = {};
      query_api.query_adapter_info = ObserveQuery;
      amdf_windows_xdna_native_abi_t abi = {};
      ASSERT_EQ(amdf_windows_xdna_native_abi_query(&query_api,
                                                   endpoint_->adapter, &abi),
                AMDF_STATUS_OK);
      EXPECT_GT(abi.submission_header_byte_length, 0u);
      RecordProperty("context_encoding",
                     static_cast<int>(abi.context_encoding));
      RecordProperty("submission_header_byte_length",
                     static_cast<int>(abi.submission_header_byte_length));
      RecordProperty("shared_kernel_buffers",
                     abi.shared_kernel_buffers ? 1 : 0);
      std::cout << "Native context encoding=" << abi.context_encoding
                << " submission header bytes="
                << abi.submission_header_byte_length
                << " shared kernel buffers=" << abi.shared_kernel_buffers
                << std::endl;
    }
    ASSERT_EQ(amdf_platform_endpoint_close(endpoint_), AMDF_STATUS_OK);
    endpoint_ = nullptr;
  }
  if (!found) {
    GTEST_SKIP() << "no XDNA endpoint with interpreter execution support";
  }
}

}  // namespace
