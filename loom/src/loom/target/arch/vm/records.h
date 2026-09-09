// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TARGET_ARCH_VM_RECORDS_H_
#define LOOM_TARGET_ARCH_VM_RECORDS_H_

#include "loom/target/types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Immutable target bundles selected by vm.target. The Core VM uses 64-bit
// index/offset arithmetic independently of the native interpreter's host ABI.
extern const loom_target_bundle_table_t loom_vm_target_bundles;

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_VM_RECORDS_H_
