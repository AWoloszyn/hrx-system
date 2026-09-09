// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_SRC_XDNA_UMD_DRM_MEMORY_H_
#define AMDF_SRC_XDNA_UMD_DRM_MEMORY_H_

#include "libamdf/src/xdna/umd/drm/buffer.h"
#include "libamdf/src/xdna/umd/memory.h"

struct amdf_xdna_umd_memory_t {
  // Device borrowed while memory metadata is live.
  amdf_xdna_umd_device_t* device;
  // Byte offset of logical byte zero in the complete native backing.
  uint64_t source_byte_offset;
  // Canonical identity of imported memory or provider-local created backing.
  amdf_physical_memory_id_t physical_backing_id;
  // SHARE allocation with its persistent SVA mapping.
  amdf_linux_xdna_buffer_t buffer;
};

#endif  // AMDF_SRC_XDNA_UMD_DRM_MEMORY_H_
