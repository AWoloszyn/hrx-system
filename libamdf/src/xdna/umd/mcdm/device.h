// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_SRC_XDNA_UMD_MCDM_DEVICE_H_
#define AMDF_SRC_XDNA_UMD_MCDM_DEVICE_H_

#include <stdint.h>

#include "libamdf/src/platform/windows/device_status.h"
#include "libamdf/src/platform/windows/kmt_api.h"
#include "libamdf/src/xdna/umd/device.h"

// Concrete Windows state backing one program-independent XDNA device.
struct amdf_xdna_umd_device_t {
  // KMT table borrowed from the endpoint's platform instance.
  const amdf_kmt_api_t* kmt;
  // Logical KMT device owning paging and execution state.
  D3DKMT_HANDLE device;
  // Confirmed execution failure shared by this device's execution paths.
  amdf_kmt_device_status_t status;
  // Paging queue owned by this logical device.
  D3DKMT_HANDLE paging_queue;
  // Synchronization object owned by the paging queue.
  D3DKMT_HANDLE paging_sync_object;
  // CPU mapping of the paging queue's monitored fence.
  const volatile uint64_t* paging_fence;
  // Program-independent XDNA context and address domain.
  D3DKMT_HANDLE context;
  // Driver-returned command aperture selector; zero is a valid value.
  uint32_t command_aperture_cookie;
};

#endif  // AMDF_SRC_XDNA_UMD_MCDM_DEVICE_H_
