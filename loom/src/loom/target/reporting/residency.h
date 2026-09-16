// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Retained final residency constraints for individual compiled entries.

#ifndef LOOM_TARGET_REPORTING_RESIDENCY_H_
#define LOOM_TARGET_REPORTING_RESIDENCY_H_

#include "loom/target/residency.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_target_compile_report_residency_constraint_row_t {
  // Emitted entry function owning the constraint; never an aggregate identity.
  iree_string_view_t function_name;
  // Target-owned final facts, copied before the producer's arena expires.
  loom_target_residency_constraint_t constraint;
} loom_target_compile_report_residency_constraint_row_t;

struct loom_target_compile_report_t;

// Copies an entry's bounded resource inventory into report-owned row storage.
// Names and units retain the report's usual borrowed-string lifetime contract.
iree_status_t loom_target_compile_report_record_residency_constraints(
    struct loom_target_compile_report_t* report,
    const loom_target_residency_constraint_list_t* constraints);

// Returns the stable reporting spelling of a target constraint kind.
iree_string_view_t loom_target_compile_report_residency_constraint_kind_name(
    loom_target_residency_constraint_kind_t kind);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TARGET_REPORTING_RESIDENCY_H_
