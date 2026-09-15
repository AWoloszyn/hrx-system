// Copyright 2026 The HRX Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "common/memory.h"

#include <array>
#include <cstring>

#include "common/internal.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace {

class CpuStreamingMemoryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    IREE_ASSERT_OK(HRX_CALL(hrx_cpu_initialize(/*flags=*/0)));
    hrx_device_t hrx_device = nullptr;
    IREE_ASSERT_OK(HRX_CALL(hrx_cpu_device_get(/*index=*/0, &hrx_device)));

    device_entry_.hrx_device = hrx_device;
    device_entry_.hal_device = hrx_device_hal(hrx_device);
    iree_slim_mutex_initialize(&device_entry_.primary_context_mutex);
    iree_slim_mutex_initialize(&device_entry_.graph_memory_mutex);
    iree_arena_block_pool_initialize(/*block_size=*/64 * 1024,
                                     iree_allocator_system(),
                                     &device_entry_.block_pool);

    iree_hal_streaming_context_flags_t context_flags = {};
    context_flags.scheduling_mode = IREE_HAL_STREAMING_SCHEDULING_MODE_AUTO;
    IREE_ASSERT_OK(iree_hal_streaming_context_create(
        &device_entry_, context_flags, iree_allocator_system(), &context_));
    IREE_ASSERT_OK(iree_hal_streaming_stream_create(
        context_, context_->queue, IREE_HAL_STREAMING_STREAM_FLAG_NONE,
        /*priority=*/0, iree_allocator_system(), &stream_));
  }

  void TearDown() override {
    iree_hal_streaming_memory_release_wrapped_buffer(buffer_);
    buffer_ = nullptr;
    device_pointer_ = 0;
    iree_hal_streaming_stream_release(stream_);
    iree_hal_streaming_context_release(context_);
    iree_arena_block_pool_deinitialize(&device_entry_.block_pool);
    iree_slim_mutex_deinitialize(&device_entry_.graph_memory_mutex);
    iree_slim_mutex_deinitialize(&device_entry_.primary_context_mutex);
    IREE_EXPECT_OK(HRX_CALL(hrx_cpu_shutdown()));
  }

  iree_hal_streaming_device_t device_entry_ = {};
  iree_hal_streaming_context_t* context_ = nullptr;
  iree_hal_streaming_stream_t* stream_ = nullptr;
  iree_hal_streaming_buffer_t* buffer_ = nullptr;
  iree_hal_streaming_deviceptr_t device_pointer_ = 0;
};

TEST_F(CpuStreamingMemoryTest,
       PitchedCopyDrainsAcceptedRowsBeforeReturningRecordingError) {
  constexpr iree_device_size_t kAllocationSize = 16;
  IREE_ASSERT_OK(iree_hal_streaming_memory_allocate_device(
      context_, kAllocationSize, IREE_HAL_STREAMING_MEMORY_FLAG_NONE,
      &buffer_));
  device_pointer_ = iree_hal_streaming_buffer_device_pointer(buffer_);
  ASSERT_NE(nullptr, buffer_->host_ptr);

  const std::array<uint8_t, kAllocationSize> initial = {
      1, 2, 3, 4, 21, 22, 23, 24, 41, 42, 43, 44, 61, 62, 63, 64};
  std::memcpy(buffer_->host_ptr, initial.data(), initial.size());

  // Row zero copies [0, 4) to the disjoint range [4, 8). Row one then tries
  // to copy [8, 12) onto itself, forcing command-buffer validation to fail
  // only after the first row has been accepted.
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      iree_hal_streaming_memcpy_device_to_device_2d(
          context_, device_pointer_ + 4, /*dst_pitch=*/4, device_pointer_,
          /*src_pitch=*/8, /*width=*/4, /*height=*/2, stream_));

  const auto* contents = static_cast<const uint8_t*>(buffer_->host_ptr);
  EXPECT_EQ(0, std::memcmp(contents + 4, initial.data(), 4));
  EXPECT_EQ(0, std::memcmp(contents + 8, initial.data() + 8, 4));
}

}  // namespace
