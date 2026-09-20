// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/carrier/shm/bootstrap.h"

#include <array>
#include <cstring>

#include "iree/base/alignment.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace {

constexpr std::array<uint8_t, 20> kOffer = {
    'I', 'R', 'S', 'H', 1, 0, 1, 0, 4, 0, 0, 0, 16, 0, 0, 0, 0, 0, 1, 0,
};

TEST(ShmBootstrapTest, CanonicalOfferDerivesMappingGeometry) {
  iree_net_shm_region_layout_t layout;
  IREE_ASSERT_OK(iree_net_shm_bootstrap_decode_offer(
      iree_make_const_byte_span(kOffer.data(), kOffer.size()), &layout));
  EXPECT_EQ(layout.options.endpoint_count, 4);
  EXPECT_EQ(layout.options.slot_count, 16);
  EXPECT_EQ(layout.options.slot_capacity, 65536);
  EXPECT_EQ(layout.total_size, 128 + 8 * (192 + 256 + 512 + 16 * 65536));
  std::array<uint8_t, IREE_NET_SHM_BOOTSTRAP_OFFER_SIZE> encoded;
  iree_net_shm_bootstrap_encode_offer(&layout, encoded.data());
  EXPECT_EQ(encoded, kOffer);
}

TEST(ShmBootstrapTest, ExactPhasesRequireExactRecordSizes) {
  uint8_t record[IREE_NET_SHM_BOOTSTRAP_HEADER_SIZE];
  for (auto type : {IREE_NET_SHM_BOOTSTRAP_TYPE_ACCEPT,
                    IREE_NET_SHM_BOOTSTRAP_TYPE_READY}) {
    iree_net_shm_bootstrap_encode_ack(type, record);
    IREE_EXPECT_OK(iree_net_shm_bootstrap_decode_ack(
        iree_make_const_byte_span(record, sizeof(record)), type));
    auto other = type == IREE_NET_SHM_BOOTSTRAP_TYPE_ACCEPT
                     ? IREE_NET_SHM_BOOTSTRAP_TYPE_READY
                     : IREE_NET_SHM_BOOTSTRAP_TYPE_ACCEPT;
    IREE_EXPECT_STATUS_IS(
        IREE_STATUS_INVALID_ARGUMENT,
        iree_net_shm_bootstrap_decode_ack(
            iree_make_const_byte_span(record, sizeof(record)), other));
    iree_net_shm_region_layout_t layout;
    IREE_EXPECT_STATUS_IS(
        IREE_STATUS_INVALID_ARGUMENT,
        iree_net_shm_bootstrap_decode_offer(
            iree_make_const_byte_span(record, sizeof(record)), &layout));
  }
  for (size_t length = 0; length < kOffer.size(); ++length) {
    iree_net_shm_region_layout_t layout;
    IREE_EXPECT_STATUS_IS(
        IREE_STATUS_INVALID_ARGUMENT,
        iree_net_shm_bootstrap_decode_offer(
            iree_make_const_byte_span(kOffer.data(), length), &layout));
    EXPECT_EQ(layout.total_size, 0);
  }
  std::array<uint8_t, 21> extended = {};
  std::memcpy(extended.data(), kOffer.data(), kOffer.size());
  iree_net_shm_region_layout_t layout;
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      iree_net_shm_bootstrap_decode_offer(
          iree_make_const_byte_span(extended.data(), extended.size()),
          &layout));
}

TEST(ShmBootstrapTest, RejectsDifferentProtocolOrInvalidGeometry) {
  for (size_t offset : {0u, 4u, 6u, 8u, 12u, 18u}) {
    auto record = kOffer;
    record[offset] = 0;
    iree_net_shm_region_layout_t layout;
    std::memset(&layout, 0xCD, sizeof(layout));
    IREE_EXPECT_STATUS_IS(
        IREE_STATUS_INVALID_ARGUMENT,
        iree_net_shm_bootstrap_decode_offer(
            iree_make_const_byte_span(record.data(), record.size()), &layout));
    EXPECT_EQ(layout.total_size, 0);
  }
}

TEST(ShmBootstrapTest, UnalignedInputDoesNotBecomeSharedStructPointer) {
  uint8_t bytes[21];
  std::memcpy(bytes + 1, kOffer.data(), kOffer.size());
  iree_net_shm_region_layout_t layout;
  IREE_ASSERT_OK(iree_net_shm_bootstrap_decode_offer(
      iree_make_const_byte_span(bytes + 1, kOffer.size()), &layout));
  EXPECT_EQ(layout.options.slot_capacity, 65536);
}

TEST(ShmBootstrapTest, LargeOfferIsSizedWithoutAllocating) {
  auto record = kOffer;
  iree_unaligned_store_le_u32(record.data() + 8, UINT32_MAX);
  iree_unaligned_store_le_u32(record.data() + 12, 65534);
  iree_unaligned_store_le_u32(record.data() + 16, UINT32_MAX);
  iree_net_shm_region_layout_t layout;
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_OUT_OF_RANGE,
      iree_net_shm_bootstrap_decode_offer(
          iree_make_const_byte_span(record.data(), record.size()), &layout));
  EXPECT_EQ(layout.total_size, 0);
}

}  // namespace
