// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Structured loop fact solving. The loop owner seeds counted induction values,
// solves the carried-state forwarding equations, and publishes body and result
// facts together. Nested regions use the table's current function context.

#ifndef LOOM_UTIL_FACT_LOOP_H_
#define LOOM_UTIL_FACT_LOOP_H_

#include "loom/util/fact_table.h"

#ifdef __cplusplus
extern "C" {
#endif

// Seeds the counted induction argument when block is the LoopLike body entry.
iree_status_t loom_value_fact_table_seed_loop_iv_arg(
    loom_value_fact_table_t* table, const loom_module_t* module,
    const loom_block_t* block, loom_op_t* parent_op);

// Computes the complete LoopLike summary, including nested region facts.
// Reports result changes to the caller-maintained rewrite worklist.
iree_status_t loom_value_fact_table_compute_loop_like_summary(
    loom_value_fact_table_t* table, const loom_module_t* module, loom_op_t* op,
    bool* out_changed);

#ifdef __cplusplus
}
#endif

#endif  // LOOM_UTIL_FACT_LOOP_H_
