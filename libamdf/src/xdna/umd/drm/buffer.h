// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_SRC_XDNA_UMD_DRM_BUFFER_H_
#define AMDF_SRC_XDNA_UMD_DRM_BUFFER_H_

#include <stddef.h>

#include "amdf/amdf.h"

// A GEM allocation and its persistent native attachment. Zero initialization
// is empty. DEV allocations borrow the heap mapping; other types own theirs.
typedef struct amdf_linux_xdna_buffer_t {
  // File-local GEM handle, or zero after release.
  uint32_t handle;
  // Native `AMDXDNA_BO_*` allocation type fixed by creation.
  uint32_t type;
  // Allocation extent in bytes, rounded to the native page size.
  size_t byte_length;
  // Stable address in this native device's address domain.
  uint64_t device_address;
  // CPU base, including for heap suballocations that have no independent mmap.
  void* host_pointer;
  // Owned virtual reservation including any alignment padding.
  struct {
    // Beginning of the owned reservation, or NULL for a borrowed mapping.
    void* base;
    // Complete reservation extent in bytes; zero after unmapping.
    size_t byte_length;
  } mapping;
} amdf_linux_xdna_buffer_t;

#ifdef __cplusplus
extern "C" {
#endif

// Creates one GEM buffer. Success transfers its file-local handle to
// `out_buffer`; failure leaves the output unchanged and retains no ownership.
amdf_status_t amdf_linux_xdna_buffer_create(
    int descriptor, uint32_t type, size_t byte_length,
    amdf_linux_xdna_buffer_t* out_buffer);

// Establishes native addresses and a persistent CPU mapping for one live GEM
// buffer. Failure retains any mapping state acquired by this one-shot operation
// in `buffer` for deinitialization.
amdf_status_t amdf_linux_xdna_buffer_attach(
    int descriptor, size_t alignment, size_t page_size,
    const amdf_linux_xdna_buffer_t* heap, amdf_linux_xdna_buffer_t* buffer);

// Releases an idle buffer. A failed release retains the remaining ownership.
amdf_status_t amdf_linux_xdna_buffer_deinitialize(
    int descriptor, amdf_linux_xdna_buffer_t* buffer);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // AMDF_SRC_XDNA_UMD_DRM_BUFFER_H_
