// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_SRC_GPU_UMD_KFD_DEVICE_H_
#define AMDF_SRC_GPU_UMD_KFD_DEVICE_H_

#include <stddef.h>

#include "libamdf/src/gpu/umd/device.h"
#include "libamdf/src/gpu/umd/kfd/topology.h"

// Explicit owner of one KFD context and its acquired DRM virtual address space.
struct amdf_gpu_umd_device_t {
  // KFD file selecting the primary or independent process context.
  int descriptor;
  // Fresh render file whose VM is acquired by the KFD context.
  int render_descriptor;
  // Context ownership mode selected at creation.
  amdf_gpu_device_mode_t mode;
  // Native identity, geometry and address limits established at creation.
  amdf_gpu_kfd_topology_t topology;
  // Native CPU page length in bytes.
  size_t page_size;
  // Qualified host cache-line length in bytes.
  uint32_t cache_line_size;
};

#endif  // AMDF_SRC_GPU_UMD_KFD_DEVICE_H_
