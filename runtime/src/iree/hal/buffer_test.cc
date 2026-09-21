// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/hal/buffer.h"

#include <string>

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"

namespace {

static std::string FormatMemoryType(iree_hal_memory_type_t memory_type) {
  iree_bitfield_string_temp_t temporary;
  const iree_string_view_t value =
      iree_hal_memory_type_format(memory_type, &temporary);
  return std::string(value.data, value.size);
}

TEST(MemoryTypeTest, EncodesLocalityIndependentlyFromCoherence) {
  EXPECT_EQ(IREE_HAL_MEMORY_TYPE_HOST_LOCAL, 0x42u);
  EXPECT_EQ(
      IREE_HAL_MEMORY_TYPE_HOST_LOCAL | IREE_HAL_MEMORY_TYPE_HOST_COHERENT,
      0x46u);
  EXPECT_EQ(IREE_HAL_MEMORY_TYPE_HOST_LOCAL | IREE_HAL_MEMORY_TYPE_DEVICE_LOCAL,
            0x72u);
  EXPECT_EQ(IREE_HAL_MEMORY_TYPE_HOST_LOCAL |
                IREE_HAL_MEMORY_TYPE_HOST_COHERENT |
                IREE_HAL_MEMORY_TYPE_DEVICE_LOCAL,
            0x76u);
}

TEST(MemoryTypeTest, RoundTripsOrthogonalLocalityAndCoherence) {
  static const struct {
    iree_hal_memory_type_t memory_type;
    const char* value;
  } cases[] = {
      {IREE_HAL_MEMORY_TYPE_HOST_LOCAL, "HOST_LOCAL"},
      {IREE_HAL_MEMORY_TYPE_HOST_LOCAL | IREE_HAL_MEMORY_TYPE_HOST_COHERENT,
       "HOST_LOCAL|HOST_COHERENT"},
      {IREE_HAL_MEMORY_TYPE_HOST_LOCAL | IREE_HAL_MEMORY_TYPE_DEVICE_LOCAL,
       "HOST_LOCAL|DEVICE_LOCAL"},
      {IREE_HAL_MEMORY_TYPE_HOST_LOCAL | IREE_HAL_MEMORY_TYPE_HOST_COHERENT |
           IREE_HAL_MEMORY_TYPE_DEVICE_LOCAL,
       "HOST_LOCAL|DEVICE_LOCAL|HOST_COHERENT"},
  };
  for (const auto& test_case : cases) {
    EXPECT_EQ(FormatMemoryType(test_case.memory_type), test_case.value);
    iree_hal_memory_type_t parsed_memory_type = 0;
    IREE_EXPECT_OK(iree_hal_memory_type_parse(
        iree_make_cstring_view(test_case.value), &parsed_memory_type));
    EXPECT_EQ(parsed_memory_type, test_case.memory_type);
  }
}

TEST(BufferRangeTest, AcceptsContainedRanges) {
  iree_hal_buffer_t buffer = {};
  buffer.byte_length = 16;

  IREE_EXPECT_OK(iree_hal_buffer_validate_range(&buffer, 0, 16));
  IREE_EXPECT_OK(iree_hal_buffer_validate_range(&buffer, 7, 9));
  IREE_EXPECT_OK(iree_hal_buffer_validate_range(&buffer, 16, 0));
}

TEST(BufferRangeTest, RejectsOutOfRangeAndOverflowingRanges) {
  iree_hal_buffer_t buffer = {};
  buffer.byte_length = 16;

  IREE_EXPECT_STATUS_IS(IREE_STATUS_OUT_OF_RANGE,
                        iree_hal_buffer_validate_range(&buffer, 17, 0));
  IREE_EXPECT_STATUS_IS(IREE_STATUS_OUT_OF_RANGE,
                        iree_hal_buffer_validate_range(&buffer, 15, 2));

  buffer.byte_length = IREE_DEVICE_SIZE_MAX;
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_OUT_OF_RANGE,
      iree_hal_buffer_validate_range(&buffer, IREE_DEVICE_SIZE_MAX - 3, 8));
}

TEST(BufferPermissionTest, ValidatesAccessAndUsage) {
  IREE_EXPECT_OK(iree_hal_buffer_validate_access(IREE_HAL_MEMORY_ACCESS_READ,
                                                 IREE_HAL_MEMORY_ACCESS_READ));
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_PERMISSION_DENIED,
      iree_hal_buffer_validate_access(IREE_HAL_MEMORY_ACCESS_READ,
                                      IREE_HAL_MEMORY_ACCESS_WRITE));

  IREE_EXPECT_OK(
      iree_hal_buffer_validate_usage(IREE_HAL_BUFFER_USAGE_TRANSFER_SOURCE,
                                     IREE_HAL_BUFFER_USAGE_TRANSFER_SOURCE));
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_PERMISSION_DENIED,
      iree_hal_buffer_validate_usage(IREE_HAL_BUFFER_USAGE_TRANSFER_SOURCE,
                                     IREE_HAL_BUFFER_USAGE_TRANSFER_TARGET));
}

static void CountBufferRelease(void* user_data, iree_hal_buffer_t* buffer) {
  ++*static_cast<int*>(user_data);
}

TEST(BufferExportTest, NestedSubspansBorrowTheExactView) {
  alignas(64) uint8_t storage[128] = {};
  int release_count = 0;
  iree_hal_buffer_release_callback_t release_callback = {
      /*.fn=*/CountBufferRelease,
      /*.user_data=*/&release_count,
  };
  iree_hal_buffer_t* buffer = nullptr;
  IREE_ASSERT_OK(iree_hal_heap_buffer_wrap(
      iree_hal_buffer_placement_undefined(), IREE_HAL_MEMORY_TYPE_HOST_LOCAL,
      IREE_HAL_MEMORY_ACCESS_ALL,
      IREE_HAL_BUFFER_USAGE_MAPPING_PERSISTENT |
          IREE_HAL_BUFFER_USAGE_MAPPING_SCOPED,
      sizeof(storage), iree_make_byte_span(storage, 96), release_callback,
      iree_allocator_system(), &buffer));

  iree_hal_external_buffer_t external_buffer = {};
  IREE_ASSERT_OK(iree_hal_buffer_export(
      buffer, IREE_HAL_EXTERNAL_BUFFER_TYPE_HOST_ALLOCATION,
      IREE_HAL_EXTERNAL_BUFFER_FLAG_NONE, &external_buffer));
  EXPECT_EQ(storage, external_buffer.handle.host_allocation.ptr);
  EXPECT_EQ(96u, external_buffer.size);

  iree_hal_buffer_t* subspan = nullptr;
  IREE_ASSERT_OK(iree_hal_buffer_subspan(buffer, 16, 64,
                                         iree_allocator_system(), &subspan));
  iree_hal_buffer_t* nested_subspan = nullptr;
  IREE_ASSERT_OK(iree_hal_buffer_subspan(
      subspan, 8, 16, iree_allocator_system(), &nested_subspan));
  iree_hal_buffer_release(subspan);
  iree_hal_buffer_release(buffer);

  IREE_ASSERT_OK(iree_hal_buffer_export(
      nested_subspan, IREE_HAL_EXTERNAL_BUFFER_TYPE_HOST_ALLOCATION,
      IREE_HAL_EXTERNAL_BUFFER_FLAG_NONE, &external_buffer));
  EXPECT_EQ(storage + 24, external_buffer.handle.host_allocation.ptr);
  EXPECT_EQ(16u, external_buffer.size);
  const uint8_t pattern = 0xA7;
  IREE_ASSERT_OK(iree_hal_buffer_map_fill(
      nested_subspan, 0, IREE_HAL_WHOLE_BUFFER, &pattern, sizeof(pattern)));
  auto* exported_bytes =
      static_cast<uint8_t*>(external_buffer.handle.host_allocation.ptr);
  for (size_t i = 0; i < external_buffer.size; ++i) {
    EXPECT_EQ(pattern, exported_bytes[i]);
  }
  EXPECT_EQ(0, storage[23]);
  EXPECT_EQ(0, storage[40]);

  IREE_ASSERT_OK(iree_hal_buffer_export(
      nested_subspan, IREE_HAL_EXTERNAL_BUFFER_TYPE_DEVICE_ALLOCATION,
      IREE_HAL_EXTERNAL_BUFFER_FLAG_NONE, &external_buffer));
  EXPECT_EQ(reinterpret_cast<uintptr_t>(storage + 24),
            external_buffer.handle.device_allocation.ptr);
  EXPECT_EQ(16u, external_buffer.size);
  EXPECT_EQ(0, release_count);
  iree_hal_buffer_release(nested_subspan);
  EXPECT_EQ(1, release_count);
}

TEST(BufferExportTest, FailureClearsOutputAndPreservesMappingRequirements) {
  alignas(64) uint8_t storage[64] = {};
  iree_hal_buffer_t* buffer = nullptr;
  IREE_ASSERT_OK(iree_hal_heap_buffer_wrap(
      iree_hal_buffer_placement_undefined(), IREE_HAL_MEMORY_TYPE_HOST_LOCAL,
      IREE_HAL_MEMORY_ACCESS_ALL, IREE_HAL_BUFFER_USAGE_MAPPING_PERSISTENT,
      sizeof(storage), iree_make_byte_span(storage, sizeof(storage)),
      iree_hal_buffer_release_callback_null(), iree_allocator_system(),
      &buffer));

  iree_hal_external_buffer_t external_buffer = {};
  IREE_ASSERT_OK(iree_hal_buffer_export(
      buffer, IREE_HAL_EXTERNAL_BUFFER_TYPE_HOST_ALLOCATION,
      IREE_HAL_EXTERNAL_BUFFER_FLAG_NONE, &external_buffer));
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_UNAVAILABLE,
      iree_hal_buffer_export(buffer, IREE_HAL_EXTERNAL_BUFFER_TYPE_OPAQUE_FD,
                             IREE_HAL_EXTERNAL_BUFFER_FLAG_NONE,
                             &external_buffer));
  EXPECT_EQ(IREE_HAL_EXTERNAL_BUFFER_TYPE_NONE, external_buffer.type);
  EXPECT_EQ(0u, external_buffer.size);
  EXPECT_EQ(nullptr, external_buffer.handle.host_allocation.ptr);

  IREE_ASSERT_OK(iree_hal_buffer_export(
      buffer, IREE_HAL_EXTERNAL_BUFFER_TYPE_HOST_ALLOCATION,
      IREE_HAL_EXTERNAL_BUFFER_FLAG_NONE, &external_buffer));
  iree_hal_buffer_release(buffer);
  IREE_ASSERT_OK(iree_hal_heap_buffer_wrap(
      iree_hal_buffer_placement_undefined(), IREE_HAL_MEMORY_TYPE_HOST_LOCAL,
      IREE_HAL_MEMORY_ACCESS_ALL, IREE_HAL_BUFFER_USAGE_TRANSFER,
      sizeof(storage), iree_make_byte_span(storage, sizeof(storage)),
      iree_hal_buffer_release_callback_null(), iree_allocator_system(),
      &buffer));
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_PERMISSION_DENIED,
      iree_hal_buffer_export(
          buffer, IREE_HAL_EXTERNAL_BUFFER_TYPE_HOST_ALLOCATION,
          IREE_HAL_EXTERNAL_BUFFER_FLAG_NONE, &external_buffer));
  EXPECT_EQ(IREE_HAL_EXTERNAL_BUFFER_TYPE_NONE, external_buffer.type);
  EXPECT_EQ(0u, external_buffer.size);
  EXPECT_EQ(nullptr, external_buffer.handle.host_allocation.ptr);
  iree_hal_buffer_release(buffer);
}

}  // namespace
