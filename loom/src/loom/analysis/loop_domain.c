// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/analysis/loop_domain.h"

#include "loom/ir/facts.h"

static bool loom_loop_domain_value_equal(
    const loom_value_fact_table_t* fact_table, loom_value_id_t lhs,
    loom_value_id_t rhs) {
  if (lhs == LOOM_VALUE_ID_INVALID || rhs == LOOM_VALUE_ID_INVALID) {
    return false;
  }
  if (lhs == rhs) {
    return true;
  }
  if (!fact_table) {
    return false;
  }

  loom_value_facts_t lhs_facts = loom_value_fact_table_lookup(fact_table, lhs);
  loom_value_facts_t rhs_facts = loom_value_fact_table_lookup(fact_table, rhs);
  return loom_value_facts_is_exact(lhs_facts) &&
         loom_value_facts_is_exact(rhs_facts) &&
         !loom_value_facts_is_float(lhs_facts) &&
         !loom_value_facts_is_float(rhs_facts) &&
         lhs_facts.range_lo == rhs_facts.range_lo;
}

bool loom_loop_domain_equal(const loom_value_fact_table_t* fact_table,
                            loom_loop_domain_t lhs, loom_loop_domain_t rhs) {
  return loom_loop_domain_value_equal(fact_table, lhs.lower_bound,
                                      rhs.lower_bound) &&
         loom_loop_domain_value_equal(fact_table, lhs.upper_bound,
                                      rhs.upper_bound) &&
         loom_loop_domain_value_equal(fact_table, lhs.step, rhs.step);
}

static bool loom_loop_domain_lookup_range_facts(
    const loom_value_fact_table_t* fact_table, loom_loop_domain_t domain,
    loom_value_facts_t* out_lower_bound, loom_value_facts_t* out_upper_bound,
    loom_value_facts_t* out_step) {
  if (!fact_table || domain.lower_bound == LOOM_VALUE_ID_INVALID ||
      domain.upper_bound == LOOM_VALUE_ID_INVALID ||
      domain.step == LOOM_VALUE_ID_INVALID) {
    return false;
  }
  *out_lower_bound =
      loom_value_fact_table_lookup(fact_table, domain.lower_bound);
  *out_upper_bound =
      loom_value_fact_table_lookup(fact_table, domain.upper_bound);
  *out_step = loom_value_fact_table_lookup(fact_table, domain.step);
  return !loom_value_facts_is_float(*out_lower_bound) &&
         !loom_value_facts_is_float(*out_upper_bound) &&
         !loom_value_facts_is_float(*out_step) &&
         loom_value_facts_is_positive(*out_step);
}

bool loom_loop_domain_proven_empty(const loom_value_fact_table_t* fact_table,
                                   loom_loop_domain_t domain) {
  loom_value_facts_t lower_bound = {0};
  loom_value_facts_t upper_bound = {0};
  loom_value_facts_t step = {0};
  if (!loom_loop_domain_lookup_range_facts(fact_table, domain, &lower_bound,
                                           &upper_bound, &step)) {
    return false;
  }
  return lower_bound.range_lo >= upper_bound.range_hi;
}

bool loom_loop_domain_proven_nonempty(const loom_value_fact_table_t* fact_table,
                                      loom_loop_domain_t domain) {
  loom_value_facts_t lower_bound = {0};
  loom_value_facts_t upper_bound = {0};
  loom_value_facts_t step = {0};
  if (!loom_loop_domain_lookup_range_facts(fact_table, domain, &lower_bound,
                                           &upper_bound, &step)) {
    return false;
  }
  return lower_bound.range_hi < upper_bound.range_lo;
}

bool loom_loop_domain_trip_count(loom_loop_bound_flags_t bound_flags,
                                 uint8_t bitwidth, uint64_t initial_value,
                                 uint64_t upper_bound, uint64_t step,
                                 uint64_t* out_trip_count) {
  *out_trip_count = 0;
  const bool is_signed = iree_any_bit_set(bound_flags, LOOM_LOOP_BOUND_SIGNED);
  const bool is_inclusive =
      iree_any_bit_set(bound_flags, LOOM_LOOP_BOUND_INCLUSIVE);

  // Flipping the sign bit maps signed order to unsigned order. Modular addition
  // is unchanged by this rotation, so both comparison domains share one proof.
  const uint64_t mask = UINT64_MAX >> (64 - bitwidth);
  const uint64_t sign_bit = is_signed ? UINT64_C(1) << (bitwidth - 1) : 0;
  const uint64_t initial = (initial_value & mask) ^ sign_bit;
  const uint64_t upper = (upper_bound & mask) ^ sign_bit;
  const uint64_t increment = step & mask;
  if (initial > upper || (initial == upper && !is_inclusive)) {
    return true;
  }
  if (increment == 0 || (is_inclusive && upper == mask)) {
    return false;
  }

  const uint64_t distance = upper - initial + (is_inclusive ? 1 : 0);
  const uint64_t trip_count = (distance - 1) / increment + 1;
  if (trip_count > (mask - initial) / increment) {
    return false;
  }
  *out_trip_count = trip_count;
  return true;
}
