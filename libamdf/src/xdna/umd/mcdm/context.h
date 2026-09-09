// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_SRC_XDNA_UMD_MCDM_CONTEXT_H_
#define AMDF_SRC_XDNA_UMD_MCDM_CONTEXT_H_

#include "libamdf/src/xdna/umd/context.h"
#include "libamdf/src/xdna/umd/mcdm/device.h"

// Concrete Windows state backing one schedulable XDNA context.
struct amdf_xdna_umd_context_t {
  // Ordinary-address-domain device borrowed through context destruction.
  amdf_xdna_umd_device_t* device;
  // Program-independent KMT execution context.
  D3DKMT_HANDLE handle;
  // Driver-returned command aperture selector; zero is a valid value.
  uint32_t command_aperture_cookie;
};

#endif  // AMDF_SRC_XDNA_UMD_MCDM_CONTEXT_H_
