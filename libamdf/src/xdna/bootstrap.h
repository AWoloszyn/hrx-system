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

// Immutable target bootstrap consumed by native execution providers.
typedef struct amdf_xdna_bootstrap_t {
  // Provider-independent PDI bytes copied into native device storage.
  const void* pdi_bytes;
  // Number of bytes in `pdi_bytes`.
  uint32_t pdi_byte_length;
  // Transaction bytes used to admit the interpreter before application work.
  const void* admission_transaction_bytes;
  // Number of bytes in `admission_transaction_bytes`.
  uint32_t admission_transaction_byte_length;
} amdf_xdna_bootstrap_t;

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // AMDF_SRC_XDNA_BOOTSTRAP_H_
