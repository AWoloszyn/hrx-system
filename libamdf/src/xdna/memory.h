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

// Creates memory attached to one XDNA device.
amdf_status_t amdf_xdna_memory_create(
    amdf_device_t* device, const amdf_memory_profile_t* profile,
    const amdf_memory_create_info_t* create_info, amdf_memory_t** out_memory);

// Imports external memory into one XDNA device, or reports it unsupported.
amdf_status_t amdf_xdna_memory_import(
    amdf_device_t* device, const amdf_memory_profile_t* profile,
    const amdf_memory_import_info_t* import_info,
    const amdf_external_memory_t* external_memory, amdf_memory_t** out_memory);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // AMDF_SRC_XDNA_MEMORY_H_
