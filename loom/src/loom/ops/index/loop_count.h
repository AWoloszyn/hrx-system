// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Exact counts for guarded index.add recurrences.

#ifndef LOOM_OPS_INDEX_LOOP_COUNT_H_
#define LOOM_OPS_INDEX_LOOP_COUNT_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Counts body executions of a header-tested index.cmp loop whose backedge adds
// |step| to the induction value. The initial value, bound, and step are raw
// carrier bits; only their low |bitwidth| bits participate. |bitwidth| is the
// verified target carrier width in [1, 64].
//
// Signed/unsigned less-than and less-or-equal predicates are supported. A false
// initial guard proves zero trips independently of the step. Nonempty loops
// require an increasing recurrence that reaches the exit without wrapping in
// the predicate's ordered carrier domain, including the terminal increment.
// Returns false when this proof cannot establish an exact count; it does not
// imply that the loop is infinite. |out_trip_count| is zero on failure.
bool loom_index_loop_trip_count(uint8_t predicate, uint8_t bitwidth,
                                uint64_t initial_value, uint64_t upper_bound,
                                uint64_t step, uint64_t* out_trip_count);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_OPS_INDEX_LOOP_COUNT_H_
