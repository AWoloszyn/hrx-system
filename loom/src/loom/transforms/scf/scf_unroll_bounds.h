// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Representable main/tail split construction for partially unrolled SCF loops.

#ifndef LOOM_TRANSFORMS_SCF_SCF_UNROLL_BOUNDS_H_
#define LOOM_TRANSFORMS_SCF_SCF_UNROLL_BOUNDS_H_

#include "loom/ops/op_defs.h"
#include "loom/util/fact_table.h"

#ifdef __cplusplus
extern "C" {
#endif

// Builds the shared main-loop upper bound and tail-loop lower bound for a
// verified positive-step loop. The caller has proved step * factor fits the
// selected address carrier and materialized it as |scaled_step|. The main loop
// visits complete tiles and the tail visits the remaining source iterations.
// When rounding up past the source upper bound could overflow, the split stays
// between min(lower, upper) and upper. Original facts remain valid while this
// appends arithmetic at |builder|'s insertion point.
iree_status_t loom_scf_unroll_build_dynamic_split(
    loom_builder_t* builder, const loom_value_fact_table_t* facts,
    const loom_op_t* op, int64_t step, uint32_t factor,
    loom_value_id_t scaled_step, loom_value_id_t* out_main_upper);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TRANSFORMS_SCF_SCF_UNROLL_BOUNDS_H_
