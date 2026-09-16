// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_SRC_XDNA_UMD_MCDM_ADAPTER_INFO_H_
#define AMDF_SRC_XDNA_UMD_MCDM_ADAPTER_INFO_H_

#include "libamdf/src/platform/windows/kmt_api.h"

#ifdef __cplusplus
extern "C" {
#endif

// Native allocation policy required by the Windows XDNA execution interface.
typedef struct amdf_windows_xdna_adapter_info_t {
  // Whether driver kernel buffers require CreateResource and CreateShared.
  bool shared_kernel_buffers;
} amdf_windows_xdna_adapter_info_t;

// Queries the adapter's required kernel-buffer allocation policy before any
// device or context is created. Drivers that do not report this policy are
// unsupported. Failure leaves the output unchanged. Driver release numbers
// and hardware identity do not select the native protocol.
amdf_status_t amdf_windows_xdna_adapter_info_query(
    const amdf_kmt_api_t* kmt, D3DKMT_HANDLE adapter,
    amdf_windows_xdna_adapter_info_t* out_info);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // AMDF_SRC_XDNA_UMD_MCDM_ADAPTER_INFO_H_
