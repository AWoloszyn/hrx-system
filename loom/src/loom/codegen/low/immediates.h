// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Canonical enum values at descriptor-backed Low construction boundaries.

#ifndef LOOM_CODEGEN_LOW_IMMEDIATES_H_
#define LOOM_CODEGEN_LOW_IMMEDIATES_H_

#include "loom/codegen/low/descriptors.h"
#include "loom/ir/module.h"

#ifdef __cplusplus
extern "C" {
#endif

// Resolves named enum immediates to their semantic i64 values. The input
// dictionary remains immutable; a replacement is allocated in the module only
// when a recognized token changes. Unrecognized tokens and malformed values
// remain available to descriptor verification for source diagnostics.
iree_status_t loom_low_resolve_immediate_enums(
    loom_module_t* module, const loom_low_descriptor_set_t* descriptor_set,
    const loom_low_descriptor_t* descriptor, loom_attribute_t* attrs);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_CODEGEN_LOW_IMMEDIATES_H_
