// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/xdna/umd/mcdm/legacy_context.h"

#include <cstdint>
#include <cstring>

#include "gtest/gtest.h"
#include "libamdf/src/allocator.h"

namespace {

uint32_t ReadU32(const uint8_t* data, size_t offset) {
  uint32_t value = 0;
  std::memcpy(&value, data + offset, sizeof(value));
  return value;
}

uint64_t ReadU64(const uint8_t* data, size_t offset) {
  uint64_t value = 0;
  std::memcpy(&value, data + offset, sizeof(value));
  return value;
}

TEST(XdnaLegacyContextTest, BuildsExactNpu5CompatibilityRecord) {
  const amdf_allocator_t host_allocator = amdf_allocator_system();
  uint8_t* data = nullptr;
  uint32_t data_size = 0;
  ASSERT_TRUE(amdf_status_is_ok(amdf_windows_xdna_legacy_context_build(
      3, 0, host_allocator, &data, &data_size)));
  ASSERT_NE(data, nullptr);
  EXPECT_EQ(data_size, 9578u);

  constexpr uint8_t kExpectedUuid[16] = {
      0xFD, 0xCC, 0xC6, 0x7B, 0x88, 0xFD, 0x7C, 0x88,
      0x81, 0x8C, 0x44, 0xB2, 0xFC, 0x5D, 0x18, 0x84,
  };
  EXPECT_EQ(std::memcmp(data, kExpectedUuid, sizeof(kExpectedUuid)), 0);
  EXPECT_EQ(ReadU64(data, 0x48), UINT64_C(0x04000000));
  EXPECT_EQ(ReadU64(data, 0x58), data_size - 0x80);
  EXPECT_NE(ReadU64(data, 0x60), 0u);
  EXPECT_EQ(ReadU64(data, 0xD0), 8454u);
  EXPECT_EQ(std::memcmp(data + 0xE8, "xclbin2\0", 8), 0);

  constexpr size_t kTailOffset = 0xE8 + 8454;
  EXPECT_STREQ(reinterpret_cast<const char*>(data + kTailOffset), "MLIR_AIE");
  // Kernel metadata describes the register range and argument count, not the
  // physical array width.
  EXPECT_EQ(ReadU64(data, kTailOffset + 0x40), UINT64_C(0x10000));
  EXPECT_EQ(ReadU64(data, kTailOffset + 0x48), 8u);
  // Partition admission describes operations per cycle, a candidate count,
  // the requested width, and the complete candidate starting-column list.
  EXPECT_EQ(ReadU32(data, kTailOffset + 0x360), 0x800u);
  EXPECT_EQ(ReadU32(data, kTailOffset + 0x364), 4u);
  EXPECT_EQ(ReadU32(data, kTailOffset + 0x368), 3u);
  EXPECT_EQ(ReadU32(data, kTailOffset + 0x36C), 0u);
  EXPECT_EQ(ReadU32(data, kTailOffset + 0x370), 2u);
  EXPECT_EQ(ReadU32(data, kTailOffset + 0x374), 3u);
  EXPECT_EQ(ReadU32(data, kTailOffset + 0x378), 4u);

  uint32_t cookie = UINT32_MAX;
  ASSERT_TRUE(amdf_status_is_ok(
      amdf_windows_xdna_legacy_context_query_command_aperture_cookie(
          data, data_size, &cookie)));
  EXPECT_EQ(cookie, 0u);
  constexpr uint32_t kReturnedCookie = 0x12345678u;
  std::memcpy(data + 0x40, &kReturnedCookie, sizeof(kReturnedCookie));
  ASSERT_TRUE(amdf_status_is_ok(
      amdf_windows_xdna_legacy_context_query_command_aperture_cookie(
          data, data_size, &cookie)));
  EXPECT_EQ(cookie, kReturnedCookie);
  amdf_free(host_allocator, data);
}

TEST(XdnaLegacyContextTest, ValidatesOutputStorage) {
  const amdf_allocator_t host_allocator = amdf_allocator_system();
  uint8_t* data = reinterpret_cast<uint8_t*>(uintptr_t{1});
  uint32_t data_size = UINT32_MAX;
  EXPECT_EQ(amdf_status_code(amdf_windows_xdna_legacy_context_build(
                1, 0, host_allocator, nullptr, &data_size)),
            AMDF_STATUS_CODE_INVALID_ARGUMENT);
  EXPECT_EQ(data_size, UINT32_MAX);
  EXPECT_EQ(amdf_status_code(amdf_windows_xdna_legacy_context_build(
                1, 0, host_allocator, &data, nullptr)),
            AMDF_STATUS_CODE_INVALID_ARGUMENT);
  EXPECT_EQ(data, reinterpret_cast<uint8_t*>(uintptr_t{1}));
}

}  // namespace
