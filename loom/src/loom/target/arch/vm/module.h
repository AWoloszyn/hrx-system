// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TARGET_ARCH_VM_MODULE_H_
#define LOOM_TARGET_ARCH_VM_MODULE_H_

#include "loom/target/provider.h"

#ifdef __cplusplus
extern "C" {
#endif

// Emits VM functions in a prepared mixed-target module as one immutable .vm
// artifact. Signature and export tables are sorted for runtime consumption;
// the common compiler has already resolved the functions participating in the
// module. Bytes are appended once to a segmented stream and fixed table rows
// are backpatched. No instruction sizing pass or contiguous image is required.
// Success transfers the byte sequence to |out_artifact|; failure publishes
// none.
iree_status_t loom_vm_module_emit(const loom_target_emit_request_t* request,
                                  loom_target_emit_artifact_t* out_artifact);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_ARCH_VM_MODULE_H_
