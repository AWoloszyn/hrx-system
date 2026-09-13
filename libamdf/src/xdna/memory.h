// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_SRC_XDNA_MEMORY_H_
#define AMDF_SRC_XDNA_MEMORY_H_

#include "amdf/amdf.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Copies one XDNA memory profile, or reports it unsupported.
amdf_status_t amdf_xdna_device_query_memory_profile(
    amdf_device_t* device, uint32_t memory_profile_ordinal,
    amdf_memory_profile_t* out_profile);

// Prepares native state in the common memory owner, including on failure.
amdf_status_t amdf_xdna_memory_prepare(
    amdf_memory_t* memory, const amdf_memory_profile_t* profile,
    const amdf_memory_create_info_t* create_info);

// Prepares imported state without consuming the input. Success reports no
// external lease: the native provider acquires its own backing reference.
amdf_status_t amdf_xdna_memory_prepare_import(
    amdf_memory_t* memory, const amdf_memory_profile_t* profile,
    const amdf_memory_import_info_t* import_info,
    const amdf_external_memory_t* external_memory,
    amdf_external_memory_t** out_external_memory_lease);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // AMDF_SRC_XDNA_MEMORY_H_
