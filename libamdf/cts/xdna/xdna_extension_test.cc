// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "amdf/amdf.h"
#include "amdf/xdna.h"
#include "gtest/gtest.h"
#include "util/device_cache.h"
#include "util/provider.h"
#include "xdna_device_fixture.h"

namespace {

static_assert(sizeof(amdf_xdna_device_create_info_t) ==
              sizeof(amdf_input_structure_t));
static_assert(offsetof(amdf_xdna_device_info_t, id) ==
              sizeof(amdf_output_structure_t));
static_assert(offsetof(amdf_xdna_device_info_t, reset_epoch) == 32);
static_assert(offsetof(amdf_xdna_device_info_t, placement_modes) == 40);
static_assert(sizeof(amdf_xdna_device_info_t) == 48);
static_assert(offsetof(amdf_xdna_context_create_info_t, logical_column_count) ==
              sizeof(amdf_input_structure_t));
static_assert(offsetof(amdf_xdna_context_create_info_t,
                       acceptable_scheduling_modes) == 24);
static_assert(sizeof(amdf_xdna_context_create_info_t) == 32);
static_assert(offsetof(amdf_xdna_context_info_t, id) ==
              sizeof(amdf_output_structure_t));
static_assert(offsetof(amdf_xdna_context_info_t, device_id) == 32);
static_assert(offsetof(amdf_xdna_context_info_t, reset_epoch) == 48);
static_assert(offsetof(amdf_xdna_context_info_t, logical_column_count) == 60);
static_assert(offsetof(amdf_xdna_context_info_t, row_count) == 64);
static_assert(sizeof(amdf_xdna_context_info_t) == 72);
static_assert(offsetof(amdf_xdna_context_placement_info_t, column_origin) ==
              sizeof(amdf_output_structure_t));
static_assert(sizeof(amdf_xdna_context_placement_info_t) == 24);
static_assert(offsetof(amdf_xdna_endpoint_info_t, target_id) == 76);
static_assert(sizeof(amdf_xdna_endpoint_info_t) == 144);
static_assert(offsetof(amdf_xdna_api_t, context_destroy) +
                  sizeof(amdf_xdna_api_t::context_destroy) ==
              sizeof(amdf_xdna_api_t));

const amdf_api_t* QueryApi() {
  const amdf_api_t* api = nullptr;
  EXPECT_TRUE(amdf_status_is_ok(amdf_cts_provider_query_api()(
      AMDF_ABI_VERSION_1, AMDF_ABI_VERSION_LATEST, &api)));
  return api;
}

const amdf_xdna_api_t* QueryXdnaApi(const amdf_api_t* api) {
  const void* extension_api = nullptr;
  EXPECT_TRUE(amdf_status_is_ok(api->query_extension(
      AMDF_EXTENSION_XDNA, AMDF_XDNA_EXTENSION_VERSION_1,
      AMDF_XDNA_EXTENSION_VERSION_LATEST, &extension_api)));
  return static_cast<const amdf_xdna_api_t*>(extension_api);
}

TEST(XdnaExtensionTest, ReportsCompiledAvailabilityBeforeCreatingInstance) {
  const amdf_api_t* api = QueryApi();
  ASSERT_NE(api, nullptr);
  const void* extension_api = reinterpret_cast<const void*>(uintptr_t{1});

  const amdf_status_t status =
      api->query_extension(AMDF_EXTENSION_XDNA, AMDF_XDNA_EXTENSION_VERSION_1,
                           AMDF_XDNA_EXTENSION_VERSION_LATEST, &extension_api);

  ASSERT_TRUE(amdf_status_is_ok(status));
  const auto* xdna_api = static_cast<const amdf_xdna_api_t*>(extension_api);
  ASSERT_NE(xdna_api, nullptr);
  EXPECT_EQ(xdna_api->structure_size, sizeof(amdf_xdna_api_t));
  EXPECT_EQ(xdna_api->extension_version, AMDF_XDNA_EXTENSION_VERSION_1);
  EXPECT_NE(xdna_api->endpoint_query_info, nullptr);
  EXPECT_NE(xdna_api->device_create, nullptr);
  EXPECT_NE(xdna_api->context_query_placement_info, nullptr);
  EXPECT_NE(xdna_api->device_query_info, nullptr);
  EXPECT_NE(xdna_api->context_create, nullptr);
  EXPECT_NE(xdna_api->context_query_info, nullptr);
  EXPECT_NE(xdna_api->context_destroy, nullptr);
}

TEST(XdnaExtensionTest, ReturnsStableImmutableTable) {
  const amdf_api_t* api = QueryApi();
  ASSERT_NE(api, nullptr);

  const amdf_xdna_api_t* first_api = QueryXdnaApi(api);
  const amdf_xdna_api_t* second_api = QueryXdnaApi(api);

  EXPECT_EQ(first_api, second_api);
}

TEST(XdnaExtensionTest, RejectsUnsupportedVersionWithoutPublishingOutput) {
  const amdf_api_t* api = QueryApi();
  ASSERT_NE(api, nullptr);
  const void* const sentinel = reinterpret_cast<const void*>(uintptr_t{1});
  const void* extension_api = sentinel;

  const amdf_status_t status = api->query_extension(
      AMDF_EXTENSION_XDNA, AMDF_XDNA_EXTENSION_VERSION_LATEST + 1,
      AMDF_XDNA_EXTENSION_VERSION_LATEST + 1, &extension_api);

  EXPECT_EQ(amdf_status_domain(status), AMDF_STATUS_DOMAIN_API);
  EXPECT_EQ(amdf_status_code(status), AMDF_STATUS_CODE_VERSION_MISMATCH);
  EXPECT_EQ(extension_api, sentinel);
}

class XdnaEndpointTest : public ::testing::Test {
 protected:
  void SetUp() override {
    api_ = QueryApi();
    ASSERT_NE(api_, nullptr);
    xdna_api_ = QueryXdnaApi(api_);
    ASSERT_NE(xdna_api_, nullptr);

    const amdf_status_t status = GetCtsDeviceCache().GetInstance(&instance_);
    if (amdf_status_domain(status) == AMDF_STATUS_DOMAIN_API &&
        amdf_status_code(status) == AMDF_STATUS_CODE_UNSUPPORTED) {
      GTEST_SKIP() << "platform provider is not implemented";
    }
    ASSERT_TRUE(amdf_status_is_ok(status));
  }

  void TearDown() override {
    if (context_ != nullptr) {
      ASSERT_EQ(xdna_api_->context_destroy(context_), AMDF_STATUS_OK);
      context_ = nullptr;
    }
  }

  amdf_status_t OpenEngine(amdf_engine_kind_t engine_kind,
                           bool* out_engine_found) {
    *out_engine_found = false;
    uint32_t endpoint_count = 0;
    amdf_status_t status =
        api_->endpoint_enumerate(instance_, 0, nullptr, &endpoint_count);
    if (!amdf_status_is_ok(status)) return status;
    std::vector<amdf_endpoint_summary_t> summaries(endpoint_count);
    if (endpoint_count != 0) {
      status = api_->endpoint_enumerate(instance_, endpoint_count,
                                        summaries.data(), &endpoint_count);
      if (!amdf_status_is_ok(status)) return status;
    }
    for (uint32_t ordinal = 0; ordinal < endpoint_count; ++ordinal) {
      const amdf_endpoint_summary_t& summary = summaries[ordinal];
      if (summary.engine_kind == engine_kind) {
        status = GetCtsDeviceCache().OpenEndpoint(summary.id, &endpoint_);
        *out_engine_found = amdf_status_is_ok(status);
        return status;
      }
    }
    return AMDF_STATUS_OK;
  }

  amdf_xdna_device_create_info_t MakeDeviceCreateInfo() {
    amdf_xdna_device_create_info_t create_info = {};
    create_info.type = AMDF_STRUCTURE_TYPE_XDNA_DEVICE_CREATE_INFO;
    create_info.structure_size = sizeof(create_info);
    return create_info;
  }

  amdf_xdna_context_create_info_t MakeContextCreateInfo(
      uint32_t logical_column_count = 1) {
    amdf_xdna_context_create_info_t create_info = {};
    create_info.type = AMDF_STRUCTURE_TYPE_XDNA_CONTEXT_CREATE_INFO;
    create_info.structure_size = sizeof(create_info);
    create_info.logical_column_count = logical_column_count;
    create_info.physical_column_origin = AMDF_XDNA_PHYSICAL_COLUMN_ORIGIN_ANY;
    create_info.acceptable_scheduling_modes =
        AMDF_XDNA_SCHEDULING_MODE_TIME_SLICED;
    return create_info;
  }

  // Core table borrowed from the CTS provider.
  const amdf_api_t* api_ = nullptr;
  // XDNA table borrowed from the CTS provider.
  const amdf_xdna_api_t* xdna_api_ = nullptr;
  // Shared instance used by all device tests.
  amdf_instance_t* instance_ = nullptr;
  // Shared endpoint selected without activating a device.
  amdf_endpoint_t* endpoint_ = nullptr;
  // Shared device materialized only by tests that require one.
  amdf_device_t* device_ = nullptr;
  // Case-owned context for explicit context creation and query coverage.
  amdf_xdna_context_t* context_ = nullptr;
};

TEST_F(XdnaEndpointTest, ReturnsStableCachedProfile) {
  bool engine_found = false;
  ASSERT_EQ(OpenEngine(AMDF_ENGINE_KIND_XDNA, &engine_found), AMDF_STATUS_OK);
  if (!engine_found) {
    GTEST_SKIP() << "no qualified XDNA endpoint present";
  }

  amdf_xdna_endpoint_info_t info = {};
  info.type = AMDF_STRUCTURE_TYPE_XDNA_ENDPOINT_INFO;
  info.structure_size = sizeof(info);
  ASSERT_TRUE(
      amdf_status_is_ok(xdna_api_->endpoint_query_info(endpoint_, &info)));

  EXPECT_EQ(info.type, AMDF_STRUCTURE_TYPE_XDNA_ENDPOINT_INFO);
  EXPECT_EQ(info.structure_size, sizeof(info));
  EXPECT_NE(info.architecture, AMDF_XDNA_ARCHITECTURE_UNKNOWN);
  EXPECT_GT(info.array.column_count, 0u);
  EXPECT_GT(info.array.row_count, 0u);
  EXPECT_GT(info.array.column_stride, 0u);
  EXPECT_GT(info.context.minimum_column_count, 0u);
  EXPECT_LE(info.context.minimum_column_count,
            info.context.maximum_column_count);
  EXPECT_LE(info.context.maximum_column_count, info.array.column_count);
  EXPECT_GT(info.context.column_count_granularity, 0u);
  EXPECT_GE(info.context.maximum_live_context_count,
            info.context.maximum_hardware_context_count);
  EXPECT_NE(info.target_id[0], '\0');
  EXPECT_EQ(info.target_id[AMDF_XDNA_TARGET_ID_CAPACITY - 1], '\0');

  amdf_xdna_endpoint_info_t second_info = {};
  second_info.type = AMDF_STRUCTURE_TYPE_XDNA_ENDPOINT_INFO;
  second_info.structure_size = sizeof(second_info);
  ASSERT_TRUE(amdf_status_is_ok(
      xdna_api_->endpoint_query_info(endpoint_, &second_info)));
  EXPECT_EQ(std::memcmp(&info, &second_info, sizeof(info)), 0);
}

TEST_F(XdnaEndpointTest, RejectsMalformedOutputWithoutMutation) {
  bool engine_found = false;
  ASSERT_EQ(OpenEngine(AMDF_ENGINE_KIND_XDNA, &engine_found), AMDF_STATUS_OK);
  if (!engine_found) {
    GTEST_SKIP() << "no qualified XDNA endpoint present";
  }

  EXPECT_EQ(
      amdf_status_code(xdna_api_->endpoint_query_info(endpoint_, nullptr)),
      AMDF_STATUS_CODE_INVALID_ARGUMENT);

  amdf_xdna_endpoint_info_t info = {};
  info.structure_size = sizeof(info);
  info.architecture = UINT32_MAX;
  EXPECT_EQ(amdf_status_code(xdna_api_->endpoint_query_info(endpoint_, &info)),
            AMDF_STATUS_CODE_INVALID_ARGUMENT);
  EXPECT_EQ(info.architecture, UINT32_MAX);

  info.type = AMDF_STRUCTURE_TYPE_XDNA_ENDPOINT_INFO;
  info.next = &info;
  EXPECT_EQ(amdf_status_code(xdna_api_->endpoint_query_info(endpoint_, &info)),
            AMDF_STATUS_CODE_UNSUPPORTED);
  EXPECT_EQ(info.architecture, UINT32_MAX);
}

TEST_F(XdnaEndpointTest, RejectsGpuEndpointWithoutMutation) {
  bool engine_found = false;
  ASSERT_EQ(OpenEngine(AMDF_ENGINE_KIND_GPU, &engine_found), AMDF_STATUS_OK);
  if (!engine_found) {
    GTEST_SKIP() << "no GPU endpoint present";
  }

  amdf_xdna_endpoint_info_t info = {};
  info.type = AMDF_STRUCTURE_TYPE_XDNA_ENDPOINT_INFO;
  info.structure_size = sizeof(info);
  info.architecture = UINT32_MAX;
  const amdf_status_t status = xdna_api_->endpoint_query_info(endpoint_, &info);

  EXPECT_EQ(amdf_status_domain(status), AMDF_STATUS_DOMAIN_API);
  EXPECT_EQ(amdf_status_code(status), AMDF_STATUS_CODE_UNSUPPORTED);
  EXPECT_EQ(info.architecture, UINT32_MAX);
}

TEST_F(XdnaEndpointTest, ValidatesDeviceCreationArgumentsWithoutNativeWork) {
  bool engine_found = false;
  ASSERT_EQ(OpenEngine(AMDF_ENGINE_KIND_XDNA, &engine_found), AMDF_STATUS_OK);
  if (!engine_found) {
    GTEST_SKIP() << "no qualified XDNA endpoint present";
  }

  amdf_xdna_device_create_info_t create_info = MakeDeviceCreateInfo();
  amdf_device_t* output = reinterpret_cast<amdf_device_t*>(uintptr_t{1});
  EXPECT_EQ(amdf_status_code(
                xdna_api_->device_create(nullptr, &create_info, &output)),
            AMDF_STATUS_CODE_INVALID_ARGUMENT);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(output), uintptr_t{1});
  EXPECT_EQ(
      amdf_status_code(xdna_api_->device_create(endpoint_, nullptr, &output)),
      AMDF_STATUS_CODE_INVALID_ARGUMENT);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(output), uintptr_t{1});
  EXPECT_EQ(amdf_status_code(
                xdna_api_->device_create(endpoint_, &create_info, nullptr)),
            AMDF_STATUS_CODE_INVALID_ARGUMENT);

  create_info.type = AMDF_STRUCTURE_TYPE_NONE;
  EXPECT_EQ(amdf_status_code(
                xdna_api_->device_create(endpoint_, &create_info, &output)),
            AMDF_STATUS_CODE_INVALID_ARGUMENT);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(output), uintptr_t{1});
}

TEST_F(XdnaEndpointTest, ValidatesContextCreationWithoutPublishingOnFailure) {
  bool engine_found = false;
  ASSERT_EQ(OpenEngine(AMDF_ENGINE_KIND_XDNA, &engine_found), AMDF_STATUS_OK);
  if (!engine_found) {
    GTEST_SKIP() << "no qualified XDNA endpoint present";
  }
  const amdf_status_t device_status =
      GetCtsDeviceCache().GetXdnaDevice(endpoint_, &device_);
  if (amdf_status_domain(device_status) == AMDF_STATUS_DOMAIN_API &&
      amdf_status_code(device_status) == AMDF_STATUS_CODE_UNSUPPORTED) {
    GTEST_SKIP() << "XDNA device materialization is unavailable";
  }
  ASSERT_TRUE(amdf_status_is_ok(device_status));

  amdf_xdna_context_create_info_t create_info = MakeContextCreateInfo();
  auto* const sentinel = reinterpret_cast<amdf_xdna_context_t*>(uintptr_t{1});
  amdf_xdna_context_t* output = sentinel;
  EXPECT_EQ(amdf_status_code(
                xdna_api_->context_create(nullptr, &create_info, &output)),
            AMDF_STATUS_CODE_INVALID_ARGUMENT);
  EXPECT_EQ(output, sentinel);
  EXPECT_EQ(
      amdf_status_code(xdna_api_->context_create(device_, nullptr, &output)),
      AMDF_STATUS_CODE_INVALID_ARGUMENT);
  EXPECT_EQ(output, sentinel);
  EXPECT_EQ(amdf_status_code(
                xdna_api_->context_create(device_, &create_info, nullptr)),
            AMDF_STATUS_CODE_INVALID_ARGUMENT);

  create_info.type = AMDF_STRUCTURE_TYPE_NONE;
  EXPECT_EQ(amdf_status_code(
                xdna_api_->context_create(device_, &create_info, &output)),
            AMDF_STATUS_CODE_INVALID_ARGUMENT);
  EXPECT_EQ(output, sentinel);

  create_info = MakeContextCreateInfo();
  create_info.acceptable_scheduling_modes = 0;
  EXPECT_EQ(amdf_status_code(
                xdna_api_->context_create(device_, &create_info, &output)),
            AMDF_STATUS_CODE_INVALID_ARGUMENT);
  EXPECT_EQ(output, sentinel);

  create_info = MakeContextCreateInfo();
  create_info.acceptable_scheduling_modes = UINT32_MAX;
  EXPECT_EQ(amdf_status_code(
                xdna_api_->context_create(device_, &create_info, &output)),
            AMDF_STATUS_CODE_INVALID_ARGUMENT);
  EXPECT_EQ(output, sentinel);
}

TEST_F(XdnaEndpointTest, MaterializesDeviceAndProgramIndependentContext) {
  bool engine_found = false;
  ASSERT_EQ(OpenEngine(AMDF_ENGINE_KIND_XDNA, &engine_found), AMDF_STATUS_OK);
  if (!engine_found) {
    GTEST_SKIP() << "no qualified XDNA endpoint present";
  }
  amdf_xdna_endpoint_info_t endpoint_info = {};
  endpoint_info.type = AMDF_STRUCTURE_TYPE_XDNA_ENDPOINT_INFO;
  endpoint_info.structure_size = sizeof(endpoint_info);
  ASSERT_TRUE(amdf_status_is_ok(
      xdna_api_->endpoint_query_info(endpoint_, &endpoint_info)));
  const amdf_status_t create_status =
      GetCtsDeviceCache().GetXdnaDevice(endpoint_, &device_);
  if (amdf_status_domain(create_status) == AMDF_STATUS_DOMAIN_API &&
      amdf_status_code(create_status) == AMDF_STATUS_CODE_UNSUPPORTED) {
    GTEST_SKIP() << "XDNA device materialization is unavailable";
  }
  ASSERT_TRUE(amdf_status_is_ok(create_status))
      << "domain=" << amdf_status_domain(create_status)
      << " code=" << amdf_status_code(create_status);
  ASSERT_NE(device_, nullptr);

  amdf_xdna_device_info_t info = {};
  info.type = AMDF_STRUCTURE_TYPE_XDNA_DEVICE_INFO;
  info.structure_size = sizeof(info);
  ASSERT_TRUE(amdf_status_is_ok(xdna_api_->device_query_info(device_, &info)));
  EXPECT_NE(info.id.words[0] | info.id.words[1], 0u);
  EXPECT_EQ(info.reset_epoch, 1u);

  amdf_xdna_device_info_t second_info = {};
  second_info.type = AMDF_STRUCTURE_TYPE_XDNA_DEVICE_INFO;
  second_info.structure_size = sizeof(second_info);
  ASSERT_TRUE(
      amdf_status_is_ok(xdna_api_->device_query_info(device_, &second_info)));
  EXPECT_EQ(std::memcmp(&info, &second_info, sizeof(info)), 0);

  amdf_xdna_endpoint_info_t repeated_endpoint_info = {};
  repeated_endpoint_info.type = AMDF_STRUCTURE_TYPE_XDNA_ENDPOINT_INFO;
  repeated_endpoint_info.structure_size = sizeof(repeated_endpoint_info);
  ASSERT_TRUE(amdf_status_is_ok(
      xdna_api_->endpoint_query_info(endpoint_, &repeated_endpoint_info)));
  EXPECT_EQ(std::memcmp(&endpoint_info, &repeated_endpoint_info,
                        sizeof(endpoint_info)),
            0);

  const amdf_xdna_context_create_info_t context_create_info =
      MakeContextCreateInfo();
  if ((endpoint_info.context.scheduling_modes &
       context_create_info.acceptable_scheduling_modes) == 0) {
    auto* const sentinel = reinterpret_cast<amdf_xdna_context_t*>(uintptr_t{1});
    amdf_xdna_context_t* output = sentinel;
    EXPECT_EQ(amdf_status_code(xdna_api_->context_create(
                  device_, &context_create_info, &output)),
              AMDF_STATUS_CODE_UNSUPPORTED);
    EXPECT_EQ(output, sentinel);
    return;
  }
  const amdf_xdna_context_create_info_t invalid_context_create_info =
      MakeContextCreateInfo(0);
  auto* const sentinel = reinterpret_cast<amdf_xdna_context_t*>(uintptr_t{1});
  amdf_xdna_context_t* output = sentinel;
  EXPECT_EQ(amdf_status_code(xdna_api_->context_create(
                device_, &invalid_context_create_info, &output)),
            AMDF_STATUS_CODE_OUT_OF_RANGE);
  EXPECT_EQ(output, sentinel);
  ASSERT_TRUE(amdf_status_is_ok(
      xdna_api_->context_create(device_, &context_create_info, &context_)));
  amdf_xdna_context_info_t context_info = {};
  context_info.type = AMDF_STRUCTURE_TYPE_XDNA_CONTEXT_INFO;
  context_info.structure_size = sizeof(context_info);
  ASSERT_TRUE(amdf_status_is_ok(
      xdna_api_->context_query_info(context_, &context_info)));
  EXPECT_NE(context_info.id.words[0] | context_info.id.words[1], 0u);
  EXPECT_TRUE(amdf_device_id_is_equal(&context_info.device_id, &info.id));
  EXPECT_EQ(context_info.reset_epoch, info.reset_epoch);
  EXPECT_EQ(context_info.scheduling_mode,
            AMDF_XDNA_SCHEDULING_MODE_TIME_SLICED);
  EXPECT_EQ(context_info.logical_column_count,
            context_create_info.logical_column_count);
  EXPECT_EQ(context_info.row_count, endpoint_info.array.row_count);

  amdf_xdna_context_placement_info_t placement = {};
  placement.type = AMDF_STRUCTURE_TYPE_XDNA_CONTEXT_PLACEMENT_INFO;
  placement.structure_size = sizeof(placement);
  placement.column_origin = UINT32_MAX;
  placement.column_count = UINT32_MAX;
  const amdf_xdna_context_placement_info_t original_placement = placement;
  const amdf_status_t placement_status =
      xdna_api_->context_query_placement_info(context_, &placement);
  if (info.placement_modes & AMDF_XDNA_PLACEMENT_MODE_FIXED_FULL_ARRAY) {
    ASSERT_TRUE(amdf_status_is_ok(placement_status));
    EXPECT_EQ(placement.column_origin, endpoint_info.array.column_origin);
    EXPECT_EQ(placement.column_count, endpoint_info.array.column_count);
    EXPECT_GE(placement.column_count, context_info.logical_column_count);
    amdf_xdna_context_placement_info_t repeated_placement = original_placement;
    ASSERT_TRUE(amdf_status_is_ok(xdna_api_->context_query_placement_info(
        context_, &repeated_placement)));
    EXPECT_EQ(std::memcmp(&placement, &repeated_placement, sizeof(placement)),
              0);
  } else {
    EXPECT_EQ(amdf_status_code(placement_status), AMDF_STATUS_CODE_UNSUPPORTED);
    EXPECT_EQ(std::memcmp(&placement, &original_placement, sizeof(placement)),
              0);
  }

  // An explicit request must obey the effective native contract. Creating a
  // sibling also checks that fixed backing does not promise exclusive
  // ownership.
  amdf_xdna_context_create_info_t fixed_create_info = MakeContextCreateInfo();
  fixed_create_info.physical_column_origin = endpoint_info.array.column_origin;
  if (info.placement_modes & AMDF_XDNA_PLACEMENT_MODE_FIXED_FULL_ARRAY) {
    amdf_xdna_context_t* fixed_context = nullptr;
    ASSERT_TRUE(amdf_status_is_ok(xdna_api_->context_create(
        device_, &fixed_create_info, &fixed_context)));
    amdf_xdna_context_placement_info_t fixed_placement = original_placement;
    EXPECT_TRUE(amdf_status_is_ok(xdna_api_->context_query_placement_info(
        fixed_context, &fixed_placement)));
    EXPECT_EQ(fixed_placement.column_origin, endpoint_info.array.column_origin);
    EXPECT_EQ(fixed_placement.column_count, endpoint_info.array.column_count);
    EXPECT_TRUE(amdf_status_is_ok(xdna_api_->context_destroy(fixed_context)));
  } else {
    auto* const sentinel = reinterpret_cast<amdf_xdna_context_t*>(uintptr_t{1});
    amdf_xdna_context_t* output = sentinel;
    EXPECT_EQ(amdf_status_code(xdna_api_->context_create(
                  device_, &fixed_create_info, &output)),
              AMDF_STATUS_CODE_UNSUPPORTED);
    EXPECT_EQ(output, sentinel);
  }
  if (endpoint_info.array.column_count > 1) {
    fixed_create_info.physical_column_origin =
        endpoint_info.array.column_origin + 1;
    auto* const sentinel = reinterpret_cast<amdf_xdna_context_t*>(uintptr_t{1});
    amdf_xdna_context_t* output = sentinel;
    EXPECT_EQ(amdf_status_code(xdna_api_->context_create(
                  device_, &fixed_create_info, &output)),
              AMDF_STATUS_CODE_UNSUPPORTED);
    EXPECT_EQ(output, sentinel);
  }

  EXPECT_EQ(amdf_status_code(api_->endpoint_close(endpoint_)),
            AMDF_STATUS_CODE_BUSY);
}

TEST_F(XdnaEndpointTest, RejectsMalformedDeviceInfoWithoutMutation) {
  bool engine_found = false;
  ASSERT_EQ(OpenEngine(AMDF_ENGINE_KIND_XDNA, &engine_found), AMDF_STATUS_OK);
  if (!engine_found) {
    GTEST_SKIP() << "no qualified XDNA endpoint present";
  }
  const amdf_status_t create_status =
      GetCtsDeviceCache().GetXdnaDevice(endpoint_, &device_);
  if (amdf_status_domain(create_status) == AMDF_STATUS_DOMAIN_API &&
      amdf_status_code(create_status) == AMDF_STATUS_CODE_UNSUPPORTED) {
    GTEST_SKIP() << "XDNA device materialization is unavailable";
  }
  ASSERT_TRUE(amdf_status_is_ok(create_status))
      << "domain=" << amdf_status_domain(create_status)
      << " code=" << amdf_status_code(create_status);

  EXPECT_EQ(amdf_status_code(xdna_api_->device_query_info(device_, nullptr)),
            AMDF_STATUS_CODE_INVALID_ARGUMENT);
  EXPECT_EQ(amdf_status_code(xdna_api_->device_query_info(nullptr, nullptr)),
            AMDF_STATUS_CODE_INVALID_ARGUMENT);

  amdf_xdna_device_info_t info = {};
  info.structure_size = sizeof(info);
  info.reset_epoch = UINT64_MAX;
  EXPECT_EQ(amdf_status_code(xdna_api_->device_query_info(device_, &info)),
            AMDF_STATUS_CODE_INVALID_ARGUMENT);
  EXPECT_EQ(info.reset_epoch, UINT64_MAX);

  info.type = AMDF_STRUCTURE_TYPE_XDNA_DEVICE_INFO;
  info.next = &info;
  EXPECT_EQ(amdf_status_code(xdna_api_->device_query_info(device_, &info)),
            AMDF_STATUS_CODE_UNSUPPORTED);
  EXPECT_EQ(info.reset_epoch, UINT64_MAX);
}

using XdnaContextTest = XdnaContextFixture;

TEST_F(XdnaContextTest, RejectsMalformedContextInfoWithoutMutation) {
  EXPECT_EQ(amdf_status_code(xdna_api_->context_query_info(context_, nullptr)),
            AMDF_STATUS_CODE_INVALID_ARGUMENT);
  EXPECT_EQ(amdf_status_code(
                xdna_api_->context_query_placement_info(context_, nullptr)),
            AMDF_STATUS_CODE_INVALID_ARGUMENT);
  EXPECT_EQ(amdf_status_code(
                xdna_api_->context_query_placement_info(nullptr, nullptr)),
            AMDF_STATUS_CODE_INVALID_ARGUMENT);

  amdf_xdna_context_placement_info_t placement = {};
  placement.structure_size = sizeof(placement);
  placement.column_count = UINT32_MAX;
  EXPECT_EQ(amdf_status_code(
                xdna_api_->context_query_placement_info(context_, &placement)),
            AMDF_STATUS_CODE_INVALID_ARGUMENT);
  EXPECT_EQ(placement.column_count, UINT32_MAX);
  placement.type = AMDF_STRUCTURE_TYPE_XDNA_CONTEXT_PLACEMENT_INFO;
  placement.next = &placement;
  EXPECT_EQ(amdf_status_code(
                xdna_api_->context_query_placement_info(context_, &placement)),
            AMDF_STATUS_CODE_UNSUPPORTED);
  EXPECT_EQ(placement.column_count, UINT32_MAX);

  amdf_xdna_context_info_t info = {};
  info.structure_size = sizeof(info);
  info.reset_epoch = UINT64_MAX;
  EXPECT_EQ(amdf_status_code(xdna_api_->context_query_info(context_, &info)),
            AMDF_STATUS_CODE_INVALID_ARGUMENT);
  EXPECT_EQ(info.reset_epoch, UINT64_MAX);

  info.type = AMDF_STRUCTURE_TYPE_XDNA_CONTEXT_INFO;
  info.next = &info;
  EXPECT_EQ(amdf_status_code(xdna_api_->context_query_info(context_, &info)),
            AMDF_STATUS_CODE_UNSUPPORTED);
  EXPECT_EQ(info.reset_epoch, UINT64_MAX);
}

TEST_F(XdnaEndpointTest, ValidatesDeviceDestructionArguments) {
  EXPECT_EQ(amdf_status_code(api_->device_destroy(nullptr)),
            AMDF_STATUS_CODE_INVALID_ARGUMENT);
}

}  // namespace
