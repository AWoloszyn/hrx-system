// Copyright 2026 The HRX Authors
// SPDX-License-Identifier: Apache-2.0

#include "device_selection.h"

#include <array>

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace {

using DeviceSelectionTest = ::testing::Test;

const std::array<iree_hal_device_info_t, 5> kDeviceInfos = {{
    {/*.device_id=*/0, /*.path=*/IREE_SV(""), /*.name=*/IREE_SV("all")},
    {/*.device_id=*/1, /*.path=*/IREE_SV("GPU-1111111111111111"),
     /*.name=*/IREE_SV("first")},
    {/*.device_id=*/2, /*.path=*/IREE_SV("GPU-2222222222222222"),
     /*.name=*/IREE_SV("second")},
    {/*.device_id=*/4, /*.path=*/IREE_SV("GPU-3333333333333333"),
     /*.name=*/IREE_SV("third")},
    {/*.device_id=*/8, /*.path=*/IREE_SV("GPU-4444444444444444"),
     /*.name=*/IREE_SV("fourth")},
}};

static std::array<iree_host_size_t, 4> Resolve(iree_string_view_t selector,
                                               iree_host_size_t* out_count) {
  std::array<iree_host_size_t, 4> indices = {};
  IREE_EXPECT_OK(hrx_gpu_resolve_device_selection(
      selector, kDeviceInfos.size(), kDeviceInfos.data(), indices.size(),
      out_count, indices.data()));
  return indices;
}

TEST_F(DeviceSelectionTest, EmptySelectsAllPhysicalDevices) {
  iree_host_size_t count = 0;
  const auto indices = Resolve(IREE_SV(""), &count);
  EXPECT_EQ(count, 4u);
  EXPECT_EQ(indices, (std::array<iree_host_size_t, 4>{1, 2, 3, 4}));
}

TEST_F(DeviceSelectionTest, PreservesMixedSelectorOrder) {
  iree_host_size_t count = 0;
  const auto indices = Resolve(IREE_SV("2,GPU-2222222222222222,0"), &count);
  EXPECT_EQ(count, 3u);
  EXPECT_EQ(indices[0], 3u);
  EXPECT_EQ(indices[1], 2u);
  EXPECT_EQ(indices[2], 1u);
}

TEST_F(DeviceSelectionTest, SuppressesDuplicatePhysicalDevices) {
  iree_host_size_t count = 0;
  const auto indices =
      Resolve(IREE_SV("0,GPU-3333333333333333,GPU-1111111111111111,0"), &count);
  EXPECT_EQ(count, 2u);
  EXPECT_EQ(indices[0], 1u);
  EXPECT_EQ(indices[1], 3u);
}

TEST_F(DeviceSelectionTest, InvalidSelectorTruncatesTheList) {
  iree_host_size_t count = 0;
  const auto indices = Resolve(IREE_SV("1,invalid,0"), &count);
  EXPECT_EQ(count, 1u);
  EXPECT_EQ(indices[0], 2u);
}

TEST_F(DeviceSelectionTest, NonCanonicalOrdinalIsInvalid) {
  iree_host_size_t count = 0;
  Resolve(IREE_SV("01,2"), &count);
  EXPECT_EQ(count, 0u);
}

TEST_F(DeviceSelectionTest, MatchingDevicePathsIsCaseInsensitive) {
  iree_host_size_t count = 0;
  const auto indices = Resolve(IREE_SV("gpu-4444444444444444"), &count);
  EXPECT_EQ(count, 1u);
  EXPECT_EQ(indices[0], 4u);
}

}  // namespace
