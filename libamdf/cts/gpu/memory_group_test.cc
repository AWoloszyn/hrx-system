// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <array>
#include <cstring>
#include <vector>

#include "gpu_device_fixture.h"

namespace {

class GpuMemoryGroupTest : public GpuDeviceFixture {
 protected:
  void TearDown() override {
    if (mapping_ != nullptr) {
      ASSERT_EQ(api_->host_mapping_destroy(mapping_), AMDF_STATUS_OK);
      mapping_ = nullptr;
    }
    if (memory_ != nullptr) {
      ASSERT_EQ(api_->memory_destroy(memory_), AMDF_STATUS_OK);
      memory_ = nullptr;
    }
    GpuDeviceFixture::TearDown();
  }

  // Complete group allocation owned by this case, not by the device cache.
  amdf_memory_t* memory_ = nullptr;
  // Explicit host view released before its backing.
  amdf_host_mapping_t* mapping_ = nullptr;
};

TEST_F(GpuMemoryGroupTest, AllocatesOneSystemBackingForTwoPhysicalConsumers) {
  uint32_t endpoint_count = 0;
  ASSERT_EQ(api_->endpoint_enumerate(instance_, 0, nullptr, &endpoint_count),
            AMDF_STATUS_OK);
  std::vector<amdf_endpoint_summary_t> summaries(endpoint_count);
  ASSERT_EQ(api_->endpoint_enumerate(instance_, endpoint_count,
                                     summaries.data(), &endpoint_count),
            AMDF_STATUS_OK);
  amdf_endpoint_t* peer_endpoint = nullptr;
  for (const auto& summary : summaries) {
    if (summary.engine_kind != AMDF_ENGINE_KIND_GPU) continue;
    amdf_endpoint_t* candidate = nullptr;
    ASSERT_EQ(GetCtsDeviceCache().OpenEndpoint(summary.id, &candidate),
              AMDF_STATUS_OK);
    if (candidate == endpoint_) continue;
    peer_endpoint = candidate;
    break;
  }
  if (peer_endpoint == nullptr) GTEST_SKIP() << "requires two physical GPUs";

  const amdf_memory_access_requirements_t requirements = {
      .access = AMDF_MEMORY_ACCESS_READ | AMDF_MEMORY_ACCESS_WRITE,
      .flags = AMDF_MEMORY_FLAG_DEVICE_ADDRESS,
      .address_kinds = UINT64_C(1) << AMDF_MEMORY_ADDRESS_GPU,
  };
  const std::array<amdf_memory_endpoint_access_t, 2> expected_accesses = {
      {{endpoint_, requirements}, {peer_endpoint, requirements}}};
  amdf_memory_scope_info_t scope_info = {};
  scope_info.type = AMDF_STRUCTURE_TYPE_MEMORY_SCOPE_INFO;
  scope_info.structure_size = sizeof(scope_info);
  ASSERT_EQ(api_->memory_scope_query_info(system_scope_, &scope_info),
            AMDF_STATUS_OK);
  amdf_memory_profile_t profile = {};
  profile.type = AMDF_STRUCTURE_TYPE_MEMORY_PROFILE;
  profile.structure_size = sizeof(profile);
  std::array<amdf_memory_access_capabilities_t, 2> capabilities = {};
  for (auto& capability : capabilities) {
    capability.type = AMDF_STRUCTURE_TYPE_MEMORY_ACCESS_CAPABILITIES;
    capability.structure_size = sizeof(capability);
  }
  uint32_t selected = AMDF_MEMORY_PROFILE_ORDINAL_UNKNOWN;
  for (uint32_t i = 0; i < scope_info.memory_profile_count; ++i) {
    const amdf_status_t status = api_->memory_scope_query_profile(
        system_scope_, i, expected_accesses.size(), expected_accesses.data(),
        &profile, capabilities.data());
    if (status == amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED)) continue;
    ASSERT_EQ(status, AMDF_STATUS_OK);
    if ((profile.roles & (AMDF_MEMORY_PROFILE_ROLE_CREATE |
                          AMDF_MEMORY_PROFILE_ROLE_HOST_MAP)) !=
        (AMDF_MEMORY_PROFILE_ROLE_CREATE | AMDF_MEMORY_PROFILE_ROLE_HOST_MAP)) {
      continue;
    }
    if ((profile.supported_flags & AMDF_MEMORY_FLAG_HOST_VISIBLE) == 0)
      continue;
    selected = i;
    break;
  }
  if (selected == AMDF_MEMORY_PROFILE_ORDINAL_UNKNOWN) {
    GTEST_SKIP() << "no joint allocation profile for this physical pair";
  }
  amdf_device_t* peer_device = nullptr;
  const amdf_status_t activation =
      GetCtsDeviceCache().GetGpuDevice(peer_endpoint, &peer_device);
  if (activation == amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED)) {
    GTEST_SKIP() << "peer activation is unavailable for this native lifetime";
  }
  ASSERT_EQ(activation, AMDF_STATUS_OK);
  const std::array<amdf_memory_device_access_t, 2> accesses = {
      {{device_, requirements}, {peer_device, requirements}}};
  ASSERT_EQ(api_->memory_scope_query_device_profile(
                system_scope_, selected, accesses.size(), accesses.data(),
                &profile, capabilities.data()),
            AMDF_STATUS_OK);
  amdf_memory_create_info_t create_info = {};
  create_info.type = AMDF_STRUCTURE_TYPE_MEMORY_CREATE_INFO;
  create_info.structure_size = sizeof(create_info);
  create_info.memory_profile_ordinal = selected;
  create_info.access_count = accesses.size();
  create_info.accesses = accesses.data();
  create_info.required_flags = AMDF_MEMORY_FLAG_HOST_VISIBLE;
  create_info.byte_length = profile.allocation.native_byte_length_granularity;
  create_info.minimum_alignment = profile.allocation.minimum_alignment;
  ASSERT_NE(create_info.byte_length, 0u);
  ASSERT_LE(create_info.byte_length, profile.allocation.maximum_byte_length);
  ASSERT_EQ(api_->memory_create(system_scope_, &create_info, &memory_),
            AMDF_STATUS_OK);
  amdf_memory_info_t memory_info = {};
  memory_info.type = AMDF_STRUCTURE_TYPE_MEMORY_INFO;
  memory_info.structure_size = sizeof(memory_info);
  ASSERT_EQ(api_->memory_query_info(memory_, &memory_info), AMDF_STATUS_OK);
  EXPECT_EQ(memory_info.access_count, accesses.size());
  EXPECT_EQ(memory_info.byte_length, create_info.byte_length);
  for (uint32_t i = 0; i < accesses.size(); ++i) {
    amdf_memory_access_info_t access_info = {};
    access_info.type = AMDF_STRUCTURE_TYPE_MEMORY_ACCESS_INFO;
    access_info.structure_size = sizeof(access_info);
    ASSERT_EQ(api_->memory_query_access_info(memory_, i, &access_info),
              AMDF_STATUS_OK);
    EXPECT_EQ(access_info.ordinal, i);
    EXPECT_EQ(access_info.access, requirements.access);
    uint64_t address = 0;
    ASSERT_EQ(api_->memory_query_address(memory_, i, AMDF_MEMORY_ADDRESS_GPU,
                                         &address),
              AMDF_STATUS_OK);
    EXPECT_GE(address, capabilities[i].device_address.minimum_address);
    EXPECT_LE(address, capabilities[i].device_address.maximum_address);
    EXPECT_EQ(address % capabilities[i].device_address.minimum_alignment, 0u);
  }
  amdf_memory_map_info_t map_info = {};
  map_info.type = AMDF_STRUCTURE_TYPE_MEMORY_MAP_INFO;
  map_info.structure_size = sizeof(map_info);
  map_info.flags = AMDF_MEMORY_MAP_FLAG_READ | AMDF_MEMORY_MAP_FLAG_WRITE;
  map_info.byte_length = create_info.byte_length;
  ASSERT_EQ(api_->memory_map(memory_, &map_info, &mapping_), AMDF_STATUS_OK);
  amdf_host_mapping_info_t mapping_info = {};
  mapping_info.type = AMDF_STRUCTURE_TYPE_HOST_MAPPING_INFO;
  mapping_info.structure_size = sizeof(mapping_info);
  ASSERT_EQ(api_->host_mapping_query_info(mapping_, &mapping_info),
            AMDF_STATUS_OK);
  std::memset(mapping_info.pointer, 0x5A, mapping_info.byte_length);
}

}  // namespace
