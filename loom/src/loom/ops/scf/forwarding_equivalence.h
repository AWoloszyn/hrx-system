// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_OPS_SCF_FORWARDING_EQUIVALENCE_H_
#define LOOM_OPS_SCF_FORWARDING_EQUIVALENCE_H_

#include "iree/base/internal/arena.h"
#include "loom/util/fact_table.h"

#ifdef __cplusplus
extern "C" {
#endif

// Marks a forwarding state whose next value comes from an opaque terminal
// instead of another state in the same recurrence.
#define LOOM_SCF_FORWARDING_TERMINAL UINT16_MAX

// Exact colored functional graph describing one forwarding recurrence.
typedef struct loom_scf_forwarding_equivalence_problem_t {
  // Exact fact owner used to canonicalize initial and terminal SSA identities.
  const loom_value_fact_table_t* fact_table;

  // Initial SSA value for each recurrence state.
  const loom_value_id_t* initial_values;

  // Yielded SSA value for each recurrence state. Values are only observed for
  // states whose successor is LOOM_SCF_FORWARDING_TERMINAL.
  const loom_value_id_t* yielded_values;

  // Successor state ordinals, or LOOM_SCF_FORWARDING_TERMINAL. The solver
  // consumes this array as predecessor-list storage.
  uint16_t* successors;

  // Number of states in each array.
  uint16_t count;
} loom_scf_forwarding_equivalence_problem_t;

// Computes the coarsest exact equivalence over |problem|. Each output entry is
// the smallest state ordinal in its class. Temporary storage is allocated from
// |scratch_arena| and remains owned by it. Failure is limited to allocation.
iree_status_t loom_scf_forwarding_equivalence_partition(
    loom_scf_forwarding_equivalence_problem_t problem,
    iree_arena_allocator_t* scratch_arena, uint16_t* out_representatives,
    uint16_t* out_class_count);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_OPS_SCF_FORWARDING_EQUIVALENCE_H_
