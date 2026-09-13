// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_CTS_API_XDNA_DEVICE_FIXTURE_H_
#define AMDF_CTS_API_XDNA_DEVICE_FIXTURE_H_

#include <vector>

#include "amdf/amdf.h"
#include "amdf/xdna.h"
#include "gtest/gtest.h"
#include "util/device_cache.h"
#include "util/provider.h"

// Borrows the shared device for the first qualified XDNA endpoint.
class XdnaDeviceFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(amdf_status_is_ok(amdf_cts_provider_query_api()(
        AMDF_ABI_VERSION_1, AMDF_ABI_VERSION_LATEST, &api_)));
    ASSERT_NE(api_, nullptr);

    const void* extension_api = nullptr;
    ASSERT_TRUE(amdf_status_is_ok(api_->query_extension(
        AMDF_EXTENSION_XDNA, AMDF_XDNA_EXTENSION_VERSION_1,
        AMDF_XDNA_EXTENSION_VERSION_LATEST, &extension_api)));
    xdna_api_ = static_cast<const amdf_xdna_api_t*>(extension_api);
    ASSERT_NE(xdna_api_, nullptr);

    amdf_status_t status = GetCtsDeviceCache().GetInstance(&instance_);
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
      if (summary.engine_kind == AMDF_ENGINE_KIND_XDNA) {
        ASSERT_TRUE(amdf_status_is_ok(
            GetCtsDeviceCache().OpenEndpoint(summary.id, &endpoint_)));
        break;
      }
    }
    if (endpoint_ == nullptr) {
      GTEST_SKIP() << "no qualified XDNA endpoint present";
    }

    status = GetCtsDeviceCache().GetXdnaDevice(endpoint_, &device_);
    if (amdf_status_domain(status) == AMDF_STATUS_DOMAIN_API &&
        amdf_status_code(status) == AMDF_STATUS_CODE_UNSUPPORTED) {
      GTEST_SKIP() << "XDNA device materialization is unavailable";
    }
    ASSERT_TRUE(amdf_status_is_ok(status))
        << "domain=" << amdf_status_domain(status)
        << " code=" << amdf_status_code(status);
  }

  uint32_t FindMemoryProfileOrdinal(amdf_memory_class_t memory_class,
                                    amdf_memory_profile_roles_t required_roles,
                                    amdf_memory_flags_t required_flags,
                                    amdf_memory_access_t device_access) const {
    for (uint32_t ordinal = 0;; ++ordinal) {
      amdf_memory_profile_t profile = {};
      profile.type = AMDF_STRUCTURE_TYPE_MEMORY_PROFILE;
      profile.structure_size = sizeof(profile);
      const amdf_status_t status =
          api_->device_query_memory_profile(device_, ordinal, &profile);
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

  // Core API table borrowed from the CTS provider.
  const amdf_api_t* api_ = nullptr;
  // XDNA API table borrowed from the CTS provider.
  const amdf_xdna_api_t* xdna_api_ = nullptr;
  // Shared instance borrowed from the process-lifetime CTS cache.
  amdf_instance_t* instance_ = nullptr;
  // Shared endpoint borrowed from the CTS cache.
  amdf_endpoint_t* endpoint_ = nullptr;
  // Shared device; each case releases only its workload children.
  amdf_device_t* device_ = nullptr;
};

// Cases exercising program activation or placement own a fresh context within
// the shared device. Device-only memory tests do not acquire execution state.
class XdnaContextFixture : public XdnaDeviceFixture {
 protected:
  void SetUp() override {
    ASSERT_NO_FATAL_FAILURE(XdnaDeviceFixture::SetUp());
    if (IsSkipped()) return;
    amdf_xdna_endpoint_info_t endpoint_info = {};
    endpoint_info.type = AMDF_STRUCTURE_TYPE_XDNA_ENDPOINT_INFO;
    endpoint_info.structure_size = sizeof(endpoint_info);
    ASSERT_EQ(xdna_api_->endpoint_query_info(endpoint_, &endpoint_info),
              AMDF_STATUS_OK);
    if ((endpoint_info.context.scheduling_modes &
         AMDF_XDNA_SCHEDULING_MODE_TIME_SLICED) == 0) {
      GTEST_SKIP() << "time-sliced XDNA contexts are unavailable";
    }
    amdf_xdna_context_create_info_t create_info = {};
    create_info.type = AMDF_STRUCTURE_TYPE_XDNA_CONTEXT_CREATE_INFO;
    create_info.structure_size = sizeof(create_info);
    create_info.logical_column_count = 1;
    create_info.physical_column_origin = AMDF_XDNA_PHYSICAL_COLUMN_ORIGIN_ANY;
    create_info.acceptable_scheduling_modes =
        AMDF_XDNA_SCHEDULING_MODE_TIME_SLICED;
    ASSERT_EQ(xdna_api_->context_create(device_, &create_info, &context_),
              AMDF_STATUS_OK);
  }

  void TearDown() override {
    if (context_ != nullptr) {
      ASSERT_EQ(xdna_api_->context_destroy(context_), AMDF_STATUS_OK);
      context_ = nullptr;
    }
  }

  // Case-owned execution context, released after all of its children.
  amdf_xdna_context_t* context_ = nullptr;
};

#endif  // AMDF_CTS_API_XDNA_DEVICE_FIXTURE_H_
