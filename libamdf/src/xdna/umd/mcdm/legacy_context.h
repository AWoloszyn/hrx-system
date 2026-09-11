// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_SRC_XDNA_UMD_MCDM_LEGACY_CONTEXT_H_
#define AMDF_SRC_XDNA_UMD_MCDM_LEGACY_CONTEXT_H_

#include <stdint.h>

#include "amdf/amdf.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Builds the exact NPU5 legacy context record around the provider-owned image.
// Replaces its partition width and first candidate starting column; the other
// candidate starting columns remain fixed. These are admission inputs, not a
// guarantee of the native context's achieved placement.
amdf_status_t amdf_windows_xdna_legacy_context_build(
    uint32_t partition_column_count, uint32_t first_start_column,
    amdf_allocator_t host_allocator, uint8_t** out_data,
    uint32_t* out_data_size);

// Reads the command-aperture cookie written back by context creation.
amdf_status_t amdf_windows_xdna_legacy_context_query_command_aperture_cookie(
    const uint8_t* data, uint32_t data_size, uint32_t* out_cookie);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // AMDF_SRC_XDNA_UMD_MCDM_LEGACY_CONTEXT_H_
