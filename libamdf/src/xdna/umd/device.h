// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_SRC_XDNA_UMD_DEVICE_H_
#define AMDF_SRC_XDNA_UMD_DEVICE_H_

#include "amdf/xdna.h"
#include "libamdf/src/platform/endpoint.h"
#include "libamdf/src/xdna/endpoint_profile.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

typedef struct amdf_xdna_umd_device_t amdf_xdna_umd_device_t;

// Native result used to publish one successfully materialized XDNA device.
typedef struct amdf_xdna_umd_device_result_t {
  // Opaque identity of the live native address-domain device.
  amdf_device_id_t id;
  // Provider epoch invalidating native state after reset.
  uint64_t reset_epoch;
} amdf_xdna_umd_device_result_t;

// Creates one native XDNA ordinary-address domain and allocation namespace.
amdf_status_t amdf_xdna_umd_device_create(
    amdf_platform_endpoint_t* endpoint,
    const amdf_xdna_endpoint_profile_t* profile,
    amdf_allocator_t host_allocator, amdf_xdna_umd_device_t** out_device,
    amdf_xdna_umd_device_result_t* out_result);

// Releases native XDNA device state in reverse ownership order.
amdf_status_t amdf_xdna_umd_device_destroy(amdf_xdna_umd_device_t* device);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // AMDF_SRC_XDNA_UMD_DEVICE_H_
