// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Shared effect semantics for operations that reference callable symbols.

#ifndef LOOM_OPS_CALLABLE_EFFECTS_H_
#define LOOM_OPS_CALLABLE_EFFECTS_H_

#include "iree/base/api.h"
#include "loom/ir/ir.h"

#ifdef __cplusplus
extern "C" {
#endif

// Returns true when |function| has pure callable semantics. Definitions are
// classified from their cached body effects while declarations use their
// explicit purity contract.
bool loom_callable_effects_is_pure(loom_func_like_t function);

// Returns true when |callee| resolves to a pure callable in |module|.
// Unresolved symbols and impure callees return false.
bool loom_callable_effects_callee_is_pure(const loom_module_t* module,
                                          loom_symbol_ref_t callee);

// Returns the effective traits for a callable application with |purity|.
// Its callable boundary remains independent of the selected effects.
loom_trait_flags_t loom_callable_effects_traits(uint8_t purity);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_OPS_CALLABLE_EFFECTS_H_
