// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/xdna/bootstrap.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "gtest/gtest.h"

namespace {

uint32_t ReadU32(const uint8_t* bytes, size_t offset) {
  return uint32_t{bytes[offset]} | (uint32_t{bytes[offset + 1]} << 8) |
         (uint32_t{bytes[offset + 2]} << 16) |
         (uint32_t{bytes[offset + 3]} << 24);
}

void VerifyChecksum(const uint8_t* bytes, size_t byte_length) {
  uint32_t sum = 0;
  for (size_t offset = 0; offset < byte_length; offset += 4)
    sum += ReadU32(bytes, offset);
  EXPECT_EQ(sum, UINT32_MAX);
}

TEST(XdnaBootstrapTest, AdmitsFunctionZeroWithoutArrayEffects) {
  std::array<uint8_t, AMDF_XDNA_BOOTSTRAP_PDI_BYTE_LENGTH> storage;
  storage.fill(0xA5);
  amdf_xdna_bootstrap_write_pdi(storage.data());
  const uint8_t* bytes = storage.data();
  ASSERT_EQ(storage.size(), 368u);
  EXPECT_EQ(ReadU32(bytes, 0), 0xDDu);
  EXPECT_EQ(ReadU32(bytes, 4), 0x11223344u);
  EXPECT_EQ(ReadU32(bytes, 8), 0x55667788u);
  EXPECT_EQ(ReadU32(bytes, 12), 0x99AABBCCu);

  // Follow native word offsets instead of assuming adjacent headers.
  const uint8_t* table = bytes + 16;
  EXPECT_EQ(ReadU32(table, 0), 0x00040000u);
  ASSERT_EQ(ReadU32(table, 4), 1u);
  ASSERT_EQ(ReadU32(table, 12), 1u);
  EXPECT_EQ(ReadU32(table, 40), 0x50504449u);
  EXPECT_EQ(ReadU32(table, 44), 0x00201020u);
  EXPECT_EQ(ReadU32(table, 48), 48u);
  VerifyChecksum(table, 128);
  const size_t image_offset = ReadU32(table, 8) * 4;
  const size_t partition_offset = ReadU32(table, 16) * 4;
  ASSERT_LE(image_offset + 64, storage.size());
  ASSERT_LE(partition_offset + 128, storage.size());
  const uint8_t* image = bytes + image_offset;
  const uint8_t* partition = bytes + partition_offset;
  EXPECT_EQ(ReadU32(image, 0) * 4, partition_offset);
  EXPECT_EQ(ReadU32(image, 4), 1u);
  EXPECT_EQ(ReadU32(image, 8), 0u);   // Last image; no chained header.
  EXPECT_EQ(ReadU32(image, 44), 0u);  // CU function ID.
  VerifyChecksum(image, 64);
  VerifyChecksum(partition, 128);
  EXPECT_EQ(ReadU32(partition, 0), ReadU32(partition, 8));
  EXPECT_EQ(ReadU32(partition, 12), 0u);           // Last partition.
  EXPECT_EQ(ReadU32(partition, 36), 0x02000006u);  // CDO, EL3.
  EXPECT_EQ(ReadU32(partition, 40), 1u);
  const size_t cdo_offset = ReadU32(partition, 32) * 4;
  const size_t cdo_length = ReadU32(partition, 4) * 4;
  const size_t cdo_storage_length = ReadU32(partition, 8) * 4;
  ASSERT_EQ(cdo_offset + cdo_storage_length, storage.size());
  EXPECT_EQ(cdo_storage_length % 16, 0u);
  ASSERT_EQ(cdo_length, 24u);
  const uint8_t* cdo = bytes + cdo_offset;
  EXPECT_EQ(ReadU32(cdo, 0), 4u);
  EXPECT_EQ(ReadU32(cdo, 4), 0x004F4443u);
  EXPECT_EQ(ReadU32(cdo, 8), 0x200u);
  EXPECT_EQ(ReadU32(cdo, 12), 1u);
  VerifyChecksum(cdo, 20);
  // Exactly one zero-argument NOP: no trailing command can enable a core,
  // initialize DMA or locks, write a route, or overwrite program/data memory.
  EXPECT_EQ(ReadU32(cdo, 20), 0x111u);
  for (size_t i = cdo_length; i < cdo_storage_length; ++i)
    EXPECT_EQ(cdo[i], 0u);
}

TEST(XdnaBootstrapTest,
     InitializesExactlyItsExtentWithoutAlignmentRequirement) {
  constexpr size_t kLength = AMDF_XDNA_BOOTSTRAP_PDI_BYTE_LENGTH;
  std::array<uint8_t, kLength + 2> first;
  std::array<uint8_t, kLength + 2> second;
  first.fill(0xA5);
  second.fill(0x5A);
  amdf_xdna_bootstrap_write_pdi(first.data() + 1);
  amdf_xdna_bootstrap_write_pdi(second.data() + 1);
  EXPECT_EQ(first.front(), 0xA5);
  EXPECT_EQ(first.back(), 0xA5);
  EXPECT_EQ(second.front(), 0x5A);
  EXPECT_EQ(second.back(), 0x5A);
  EXPECT_EQ(std::memcmp(first.data() + 1, second.data() + 1, kLength), 0);
}

}  // namespace
