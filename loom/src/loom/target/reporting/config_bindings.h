// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TARGET_REPORTING_CONFIG_BINDINGS_H_
#define LOOM_TARGET_REPORTING_CONFIG_BINDINGS_H_

#include "loom/target/reporting/report.h"

#ifdef __cplusplus
extern "C" {
#endif

// Appends bindings and their owned strings from |source| to |target|.
iree_status_t loom_target_compile_report_config_bindings_append_all(
    loom_target_compile_report_row_list_t* target,
    const loom_target_compile_report_row_list_t* source,
    iree_allocator_t allocator);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_REPORTING_CONFIG_BINDINGS_H_
