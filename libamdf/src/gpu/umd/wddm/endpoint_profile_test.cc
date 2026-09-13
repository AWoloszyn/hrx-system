// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/gpu/umd/endpoint_profile.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "gtest/gtest.h"
#include "libamdf/src/allocator.h"
#include "libamdf/src/platform/windows/endpoint.h"

namespace {

constexpr NTSTATUS kSuccess = 0;

using ResetBridgeFn = void(__cdecl*)(void);
using SetBridgeCountFn = void(__cdecl*)(uint32_t);
using QueryBridgeCountFn = uint32_t(__cdecl*)(void);

struct FakeKmtState {
  // Bridge release count queried when the endpoint adapter is closed.
  QueryBridgeCountFn query_bridge_close_success_count = nullptr;
  // Number of successfully closed endpoint adapters.
  uint32_t adapter_close_success_count = 0;
};

FakeKmtState* current_state = nullptr;

NTSTATUS APIENTRY FakeCloseAdapter(const D3DKMT_CLOSEADAPTER* close) {
  EXPECT_EQ(close->hAdapter, 0x08u);
  ++current_state->adapter_close_success_count;
  return kSuccess;
}

class WindowsGpuEndpointProfileTest : public ::testing::Test {
 protected:
  void SetUp() override {
    current_state = &state_;
    instance_.host_allocator = amdf_allocator_system();
    instance_.kmt.close_adapter = FakeCloseAdapter;

    ASSERT_EQ(amdf_calloc(instance_.host_allocator, sizeof(*endpoint_),
                          amdf_alignof(amdf_platform_endpoint_t),
                          reinterpret_cast<void**>(&endpoint_)),
              AMDF_STATUS_OK);
    endpoint_->instance = &instance_;
    endpoint_->adapter = 0x08;
    endpoint_->physical_adapter_index = 0;

    const DWORD path_capacity =
        GetEnvironmentVariableW(L"AMDF_WKMI_BRIDGE_PATH", nullptr, 0);
    ASSERT_GT(path_capacity, 0u);
    std::vector<wchar_t> path(path_capacity);
    ASSERT_LT(GetEnvironmentVariableW(L"AMDF_WKMI_BRIDGE_PATH", path.data(),
                                      path_capacity),
              path_capacity);
    const DWORD absolute_capacity =
        GetFullPathNameW(path.data(), 0, nullptr, nullptr);
    ASSERT_GT(absolute_capacity, 0u);
    std::vector<wchar_t> absolute_path(absolute_capacity);
    ASSERT_LT(GetFullPathNameW(path.data(), absolute_capacity,
                               absolute_path.data(), nullptr),
              absolute_capacity);
    bridge_module_ = LoadLibraryExW(
        absolute_path.data(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    ASSERT_NE(bridge_module_, nullptr);

    reset_bridge_ = reinterpret_cast<ResetBridgeFn>(
        GetProcAddress(bridge_module_, "amdf_test_wkmi_bridge_reset"));
    set_bridge_close_failures_ = reinterpret_cast<SetBridgeCountFn>(
        GetProcAddress(bridge_module_,
                       "amdf_test_wkmi_bridge_set_adapter_close_failures"));
    query_bridge_open_success_count_ =
        reinterpret_cast<QueryBridgeCountFn>(GetProcAddress(
            bridge_module_,
            "amdf_test_wkmi_bridge_query_adapter_open_success_count"));
    query_bridge_close_attempt_count_ =
        reinterpret_cast<QueryBridgeCountFn>(GetProcAddress(
            bridge_module_,
            "amdf_test_wkmi_bridge_query_adapter_close_attempt_count"));
    state_.query_bridge_close_success_count =
        reinterpret_cast<QueryBridgeCountFn>(GetProcAddress(
            bridge_module_,
            "amdf_test_wkmi_bridge_query_adapter_close_success_count"));
    ASSERT_NE(reset_bridge_, nullptr);
    ASSERT_NE(set_bridge_close_failures_, nullptr);
    ASSERT_NE(query_bridge_open_success_count_, nullptr);
    ASSERT_NE(query_bridge_close_attempt_count_, nullptr);
    ASSERT_NE(state_.query_bridge_close_success_count, nullptr);
    reset_bridge_();
  }

  void TearDown() override {
    if (endpoint_ != nullptr) {
      EXPECT_EQ(amdf_platform_endpoint_close(endpoint_), AMDF_STATUS_OK);
      endpoint_ = nullptr;
    }
    if (bridge_module_ != nullptr) {
      EXPECT_TRUE(FreeLibrary(bridge_module_));
      bridge_module_ = nullptr;
    }
    current_state = nullptr;
  }

  // Native dependency observations for the active test.
  FakeKmtState state_;
  // Platform instance owning the injected KMT table.
  amdf_platform_instance_t instance_ = {};
  // Live query endpoint, with no ownership of failed qualification.
  amdf_platform_endpoint_t* endpoint_ = nullptr;
  // Test-owned bridge reference used to inspect shared fake state.
  HMODULE bridge_module_ = nullptr;
  // Resets all fake bridge state before each test.
  ResetBridgeFn reset_bridge_ = nullptr;
  // Selects the number of rejected bridge adapter closes.
  SetBridgeCountFn set_bridge_close_failures_ = nullptr;
  // Queries successful fake bridge adapter opens.
  QueryBridgeCountFn query_bridge_open_success_count_ = nullptr;
  // Queries all fake bridge adapter-close attempts.
  QueryBridgeCountFn query_bridge_close_attempt_count_ = nullptr;
};

TEST_F(WindowsGpuEndpointProfileTest,
       FailedProbeReleaseLeavesNoEndpointCleanupObligation) {
  amdf_gpu_endpoint_profile_t profile = {};
  profile.info.gfx_ip.major = 99;
  const amdf_gpu_endpoint_profile_t sentinel_profile = profile;
  bool available = true;

  const amdf_status_t status = amdf_gpu_umd_query_endpoint_profile(
      endpoint_, AMDF_NATIVE_LIFETIME_PROCESS, instance_.host_allocator,
      &profile, &available);

  EXPECT_EQ(amdf_status_code(status), AMDF_STATUS_CODE_BUSY);
  EXPECT_EQ(std::memcmp(&profile, &sentinel_profile, sizeof(profile)), 0);
  EXPECT_TRUE(available);
  EXPECT_EQ(query_bridge_open_success_count_(), 1u);
  EXPECT_EQ(query_bridge_close_attempt_count_(), 1u);
  EXPECT_EQ(state_.query_bridge_close_success_count(), 0u);
  EXPECT_EQ(state_.adapter_close_success_count, 0u);

  EXPECT_EQ(amdf_platform_endpoint_close(endpoint_), AMDF_STATUS_OK);
  endpoint_ = nullptr;
  EXPECT_EQ(query_bridge_close_attempt_count_(), 1u);
  EXPECT_EQ(state_.query_bridge_close_success_count(), 0u);
  EXPECT_EQ(state_.adapter_close_success_count, 1u);
}

TEST_F(WindowsGpuEndpointProfileTest,
       PublishesQualifiedProfileAfterProbeRelease) {
  set_bridge_close_failures_(0);
  amdf_gpu_endpoint_profile_t profile = {};
  bool available = false;

  EXPECT_EQ(amdf_gpu_umd_query_endpoint_profile(
                endpoint_, AMDF_NATIVE_LIFETIME_PROCESS,
                instance_.host_allocator, &profile, &available),
            AMDF_STATUS_OK);

  EXPECT_TRUE(available);
  EXPECT_EQ(profile.info.gfx_ip.major, 11u);
  EXPECT_EQ(profile.info.gfx_ip.minor, 5u);
  EXPECT_EQ(profile.info.gfx_ip.stepping, 1u);
  EXPECT_EQ(profile.info.compute.wavefront_size, 32u);
  for (amdf_native_lifetime_t lifetime :
       {AMDF_NATIVE_LIFETIME_PROCESS, AMDF_NATIVE_LIFETIME_INSTANCE}) {
    EXPECT_TRUE(profile.native_lifetimes[lifetime].supported);
    EXPECT_EQ(profile.native_lifetimes[lifetime].features,
              AMDF_GPU_DEVICE_FEATURE_HOST_REGISTRATION |
                  AMDF_GPU_DEVICE_FEATURE_DEVICE_RECREATION |
                  AMDF_GPU_DEVICE_FEATURE_LOCAL_MEMORY);
  }
  EXPECT_EQ(query_bridge_open_success_count_(), 1u);
  EXPECT_EQ(query_bridge_close_attempt_count_(), 1u);
  EXPECT_EQ(state_.query_bridge_close_success_count(), 1u);

  EXPECT_EQ(amdf_platform_endpoint_close(endpoint_), AMDF_STATUS_OK);
  endpoint_ = nullptr;
  EXPECT_EQ(state_.adapter_close_success_count, 1u);
}

}  // namespace
