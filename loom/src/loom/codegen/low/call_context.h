// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Target-context compatibility of structurally verified Low calls.

#ifndef LOOM_CODEGEN_LOW_CALL_CONTEXT_H_
#define LOOM_CODEGEN_LOW_CALL_CONTEXT_H_

#include "loom/error/emitter.h"
#include "loom/target/function_version.h"

#ifdef __cplusplus
extern "C" {
#endif

// Verifies the call edge from |caller| to the callee of |op|. Specialization's
// retained context identities supersede authored target witnesses. Without
// target versions, both functions must name the same authored target.
// Structural verification owns callee resolution, placement and signatures.
iree_status_t loom_low_verify_call_context(
    const loom_module_t* module, const loom_op_t* caller,
    const loom_target_function_version_t* caller_version,
    const loom_target_function_version_snapshot_t* versions,
    const loom_op_t* op, iree_diagnostic_emitter_t emitter);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_CODEGEN_LOW_CALL_CONTEXT_H_
