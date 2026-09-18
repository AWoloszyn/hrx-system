// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TOOLS_LOOM_CHECK_COMPARISON_H_
#define LOOM_TOOLS_LOOM_CHECK_COMPARISON_H_

#include "loom/tools/loom-check/execute.h"

#ifdef __cplusplus
extern "C" {
#endif

// Compares actual_output against the case's exact golden or independent
// whole-line CHECK/CHECK-NOT globs. Records the verdict and mismatch details.
// Returns an error for malformed checks or allocation failure.
iree_status_t loom_check_compare_output(const loom_test_case_t* test_case,
                                        iree_allocator_t allocator,
                                        loom_check_result_t* result);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TOOLS_LOOM_CHECK_COMPARISON_H_
