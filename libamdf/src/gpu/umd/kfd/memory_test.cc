// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/licenses/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/gpu/umd/memory.h"

#include "gtest/gtest.h"
#include "libamdf/src/gpu/umd/kfd/device.h"

namespace {

static amdf_memory_profile_t QueryProfile(amdf_gpu_umd_device_t* device,
                                          uint32_t ordinal) {
  amdf_memory_profile_t profile = {};
  profile.type = AMDF_STRUCTURE_TYPE_MEMORY_PROFILE;
  profile.structure_size = sizeof(profile);
  EXPECT_EQ(amdf_gpu_umd_device_query_memory_profile(device, ordinal, &profile),
            AMDF_STATUS_OK);
  return profile;
}

TEST(LinuxGpuMemoryProfileTest, IndependentDeviceExposesOwnedSystemMemory) {
  amdf_gpu_umd_device_t device = {};
  device.mode = AMDF_GPU_DEVICE_MODE_INDEPENDENT;
  device.page_size = 4096;
  device.topology.virtual_address.begin = UINT64_C(0x10000);
  device.topology.virtual_address.end = UINT64_C(1) << 48;

  const amdf_memory_profile_t profile = QueryProfile(&device, 0);
  EXPECT_EQ(profile.ordinal, 0u);
  EXPECT_EQ(profile.memory_class, AMDF_MEMORY_CLASS_SYSTEM);
  EXPECT_EQ(profile.roles, AMDF_MEMORY_PROFILE_ROLE_CREATE |
                               AMDF_MEMORY_PROFILE_ROLE_EXPORT |
                               AMDF_MEMORY_PROFILE_ROLE_HOST_MAP);
  EXPECT_EQ(profile.guaranteed_flags, AMDF_MEMORY_FLAG_HOST_VISIBLE |
                                          AMDF_MEMORY_FLAG_SHAREABLE |
                                          AMDF_MEMORY_FLAG_HOST_COHERENT |
                                          AMDF_MEMORY_FLAG_DEVICE_ADDRESS);
  EXPECT_EQ(profile.guaranteed_device_access, AMDF_MEMORY_ACCESS_READ);
  EXPECT_EQ(profile.supported_device_access, AMDF_MEMORY_ACCESS_READ |
                                                 AMDF_MEMORY_ACCESS_WRITE |
                                                 AMDF_MEMORY_ACCESS_EXECUTE);
  EXPECT_EQ(profile.allocation.minimum_alignment, 4096u);
  EXPECT_EQ(profile.allocation.byte_length_granularity, 1u);
  ASSERT_EQ(profile.external_memory_support_count, 1u);
  EXPECT_EQ(profile.external_memory_support[0].type,
            AMDF_EXTERNAL_MEMORY_TYPE_DMA_BUF_FD);
  EXPECT_EQ(profile.external_memory_support[0].flags,
            AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_EXPORT |
                AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_SOURCE_OFFSET |
                AMDF_EXTERNAL_MEMORY_SUPPORT_FLAG_CROSS_PROCESS);

  amdf_memory_profile_t unavailable = {};
  unavailable.type = AMDF_STRUCTURE_TYPE_MEMORY_PROFILE;
  unavailable.structure_size = sizeof(unavailable);
  unavailable.ordinal = UINT32_MAX;
  EXPECT_EQ(amdf_status_code(amdf_gpu_umd_device_query_memory_profile(
                &device, 1, &unavailable)),
            AMDF_STATUS_CODE_OUT_OF_RANGE);
  EXPECT_EQ(unavailable.ordinal, UINT32_MAX);
}

TEST(LinuxGpuMemoryProfileTest, ProcessDeviceUsesDenseOptionalProfiles) {
  amdf_gpu_umd_device_t device = {};
  device.mode = AMDF_GPU_DEVICE_MODE_PROCESS;
  device.page_size = 4096;
  device.topology.virtual_address.begin = UINT64_C(0x10000);
  device.topology.virtual_address.end = UINT64_C(1) << 48;
  device.topology.memory_features =
      AMDF_GPU_DEVICE_FEATURE_LOCAL_MEMORY |
      AMDF_GPU_DEVICE_FEATURE_HOST_VISIBLE_LOCAL_MEMORY;

  const amdf_memory_profile_t local_profile = QueryProfile(&device, 1);
  EXPECT_EQ(local_profile.memory_class, AMDF_MEMORY_CLASS_LOCAL);
  EXPECT_NE(local_profile.roles & AMDF_MEMORY_PROFILE_ROLE_HOST_MAP, 0u);
  EXPECT_NE(local_profile.supported_flags & AMDF_MEMORY_FLAG_HOST_VISIBLE, 0u);

  const amdf_memory_profile_t registered_profile = QueryProfile(&device, 2);
  EXPECT_EQ(registered_profile.memory_class, AMDF_MEMORY_CLASS_REGISTERED_HOST);
  EXPECT_EQ(registered_profile.roles, AMDF_MEMORY_PROFILE_ROLE_REGISTER |
                                          AMDF_MEMORY_PROFILE_ROLE_HOST_MAP);
  EXPECT_EQ(registered_profile.registration.minimum_alignment, 1u);
  EXPECT_EQ(registered_profile.registration.registered_host_pointer_alignment,
            1u);

  device.topology.memory_features = 0;
  const amdf_memory_profile_t dense_registered_profile =
      QueryProfile(&device, 1);
  EXPECT_EQ(dense_registered_profile.memory_class,
            AMDF_MEMORY_CLASS_REGISTERED_HOST);
}

}  // namespace
