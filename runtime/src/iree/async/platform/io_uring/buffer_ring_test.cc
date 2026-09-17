// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/async/platform/io_uring/buffer_ring.h"

#include <array>
#include <cstring>

#include "iree/async/platform/io_uring/uring.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace {

class BufferRingTest : public ::testing::Test {
 protected:
  void SetUp() override {
    memset(&ring_, 0, sizeof(ring_));
    iree_status_t status = iree_io_uring_ring_initialize(
        iree_io_uring_ring_options_default(), &ring_);
    if (iree_status_is_unavailable(status)) {
      iree_status_free(status);
      GTEST_SKIP() << "io_uring is unavailable";
    }
    IREE_ASSERT_OK(status);
    ring_initialized_ = true;
  }

  void TearDown() override {
    IREE_EXPECT_OK(iree_io_uring_buffer_ring_free(second_buffer_ring_));
    IREE_EXPECT_OK(iree_io_uring_buffer_ring_free(first_buffer_ring_));
    if (ring_initialized_) {
      iree_io_uring_ring_deinitialize(&ring_);
    }
  }

  iree_io_uring_buffer_ring_options_t MakeOptions(void* buffer_base,
                                                  uint16_t preferred_group_id) {
    iree_io_uring_buffer_ring_options_t options =
        iree_io_uring_buffer_ring_options_default();
    options.buffer_base = buffer_base;
    options.buffer_size = 4096;
    options.buffer_count = 4;
    options.preferred_group_id = preferred_group_id;
    options.hint_transparent_huge_pages = false;
    return options;
  }

  // Ring providing direct same-task registration dispatch.
  iree_io_uring_ring_t ring_;

  // True after |ring_| has been initialized successfully.
  bool ring_initialized_ = false;

  // First live provided-buffer ring.
  iree_io_uring_buffer_ring_t* first_buffer_ring_ = nullptr;

  // Second live provided-buffer ring.
  iree_io_uring_buffer_ring_t* second_buffer_ring_ = nullptr;

  // Four buffers backing |first_buffer_ring_|.
  alignas(4096) std::array<uint8_t, 4 * 4096> first_buffers_ = {};

  // Four buffers backing |second_buffer_ring_|.
  alignas(4096) std::array<uint8_t, 4 * 4096> second_buffers_ = {};
};

TEST_F(BufferRingTest, CollisionUsesFullGroupIdSpace) {
  iree_status_t status = iree_io_uring_buffer_ring_allocate(
      &ring_.registration, MakeOptions(first_buffers_.data(), 0),
      iree_allocator_system(), &first_buffer_ring_);
  if (iree_status_is_invalid_argument(status)) {
    iree_status_free(status);
    GTEST_SKIP() << "provided buffer rings are unavailable";
  }
  IREE_ASSERT_OK(status);
  IREE_ASSERT_OK(iree_io_uring_buffer_ring_free(first_buffer_ring_));
  first_buffer_ring_ = nullptr;

  IREE_ASSERT_OK(iree_io_uring_buffer_ring_allocate(
      &ring_.registration, MakeOptions(first_buffers_.data(), UINT16_MAX),
      iree_allocator_system(), &first_buffer_ring_));
  ASSERT_EQ(iree_io_uring_buffer_ring_group_id(first_buffer_ring_), UINT16_MAX);

  IREE_ASSERT_OK(iree_io_uring_buffer_ring_allocate(
      &ring_.registration, MakeOptions(second_buffers_.data(), UINT16_MAX),
      iree_allocator_system(), &second_buffer_ring_));
  EXPECT_EQ(iree_io_uring_buffer_ring_group_id(second_buffer_ring_), 0);
}

TEST_F(BufferRingTest, RejectsInvalidBufferSizes) {
  iree_io_uring_buffer_ring_options_t options =
      MakeOptions(first_buffers_.data(), 0);
  options.buffer_size = 0;
  IREE_EXPECT_STATUS_IS(IREE_STATUS_INVALID_ARGUMENT,
                        iree_io_uring_buffer_ring_allocate(
                            &ring_.registration, options,
                            iree_allocator_system(), &first_buffer_ring_));
  EXPECT_EQ(first_buffer_ring_, nullptr);

  if (sizeof(iree_host_size_t) > sizeof(uint32_t)) {
    options.buffer_size = static_cast<iree_host_size_t>(UINT64_C(1) << 32);
    IREE_EXPECT_STATUS_IS(IREE_STATUS_INVALID_ARGUMENT,
                          iree_io_uring_buffer_ring_allocate(
                              &ring_.registration, options,
                              iree_allocator_system(), &first_buffer_ring_));
    EXPECT_EQ(first_buffer_ring_, nullptr);
  }
}

}  // namespace
