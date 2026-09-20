// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/net/carrier/shm/region.h"

#include <cstring>
#include <vector>

#include "iree/base/internal/shm.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace {

TEST(ShmRegionLayoutTest, GeometrySeparatesStateAndPayloads) {
  for (uint32_t slot_count : {1u, 2u, 16u, 65534u}) {
    for (uint32_t slot_capacity : {1u, 63u, 64u, 4097u, 65536u}) {
      iree_net_shm_region_layout_t layout;
      IREE_ASSERT_OK(iree_net_shm_region_calculate_layout(
          {4, slot_count, slot_capacity}, &layout));
      EXPECT_EQ(layout.direction_count, 8);
      EXPECT_EQ(layout.links_offset, 128);
      EXPECT_GE(layout.descriptors_offset,
                layout.links_offset +
                    slot_count * sizeof(iree_atomic_freelist_slot_t));
      EXPECT_EQ(layout.descriptors_offset % 64, 0);
      EXPECT_EQ(layout.payload_offset,
                layout.descriptors_offset +
                    iree_mpsc_queue_required_size(layout.descriptor_capacity));
      EXPECT_EQ(layout.payload_offset % 64, 0);
      EXPECT_GE(layout.slot_stride, slot_capacity);
      EXPECT_LT(layout.slot_stride - slot_capacity, 64);
      EXPECT_EQ(layout.slot_stride % 64, 0);
      EXPECT_EQ(layout.direction_stride,
                layout.payload_offset + slot_count * layout.slot_stride);
      EXPECT_EQ(layout.total_size, 128 + 8 * layout.direction_stride);
      EXPECT_GE(layout.descriptor_capacity, (slot_count + 1) * 24);
      EXPECT_EQ(layout.descriptor_capacity & (layout.descriptor_capacity - 1),
                0);
    }
  }
}

TEST(ShmRegionLayoutTest, RejectsInvalidDimensionsWithoutPartialLayout) {
  for (auto options : {iree_net_shm_region_options_t{0, 16, 65536},
                       {4, 0, 65536},
                       {4, 65535, 65536},
                       {4, 16, 0}}) {
    iree_net_shm_region_layout_t layout;
    std::memset(&layout, 0xCD, sizeof(layout));
    IREE_EXPECT_STATUS_IS(
        IREE_STATUS_INVALID_ARGUMENT,
        iree_net_shm_region_calculate_layout(options, &layout));
    EXPECT_EQ(layout.total_size, 0);
    EXPECT_EQ(layout.options.endpoint_count, 0);
  }
}

TEST(ShmRegionLayoutTest, RejectsUnrepresentableExtentWithoutAllocating) {
  iree_net_shm_region_layout_t layout;
  IREE_EXPECT_STATUS_IS(IREE_STATUS_OUT_OF_RANGE,
                        iree_net_shm_region_calculate_layout(
                            {UINT32_MAX, 65534, UINT32_MAX}, &layout));
  EXPECT_EQ(layout.total_size, 0);
}

TEST(ShmRegionLayoutTest, ExtentIsNotLimitedTo32Bits) {
  if (sizeof(iree_host_size_t) < sizeof(uint64_t)) {
    GTEST_SKIP();
  }
  iree_net_shm_region_layout_t layout;
  IREE_ASSERT_OK(
      iree_net_shm_region_calculate_layout({4, 16, 64 * 1024 * 1024}, &layout));
  EXPECT_GT(layout.total_size, UINT32_MAX);
}

class ShmRegionTest : public ::testing::Test {
 protected:
  void TearDown() override {
    iree_shm_close(&importer_);
    iree_shm_close(&creator_);
  }

  iree_status_t Create(iree_net_shm_region_options_t options) {
    IREE_RETURN_IF_ERROR(
        iree_net_shm_region_calculate_layout(options, &layout_));
    IREE_RETURN_IF_ERROR(
        iree_shm_create(nullptr, layout_.total_size, &creator_));
    std::memset(creator_.base, 0xCD, creator_.size);
    creator_directions_.resize(layout_.direction_count);
    IREE_RETURN_IF_ERROR(iree_net_shm_region_initialize(
        &layout_, iree_make_byte_span(creator_.base, creator_.size),
        creator_directions_.data()));
    IREE_RETURN_IF_ERROR(
        iree_shm_open_handle(creator_.handle, creator_.size, &importer_));
    importer_directions_.resize(layout_.direction_count);
    return iree_net_shm_region_open(
        &layout_, iree_make_byte_span(importer_.base, importer_.size),
        importer_directions_.data());
  }

  // Layout shared by both independently mapped views.
  iree_net_shm_region_layout_t layout_ = {};
  // Creator's mapping, closed independently of the import.
  iree_shm_mapping_t creator_ = {nullptr, 0, IREE_SHM_HANDLE_INVALID};
  // Importer's mapping, which owns its own native resource reference.
  iree_shm_mapping_t importer_ = {nullptr, 0, IREE_SHM_HANDLE_INVALID};
  // Process-local direction views into the creator's mapping.
  std::vector<iree_net_shm_direction_t> creator_directions_;
  // Process-local direction views into the importer's mapping.
  std::vector<iree_net_shm_direction_t> importer_directions_;
};

TEST_F(ShmRegionTest, InitializesOnlyMetadataAndSharesEpochs) {
  IREE_ASSERT_OK(Create({3, 2, 65}));
  EXPECT_NE(creator_.base, importer_.base);
  for (auto& direction : creator_directions_) {
    EXPECT_EQ(iree_atomic_freelist_count(direction.free_slots), 2);
    EXPECT_EQ(iree_atomic_load(direction.consumed_position,
                               iree_memory_order_acquire),
              0);
    for (uint32_t slot = 0; slot < 2; ++slot) {
      for (uint32_t byte = 0; byte < 65; ++byte) {
        EXPECT_EQ(direction.payload[slot * layout_.slot_stride + byte], 0xCD);
      }
    }
  }
  for (uint32_t side = 0; side < 2; ++side) {
    auto* epoch = iree_net_shm_region_epoch(creator_.base, side);
    EXPECT_EQ(iree_atomic_load(epoch, iree_memory_order_acquire), 0);
    iree_atomic_store(epoch, 41 + side, iree_memory_order_release);
    EXPECT_EQ(iree_atomic_load(iree_net_shm_region_epoch(importer_.base, side),
                               iree_memory_order_acquire),
              41 + side);
  }
}

TEST_F(ShmRegionTest, FullDescriptorBatchesWrapAndReturnSlotsIndependently) {
  IREE_ASSERT_OK(Create({2, 16, 65}));
  for (size_t direction_index = 0; direction_index < layout_.direction_count;
       ++direction_index) {
    auto& sender = creator_directions_[direction_index];
    auto& receiver = importer_directions_[direction_index];
    uint64_t position = 0;
    for (uint32_t batch = 0; batch < 32; ++batch) {
      std::vector<uint16_t> retained;
      for (uint32_t i = 0; i < 16; ++i) {
        uint16_t slot = 0;
        ASSERT_TRUE(iree_atomic_freelist_try_pop(sender.free_slots,
                                                 sender.links, &slot));
        std::memset(sender.payload + slot * layout_.slot_stride, batch + i, 65);
        position += 65;
        const iree_net_shm_descriptor_t descriptor = {slot, 65, position};
        ASSERT_TRUE(iree_mpsc_queue_write(&sender.descriptors, &descriptor,
                                          sizeof(descriptor)));
      }
      EXPECT_EQ(iree_atomic_freelist_count(sender.free_slots), 0);
      for (uint32_t i = 0; i < 16; ++i) {
        iree_net_shm_descriptor_t descriptor;
        iree_host_size_t length = 0;
        ASSERT_TRUE(iree_mpsc_queue_read(&receiver.descriptors, &descriptor,
                                         sizeof(descriptor), &length));
        ASSERT_EQ(length, sizeof(descriptor));
        ASSERT_LT(descriptor.slot, 16);
        ASSERT_EQ(descriptor.length, 65);
        EXPECT_EQ(descriptor.end_position, position - (15 - i) * 65);
        EXPECT_EQ(receiver.payload[descriptor.slot * layout_.slot_stride],
                  batch + i);
        retained.push_back(static_cast<uint16_t>(descriptor.slot));
      }
      iree_atomic_store(receiver.consumed_position, position,
                        iree_memory_order_release);
      EXPECT_EQ(
          iree_atomic_load(sender.consumed_position, iree_memory_order_acquire),
          position);
      // Consuming all descriptors acknowledges source reuse, not slot reuse.
      EXPECT_EQ(iree_atomic_freelist_count(sender.free_slots), 0);
      for (auto it = retained.rbegin(); it != retained.rend(); ++it) {
        iree_atomic_freelist_push(receiver.free_slots, receiver.links, *it);
      }
      EXPECT_EQ(iree_atomic_freelist_count(sender.free_slots), 16);
    }
  }
}

TEST_F(ShmRegionTest, ImportedSlotsSurviveCreatorMappingClosure) {
  IREE_ASSERT_OK(Create({1, 2, 4096}));
  auto& sender = creator_directions_[0];
  auto& receiver = importer_directions_[0];
  uint16_t slot = 0;
  ASSERT_TRUE(
      iree_atomic_freelist_try_pop(sender.free_slots, sender.links, &slot));
  std::memset(sender.payload + slot * layout_.slot_stride, 0x7A, 4096);
  iree_shm_close(&creator_);
  for (size_t i = 0; i < 4096; ++i) {
    EXPECT_EQ(receiver.payload[slot * layout_.slot_stride + i], 0x7A);
  }
  iree_atomic_freelist_push(receiver.free_slots, receiver.links, slot);
  EXPECT_EQ(iree_atomic_freelist_count(receiver.free_slots), 2);
}

TEST_F(ShmRegionTest, RejectsTruncatedOrUnalignedMappingBeforeAccess) {
  IREE_ASSERT_OK(Create({1, 1, 64}));
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      iree_net_shm_region_open(
          &layout_, iree_make_byte_span(importer_.base, layout_.total_size - 1),
          importer_directions_.data()));
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      iree_net_shm_region_open(
          &layout_,
          iree_make_byte_span((uint8_t*)importer_.base + 1, layout_.total_size),
          importer_directions_.data()));
}

TEST_F(ShmRegionTest, RejectsDescriptorQueueGeometryMismatch) {
  IREE_ASSERT_OK(Create({1, 16, 64}));
  creator_directions_[0].descriptors.header->capacity /= 2;
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      iree_net_shm_region_open(
          &layout_, iree_make_byte_span(importer_.base, importer_.size),
          importer_directions_.data()));
}

}  // namespace
