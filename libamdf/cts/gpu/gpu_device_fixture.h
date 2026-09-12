// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_CTS_GPU_GPU_DEVICE_FIXTURE_H_
#define AMDF_CTS_GPU_GPU_DEVICE_FIXTURE_H_

#include <vector>

#include "amdf/amdf.h"
#include "amdf/gpu.h"
#include "gtest/gtest.h"
#include "util/provider.h"

// Finds a profile for explicit device access without acquiring another owner.
inline uint32_t FindGpuMemoryProfileOrdinal(
    const amdf_api_t* api, amdf_device_t* device,
    amdf_memory_class_t memory_class,
    amdf_memory_profile_roles_t required_roles,
    amdf_memory_flags_t required_flags, amdf_memory_access_t device_access) {
  for (uint32_t ordinal = 0;; ++ordinal) {
    amdf_memory_profile_t profile = {};
    profile.type = AMDF_STRUCTURE_TYPE_MEMORY_PROFILE;
    profile.structure_size = sizeof(profile);
    const amdf_status_t status =
        api->device_query_memory_profile(device, ordinal, &profile);
    if (amdf_status_code(status) == AMDF_STATUS_CODE_OUT_OF_RANGE) break;
    if (!amdf_status_is_ok(status)) {
      ADD_FAILURE() << "memory profile query failed: domain="
                    << amdf_status_domain(status)
                    << " code=" << amdf_status_code(status);
      break;
    }
    if (profile.memory_class == memory_class &&
        (profile.roles & required_roles) == required_roles &&
        (required_flags & ~profile.supported_flags) == 0 &&
        (device_access & profile.guaranteed_device_access) ==
            profile.guaranteed_device_access &&
        (device_access & ~profile.supported_device_access) == 0) {
      return ordinal;
    }
  }
  ADD_FAILURE() << "no matching memory profile";
  return AMDF_MEMORY_PROFILE_ORDINAL_UNKNOWN;
}

// Materializes the first selected GPU endpoint and one native device.
class GpuDeviceFixture : public ::testing::Test {
 protected:
  virtual amdf_gpu_device_mode_t GetDeviceMode() const {
    return AMDF_GPU_DEVICE_MODE_INDEPENDENT;
  }

  // Selects which opened GPU endpoint should back this fixture. A failure
  // leaves `out_matches` unchanged.
  virtual amdf_status_t MatchGpuEndpoint(amdf_endpoint_t* endpoint,
                                         bool* out_matches) const {
    (void)endpoint;
    *out_matches = true;
    return AMDF_STATUS_OK;
  }

  void SetUp() override {
    ASSERT_TRUE(amdf_status_is_ok(amdf_cts_provider_query_api()(
        AMDF_ABI_VERSION_1, AMDF_ABI_VERSION_LATEST, &api_)));
    ASSERT_NE(api_, nullptr);

    const void* extension_api = nullptr;
    ASSERT_TRUE(amdf_status_is_ok(api_->query_extension(
        AMDF_EXTENSION_GPU, AMDF_GPU_EXTENSION_VERSION_1,
        AMDF_GPU_EXTENSION_VERSION_LATEST, &extension_api)));
    gpu_api_ = static_cast<const amdf_gpu_api_t*>(extension_api);
    ASSERT_NE(gpu_api_, nullptr);

    amdf_instance_create_info_t instance_create_info = {};
    instance_create_info.type = AMDF_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_create_info.structure_size = sizeof(instance_create_info);
    amdf_status_t status =
        api_->instance_create(&instance_create_info, &instance_);
    if (amdf_status_domain(status) == AMDF_STATUS_DOMAIN_API &&
        amdf_status_code(status) == AMDF_STATUS_CODE_UNSUPPORTED) {
      GTEST_SKIP() << "platform provider is not implemented";
    }
    ASSERT_TRUE(amdf_status_is_ok(status));

    uint32_t endpoint_count = 0;
    ASSERT_TRUE(amdf_status_is_ok(
        api_->endpoint_enumerate(instance_, 0, nullptr, &endpoint_count)));
    std::vector<amdf_endpoint_summary_t> summaries(endpoint_count);
    if (endpoint_count != 0) {
      ASSERT_TRUE(amdf_status_is_ok(api_->endpoint_enumerate(
          instance_, endpoint_count, summaries.data(), &endpoint_count)));
    }
    for (const amdf_endpoint_summary_t& summary : summaries) {
      if (summary.engine_kind != AMDF_ENGINE_KIND_GPU) continue;
      ASSERT_TRUE(amdf_status_is_ok(
          api_->endpoint_open(instance_, &summary.id, &endpoint_)));
      bool matches = false;
      status = MatchGpuEndpoint(endpoint_, &matches);
      ASSERT_EQ(status, AMDF_STATUS_OK)
          << "domain=" << amdf_status_domain(status)
          << " code=" << amdf_status_code(status);
      if (matches) break;
      status = api_->endpoint_close(endpoint_);
      if (amdf_status_is_ok(status)) endpoint_ = nullptr;
      ASSERT_EQ(status, AMDF_STATUS_OK)
          << "domain=" << amdf_status_domain(status)
          << " code=" << amdf_status_code(status);
    }
    if (endpoint_ == nullptr) {
      GTEST_SKIP() << "no qualified GPU endpoint present";
    }

    amdf_gpu_device_capabilities_t capabilities = {};
    capabilities.type = AMDF_STRUCTURE_TYPE_GPU_DEVICE_CAPABILITIES;
    capabilities.structure_size = sizeof(capabilities);
    status = gpu_api_->endpoint_query_device_capabilities(
        endpoint_, GetDeviceMode(), &capabilities);
    if (amdf_status_domain(status) == AMDF_STATUS_DOMAIN_API &&
        amdf_status_code(status) == AMDF_STATUS_CODE_UNSUPPORTED) {
      GTEST_SKIP() << "requested GPU device mode is unavailable";
    }
    ASSERT_EQ(status, AMDF_STATUS_OK);
    features_ = capabilities.features;

    amdf_gpu_device_create_info_t device_create_info = {};
    device_create_info.type = AMDF_STRUCTURE_TYPE_GPU_DEVICE_CREATE_INFO;
    device_create_info.structure_size = sizeof(device_create_info);
    device_create_info.mode = GetDeviceMode();
    status = gpu_api_->device_create(endpoint_, &device_create_info, &device_);
    ASSERT_TRUE(amdf_status_is_ok(status))
        << "domain=" << amdf_status_domain(status)
        << " code=" << amdf_status_code(status);
  }

  void TearDown() override {
    if (device_ != nullptr) {
      ASSERT_EQ(api_->device_destroy(device_), AMDF_STATUS_OK);
      device_ = nullptr;
    }
    if (endpoint_ != nullptr) {
      ASSERT_EQ(api_->endpoint_close(endpoint_), AMDF_STATUS_OK);
      endpoint_ = nullptr;
    }
    if (instance_ != nullptr) {
      ASSERT_EQ(api_->instance_destroy(instance_), AMDF_STATUS_OK);
      instance_ = nullptr;
    }
  }

  uint32_t FindMemoryProfileOrdinal(amdf_memory_class_t memory_class,
                                    amdf_memory_profile_roles_t required_roles,
                                    amdf_memory_flags_t required_flags,
                                    amdf_memory_access_t device_access) const {
    return FindMemoryProfileOrdinal(device_, memory_class, required_roles,
                                    required_flags, device_access);
  }

  uint32_t FindMemoryProfileOrdinal(amdf_device_t* device,
                                    amdf_memory_class_t memory_class,
                                    amdf_memory_profile_roles_t required_roles,
                                    amdf_memory_flags_t required_flags,
                                    amdf_memory_access_t device_access) const {
    return FindGpuMemoryProfileOrdinal(api_, device, memory_class,
                                       required_roles, required_flags,
                                       device_access);
  }

  // Core table borrowed from the CTS provider.
  const amdf_api_t* api_ = nullptr;
  // GPU table borrowed from the CTS provider.
  const amdf_gpu_api_t* gpu_api_ = nullptr;
  // Instance owning the endpoint.
  amdf_instance_t* instance_ = nullptr;
  // Query endpoint borrowed by the device.
  amdf_endpoint_t* endpoint_ = nullptr;
  // Native device retained until all test children have been released.
  amdf_device_t* device_ = nullptr;
  // Cached capabilities of the explicitly selected mode.
  amdf_gpu_device_features_t features_ = 0;
};

#endif  // AMDF_CTS_GPU_GPU_DEVICE_FIXTURE_H_
