// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TOOLS_LOOM_CHECK_INPUT_H_
#define LOOM_TOOLS_LOOM_CHECK_INPUT_H_

#include "loom/tools/loom-check/execute.h"

#ifdef __cplusplus
extern "C" {
#endif

// Loads case input through its selected source provider. Only Loom text strips
// standalone fixture comments; foreign languages receive the exact case bytes.
// Always deinitialize the output, including failures and source rejection.
iree_status_t loom_check_load_input(
    const loom_test_case_t* test_case, const loom_input_request_t* request,
    const loom_check_environment_t* environment, loom_context_t* context,
    iree_arena_block_pool_t* block_pool,
    const loom_text_parse_options_t* parse_options,
    iree_allocator_t host_allocator, loom_input_module_t* out_input);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TOOLS_LOOM_CHECK_INPUT_H_
