// Copyright 2026 The HRX Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "binding/hip/peer.h"

#include <cstdint>
#include <limits>

#include "iree/testing/gtest.h"

namespace {

TEST(HipPeerTest, MapsKnownLinkTypesToNativeValues) {
  struct TestCase {
    iree_hal_topology_link_type_t topology_link_type;
    int native_link_type;
  };
  const TestCase test_cases[] = {
      {IREE_HAL_TOPOLOGY_LINK_TYPE_HYPERTRANSPORT, 0},
      {IREE_HAL_TOPOLOGY_LINK_TYPE_QPI, 1},
      {IREE_HAL_TOPOLOGY_LINK_TYPE_PCIE, 2},
      {IREE_HAL_TOPOLOGY_LINK_TYPE_INFINIBAND, 3},
      {IREE_HAL_TOPOLOGY_LINK_TYPE_XGMI, 4},
  };

  for (const TestCase& test_case : test_cases) {
    int performance_rank = -2;
    EXPECT_TRUE(iree_hip_peer_performance_rank_from_topology(
        test_case.topology_link_type, &performance_rank));
    EXPECT_EQ(performance_rank, test_case.native_link_type);

    uint32_t link_type = std::numeric_limits<uint32_t>::max();
    uint32_t hop_count = 0;
    EXPECT_TRUE(iree_hip_peer_link_info_from_topology(
        test_case.topology_link_type, /*topology_hop_count=*/7, &link_type,
        &hop_count));
    EXPECT_EQ(link_type, static_cast<uint32_t>(test_case.native_link_type));
    EXPECT_EQ(hop_count, 7u);
  }
}

TEST(HipPeerTest, MapsUnknownLinkToSuccessfulNoLinkSentinels) {
  int performance_rank = 0;
  EXPECT_TRUE(iree_hip_peer_performance_rank_from_topology(
      IREE_HAL_TOPOLOGY_LINK_TYPE_UNKNOWN, &performance_rank));
  EXPECT_EQ(performance_rank, -1);

  uint32_t link_type = 0;
  uint32_t hop_count = 1;
  EXPECT_TRUE(iree_hip_peer_link_info_from_topology(
      IREE_HAL_TOPOLOGY_LINK_TYPE_UNKNOWN, /*topology_hop_count=*/0, &link_type,
      &hop_count));
  EXPECT_EQ(link_type, std::numeric_limits<uint32_t>::max());
  EXPECT_EQ(hop_count, 0u);
}

TEST(HipPeerTest, RejectsInvalidTopologyLinkWithoutChangingOutputs) {
  const iree_hal_topology_link_type_t invalid_link_type =
      static_cast<iree_hal_topology_link_type_t>(
          IREE_HAL_TOPOLOGY_LINK_TYPE_XGMI + 1);

  int performance_rank = 123;
  EXPECT_FALSE(iree_hip_peer_performance_rank_from_topology(invalid_link_type,
                                                            &performance_rank));
  EXPECT_EQ(performance_rank, 123);

  uint32_t link_type = 456;
  uint32_t hop_count = 789;
  EXPECT_FALSE(iree_hip_peer_link_info_from_topology(
      invalid_link_type, /*topology_hop_count=*/9, &link_type, &hop_count));
  EXPECT_EQ(link_type, 456u);
  EXPECT_EQ(hop_count, 789u);
}

}  // namespace
