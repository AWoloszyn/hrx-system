// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Dialect-independent loop-domain proofs. SSA domains support equality and
// emptiness proofs from value facts; concrete integer recurrences support exact
// trip counts with finite-width comparison semantics. Callers own loop
// recognition and supply the domain's bounds and step.

#ifndef LOOM_ANALYSIS_LOOP_DOMAIN_H_
#define LOOM_ANALYSIS_LOOP_DOMAIN_H_

#include "iree/base/api.h"
#include "loom/ir/ir.h"
#include "loom/util/fact_table.h"

#ifdef __cplusplus
extern "C" {
#endif

// Half-open counted range [lower_bound, upper_bound) with a positive step.
// SSA value IDs keep the domain independent of the operation defining it.
typedef struct loom_loop_domain_t {
  // Inclusive lower bound of the counted loop domain.
  loom_value_id_t lower_bound;
  // Exclusive upper bound of the counted loop domain.
  loom_value_id_t upper_bound;
  // Positive step between consecutive induction values.
  loom_value_id_t step;
} loom_loop_domain_t;

// Returns true when both domains are proven identical by SSA identity or exact
// integer value facts. Non-exact facts deliberately do not prove equality: two
// values with the same range may still differ at runtime.
bool loom_loop_domain_equal(const loom_value_fact_table_t* fact_table,
                            loom_loop_domain_t lhs, loom_loop_domain_t rhs);

// Returns true when every value admitted by the domain facts produces zero
// iterations. The proof requires a positive integer step and lower_bound >=
// upper_bound for the complete fact ranges.
bool loom_loop_domain_proven_empty(const loom_value_fact_table_t* fact_table,
                                   loom_loop_domain_t domain);

// Returns true when every value admitted by the domain facts produces at least
// one iteration. The proof requires a positive integer step and lower_bound <
// upper_bound for the complete fact ranges.
bool loom_loop_domain_proven_nonempty(const loom_value_fact_table_t* fact_table,
                                      loom_loop_domain_t domain);

// Upper-bound comparison semantics for a header-tested integer recurrence.
enum loom_loop_bound_flag_bits_e {
  // Unsigned exclusive comparison: induction value < upper bound.
  LOOM_LOOP_BOUND_NONE = 0,
  // Use signed two's-complement order for the induction value and bound.
  LOOM_LOOP_BOUND_SIGNED = 1u << 0,
  // Continue while the induction value <= bound instead of < bound.
  LOOM_LOOP_BOUND_INCLUSIVE = 1u << 1,
};
typedef uint8_t loom_loop_bound_flags_t;

// Counts body executions of a header-tested loop whose backedge adds |step| to
// the induction value. |bound_flags| selects signed/unsigned and exclusive/
// inclusive comparison with |upper_bound|. The initial value, bound, and step
// are raw carrier bits; only their low |bitwidth| bits participate. |bitwidth|
// is the verified carrier width in [1, 64].
//
// A false initial guard proves zero trips independently of the step. Nonempty
// loops require an increasing recurrence that reaches the exit without wrapping
// in the comparison's ordered carrier domain, including the terminal increment.
// This differs from the mathematical cardinality of a counted range: a finite
// range may still require an overflowing terminal increment in a lowered loop.
// Returns false when this proof cannot establish an exact count; it does not
// imply that the loop is infinite. |out_trip_count| is zero on failure.
bool loom_loop_domain_trip_count(loom_loop_bound_flags_t bound_flags,
                                 uint8_t bitwidth, uint64_t initial_value,
                                 uint64_t upper_bound, uint64_t step,
                                 uint64_t* out_trip_count);

#ifdef __cplusplus
}
#endif

#endif  // LOOM_ANALYSIS_LOOP_DOMAIN_H_
