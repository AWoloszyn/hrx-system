// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_IMPORT_CXX_TOOLING_INPUT_H_
#define LOOM_IMPORT_CXX_TOOLING_INPUT_H_

#include "loom/tooling/input/input.h"

#ifdef __cplusplus
extern "C" {
#endif

// Native C/C++ source admission for build-enabled tools. Options are whitespace
// separated key=value tokens: std, triple, data-model, I, isystem, D, root,
// approximate-functions, and builtin-includes. Quoted values may contain
// spaces; quotes and backslashes can be escaped inside quotes. I/isystem paths
// are relative to the source file, and I/isystem/D/root may be repeated.
extern const loom_input_provider_t loom_cxx_input_provider;

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_IMPORT_CXX_TOOLING_INPUT_H_
