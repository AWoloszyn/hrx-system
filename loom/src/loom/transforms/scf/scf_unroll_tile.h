// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Materialization of finite, explicitly scheduled SCF body copies.
//
// The caller owns finite-domain arithmetic and the insertion point. This
// component owns body-payload readiness, cross-iteration memory dependencies,
// and clone order for interleaved and recurrence schedules.

#ifndef LOOM_TRANSFORMS_SCF_SCF_UNROLL_TILE_H_
#define LOOM_TRANSFORMS_SCF_SCF_UNROLL_TILE_H_

#include "iree/base/api.h"
#include "loom/ops/scf/ops.h"
#include "loom/pass/types.h"
#include "loom/rewrite/rewriter.h"
#include "loom/util/fact_table.h"

#ifdef __cplusplus
extern "C" {
#endif

// Emits |iteration_count| copies of the verified |loop|'s body at |rewriter|'s
// insertion point using an interleaved or recurrence schedule. The caller has
// already proved and materialized the indices; |iteration_indices| is NULL
// only when the induction variable has no payload references. Carried input
// and output arrays each have loop->result_count values. An empty tile forwards
// the input values directly.
//
// Source IR and |fact_table| remain stable during emission. Temporary
// dependency and remapping state belongs to |scratch_arena| and is dead on
// return. Requested schedules that cannot be implemented emit a pass
// diagnostic; status failures describe allocation or IR construction failures.
// The caller removes the source loop only after successful emission without
// error diagnostics.
iree_status_t loom_scf_unroll_tile_emit(
    loom_pass_t* pass, loom_rewriter_t* rewriter,
    const loom_value_fact_table_t* fact_table, loom_op_t* loop,
    const loom_value_id_t* iteration_indices, uint32_t iteration_count,
    const loom_value_id_t* initial_carried_values,
    loom_scf_for_unroll_schedule_t schedule,
    iree_arena_allocator_t* scratch_arena,
    loom_value_id_t* final_carried_values);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_TRANSFORMS_SCF_SCF_UNROLL_TILE_H_
