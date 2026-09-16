// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_TARGET_REPORTING_FORMAT_LOOP_PIPELINE_H_
#define LOOM_TARGET_REPORTING_FORMAT_LOOP_PIPELINE_H_

#include "loom/target/reporting/format.h"

#ifdef __cplusplus
extern "C" {
#endif

// Writes applied source policies, with operation schedules in details mode.
iree_status_t loom_target_compile_report_format_loop_pipelines_json(
    const loom_target_compile_report_t* report,
    loom_target_compile_report_format_mode_t mode,
    loom_output_stream_t* stream);

// Appends applied source policies and optional operation schedules.
iree_status_t loom_target_compile_report_format_loop_pipelines_text(
    const loom_target_compile_report_t* report,
    loom_target_compile_report_format_mode_t mode,
    iree_string_builder_t* builder);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_REPORTING_FORMAT_LOOP_PIPELINE_H_
