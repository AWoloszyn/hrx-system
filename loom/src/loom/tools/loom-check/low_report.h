// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TOOLS_LOOM_CHECK_LOW_REPORT_H_
#define LOOM_TOOLS_LOOM_CHECK_LOW_REPORT_H_

#include "loom/tools/loom-check/execute.h"

#ifdef __cplusplus
extern "C" {
#endif

// Builds a Low emission frame and formats its compile report using the linked
// descriptor registry. The caller verifies the module and matches diagnostics.
iree_status_t loom_check_emit_low_report(
    loom_module_t* module, iree_string_view_t symbol_name,
    const loom_low_descriptor_registry_t* descriptor_registry,
    const loom_test_case_t* test_case, iree_string_view_t filename,
    loom_check_diagnostic_collector_t* diagnostic_collector,
    iree_diagnostic_emitter_t emitter, iree_arena_allocator_t* arena,
    loom_check_result_t* result);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TOOLS_LOOM_CHECK_LOW_REPORT_H_
