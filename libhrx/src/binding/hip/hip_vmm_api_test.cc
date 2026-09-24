// Copyright 2026 The HRX Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <cstddef>

#include "binding/hip/api.h"
#include "binding/hip/hip_dso_test_util.h"
#include "iree/testing/gtest.h"

namespace {

using HipInitFn = hipError_t (*)(unsigned int flags);
using HipMemCreateFn = hipError_t (*)(hipMemGenericAllocationHandle_t* handle,
                                      size_t size,
                                      const hipMemAllocationProp* properties,
                                      unsigned long long flags);
using HipMemReleaseFn = hipError_t (*)(hipMemGenericAllocationHandle_t handle);
using HipMemGetAllocationGranularityFn =
    hipError_t (*)(size_t* granularity, const hipMemAllocationProp* properties,
                   hipMemAllocationGranularity_flags option);

TEST(HipVmmApiTest, UncachedHostAllocationUsesDefaultPhysicalPlacement) {
  hrx::hip::testing::HipDso dso;
  ASSERT_TRUE(dso.Open()) << dso.error();
  HipInitFn init = dso.Resolve<HipInitFn>("hipInit");
  HipMemCreateFn create = dso.Resolve<HipMemCreateFn>("hipMemCreate");
  HipMemReleaseFn release = dso.Resolve<HipMemReleaseFn>("hipMemRelease");
  HipMemGetAllocationGranularityFn get_granularity =
      dso.Resolve<HipMemGetAllocationGranularityFn>(
          "hipMemGetAllocationGranularity");
  ASSERT_NE(nullptr, init) << dso.error();
  ASSERT_NE(nullptr, create) << dso.error();
  ASSERT_NE(nullptr, release) << dso.error();
  ASSERT_NE(nullptr, get_granularity) << dso.error();
  ASSERT_EQ(hipSuccess, init(/*flags=*/0));

  // AMDGPU cannot satisfy device-uncached semantics with host-owned physical
  // backing, but the same host placement is valid with its default cache
  // policy. This combination therefore deterministically requires fallback.
  hipMemAllocationProp properties = {};
  properties.type = hipMemAllocationTypeUncached;
  properties.requestedHandleType = hipMemHandleTypeNone;
  properties.location.type = hipMemLocationTypeHost;
  properties.location.id = 0;

  size_t allocation_size = 0;
  ASSERT_EQ(hipSuccess,
            get_granularity(&allocation_size, &properties,
                            hipMemAllocationGranularityRecommended));
  ASSERT_NE(0u, allocation_size);

  hipMemGenericAllocationHandle_t handle = nullptr;
  ASSERT_EQ(hipSuccess,
            create(&handle, allocation_size, &properties, /*flags=*/0));
  ASSERT_NE(nullptr, handle);
  EXPECT_EQ(hipSuccess, release(handle));
}

}  // namespace
