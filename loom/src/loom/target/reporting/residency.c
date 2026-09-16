// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/target/reporting/residency.h"

#include "loom/target/reporting/report.h"
#include "loom/target/reporting/row_list.h"

iree_status_t loom_target_compile_report_record_residency_constraints(
    loom_target_compile_report_t* report,
    const loom_target_residency_constraint_list_t* constraints) {
  report->detail_flags |=
      LOOM_TARGET_COMPILE_REPORT_DETAIL_RESIDENCY_CONSTRAINTS;
  iree_status_t status = iree_ok_status();
  for (iree_host_size_t i = 0;
       i < constraints->count && iree_status_is_ok(status); ++i) {
    const loom_target_compile_report_residency_constraint_row_t row = {
        .function_name = report->function_name,
        .constraint = constraints->rows[i],
    };
    status = loom_target_compile_report_row_list_append(
        &report->residency_constraint_rows, sizeof(row), report->allocator,
        &row);
  }
  return status;
}

iree_string_view_t loom_target_compile_report_residency_constraint_kind_name(
    loom_target_residency_constraint_kind_t kind) {
  static const iree_string_view_t names[] = {
      IREE_SVL("pooled_resource"),
      IREE_SVL("unconstrained_resource"),
      IREE_SVL("fixed_limit"),
  };
  return names[kind];
}
