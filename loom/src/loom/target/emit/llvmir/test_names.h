// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TARGET_EMIT_LLVMIR_TEST_NAMES_H_
#define LOOM_TARGET_EMIT_LLVMIR_TEST_NAMES_H_

#include "loom/target/emit/llvmir/module.h"

#ifdef __cplusplus
extern "C" {
#endif

// Builds a function with repeated, unnamed, and quoted local names. Parameters,
// results, and blocks retain their distinct identities despite name collisions.
iree_status_t loom_llvmir_test_build_local_names_module(
    iree_allocator_t allocator, loom_llvmir_module_t** out_module);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_EMIT_LLVMIR_TEST_NAMES_H_
