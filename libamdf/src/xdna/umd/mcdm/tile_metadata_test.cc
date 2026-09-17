// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/xdna/umd/mcdm/tile_metadata.h"

#include <cstring>

#include "gtest/gtest.h"

namespace {

struct QueryState {
  // Native tile record, beginning after its uint32 status byte count.
  uint16_t fields[20] = {1, 1, 12, 9, 6, 2, 1, 3, 1, 0};
  // Native result injected before writing the reply.
  NTSTATUS status = 0;
  // Number of native requests issued.
  uint32_t query_count = 0;
};

QueryState* state = nullptr;

NTSTATUS APIENTRY Query(const D3DKMT_ESCAPE* query) {
  ++state->query_count;
  EXPECT_EQ(query->hAdapter, 1u);
  EXPECT_EQ(query->hDevice, 2u);
  EXPECT_EQ(query->hContext, 0u);
  EXPECT_EQ(query->Type, D3DKMT_ESCAPE_DRIVERPRIVATE);
  EXPECT_EQ(query->Flags.Value, 0u);
  EXPECT_EQ(query->PrivateDriverDataSize, 156u);
  // The paired XRT request has two parameter blocks, with payload sizes eight
  // and 52. The first carries the requested 44-byte tile-info record size.
  const uint32_t expected[39] = {
      1,  140, 0, 0, 8,  0, 1, 0, 48, 0, 0, 0,
      44, 0,   0, 0, 52, 0, 1, 0, 0,  0, 1, 0,
  };
  EXPECT_EQ(std::memcmp(query->pPrivateDriverData, expected, sizeof(expected)),
            0);
  if (state->status < 0) {
    return state->status;
  }
  std::memcpy(static_cast<uint8_t*>(query->pPrivateDriverData) + 100,
              state->fields, sizeof(state->fields));
  return 0;
}

class WindowsXdnaTileMetadataTest : public ::testing::Test {
 protected:
  void SetUp() override {
    state = &state_;
    kmt_.escape = Query;
  }
  void TearDown() override { state = nullptr; }
  // Per-test native response, independent of any PCI identity.
  QueryState state_;
  // Only the live-device metadata procedure is available.
  amdf_kmt_api_t kmt_ = {};
};

TEST_F(WindowsXdnaTileMetadataTest, UsesReportedGeometryWithoutATargetTable) {
  amdf_xdna_umd_tile_metadata_t metadata = {};
  ASSERT_EQ(amdf_windows_xdna_query_tile_metadata(&kmt_, 1, 2, &metadata),
            AMDF_STATUS_OK);
  EXPECT_EQ(metadata.column_count, 12u);
  EXPECT_EQ(metadata.row_count, 9u);
  EXPECT_EQ(metadata.core_origin, 3u);
  EXPECT_EQ(metadata.core_count, 6u);
  EXPECT_EQ(metadata.memory_origin, 1u);
  EXPECT_EQ(metadata.memory_count, 2u);
  EXPECT_EQ(metadata.shim_origin, 0u);
  EXPECT_EQ(metadata.shim_count, 1u);
  EXPECT_EQ(state_.query_count, 1u);
}

TEST_F(WindowsXdnaTileMetadataTest, FailurePreservesOutputWithoutRetry) {
  state_.status = static_cast<NTSTATUS>(0xC0000001u);
  amdf_xdna_umd_tile_metadata_t metadata;
  std::memset(&metadata, 0xA5, sizeof(metadata));
  const auto original = metadata;
  EXPECT_EQ(amdf_windows_xdna_query_tile_metadata(&kmt_, 1, 2, &metadata),
            amdf_kmt_make_status(state_.status));
  EXPECT_EQ(std::memcmp(&metadata, &original, sizeof(metadata)), 0);
  EXPECT_EQ(state_.query_count, 1u);
}

TEST_F(WindowsXdnaTileMetadataTest, MissingMetadataDoesNotInventGeometry) {
  for (bool missing_procedure : {false, true}) {
    std::memset(state_.fields, 0, sizeof(state_.fields));
    state_.query_count = 0;
    kmt_.escape = missing_procedure ? nullptr : Query;
    amdf_xdna_umd_tile_metadata_t metadata;
    std::memset(&metadata, 0xA5, sizeof(metadata));
    const auto original = metadata;
    EXPECT_EQ(amdf_status_code(amdf_windows_xdna_query_tile_metadata(
                  &kmt_, 1, 2, &metadata)),
              AMDF_STATUS_CODE_UNSUPPORTED);
    EXPECT_EQ(std::memcmp(&metadata, &original, sizeof(metadata)), 0);
    EXPECT_EQ(state_.query_count, missing_procedure ? 0u : 1u);
  }
}

}  // namespace
