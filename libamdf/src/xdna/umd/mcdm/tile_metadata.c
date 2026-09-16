// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "libamdf/src/xdna/umd/mcdm/tile_metadata.h"

#include <string.h>

amdf_status_t amdf_windows_xdna_query_tile_metadata(
    const amdf_kmt_api_t* kmt, D3DKMT_HANDLE adapter, D3DKMT_HANDLE device,
    amdf_xdna_umd_tile_metadata_t* out_metadata) {
  if (kmt->escape == NULL) {
    return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
  }

  // XRT's aie_tiles_stats request: a 16-byte command header, followed by
  // input/output parameter blocks. Each block has 32 header bytes and eight
  // trailing bytes beyond its payload. Operation 1 takes the 44-byte tile
  // record size as uint64 and returns that record in a 52-byte output payload.
  uint32_t packet[39] = {
      [0] = 1,    // Tile metadata operation.
      [1] = 140,  // Parameter bytes following the command header.
      [4] = 8,    // Input payload byte length.
      [6] = 1,    // Input element count.
      [8] = 48,   // Byte offset to the output parameter block.
      [12] = 44,  // Requested native tile record byte length.
      [16] = 52,  // Output payload byte length.
      [18] = 1,   // Output element count.
      [22] = 1,   // Output direction; input direction is zero.
  };
  D3DKMT_ESCAPE request = {0};
  request.hAdapter = adapter;
  request.hDevice = device;
  request.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
  request.pPrivateDriverData = packet;
  request.PrivateDriverDataSize = sizeof(packet);
  const amdf_status_t status = amdf_kmt_make_status(kmt->escape(&request));
  if (!amdf_status_is_ok(status)) return status;

  // The public XRT aie2::aie_tiles_info record starts with uint32 col_size,
  // then uint16 status major/minor, columns, rows, and row-class counts/starts.
  uint16_t fields[20];
  memcpy(fields, (const uint8_t*)packet + 100, sizeof(fields));
  if (fields[2] == 0 || fields[3] == 0) {
    return amdf_make_api_status(AMDF_STATUS_CODE_UNSUPPORTED);
  }
  *out_metadata = (amdf_xdna_umd_tile_metadata_t){
      .column_count = fields[2],
      .row_count = fields[3],
      .core_origin = fields[7],
      .core_count = fields[4],
      .memory_origin = fields[8],
      .memory_count = fields[5],
      .shim_origin = fields[9],
      .shim_count = fields[6],
  };
  return AMDF_STATUS_OK;
}
