// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef AMDF_SRC_XDNA_BOOTSTRAP_H_
#define AMDF_SRC_XDNA_BOOTSTRAP_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// Target admission metadata consumed by native execution providers. Application
// programs and array configuration are supplied separately after admission.
typedef struct amdf_xdna_bootstrap_t {
  // Native context-admission identity and accounting for this bootstrap.
  struct {
    // UUID naming the bootstrap, independent of application executable images.
    uint8_t uuid[16];
    // Nominal native admission accounting, not the application's operation
    // count.
    uint32_t operations_per_cycle;
  } context;
} amdf_xdna_bootstrap_t;

enum {
  // Complete partial-PDI container, including aligned CDO partition storage.
  AMDF_XDNA_BOOTSTRAP_PDI_BYTE_LENGTH = 368,
};

// Writes the native interpreter admission PDI directly into at least
// AMDF_XDNA_BOOTSTRAP_PDI_BYTE_LENGTH writable bytes. The shared NPU4/NPU5
// encoding selects function zero and contains one CDO NOP: no program,
// register, DMA, lock, route, or tile-memory effects. All bytes within the
// encoded extent are initialized; bytes beyond it are untouched. This cold-path
// operation is infallible and performs no allocation or publication.
void amdf_xdna_bootstrap_write_pdi(void* target);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // AMDF_SRC_XDNA_BOOTSTRAP_H_
