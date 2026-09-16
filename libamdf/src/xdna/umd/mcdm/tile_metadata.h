// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_SRC_XDNA_UMD_MCDM_TILE_METADATA_H_
#define AMDF_SRC_XDNA_UMD_MCDM_TILE_METADATA_H_

#include "libamdf/src/platform/windows/kmt_api.h"
#include "libamdf/src/xdna/umd/device.h"

#ifdef __cplusplus
extern "C" {
#endif

// Queries native tile layout using the live device's private metadata service.
// No context, paging queue or allocation is required. Failure leaves the output
// unchanged.
amdf_status_t amdf_windows_xdna_query_tile_metadata(
    const amdf_kmt_api_t* kmt, D3DKMT_HANDLE adapter, D3DKMT_HANDLE device,
    amdf_xdna_umd_tile_metadata_t* out_metadata);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // AMDF_SRC_XDNA_UMD_MCDM_TILE_METADATA_H_
