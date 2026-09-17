// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/util/reference_facts.h"

bool loom_value_fact_reference_origin_equal(
    loom_value_fact_reference_origin_t lhs,
    loom_value_fact_reference_origin_t rhs) {
  return lhs.kind == rhs.kind &&
         lhs.function_symbol_id == rhs.function_symbol_id &&
         lhs.region_index == rhs.region_index &&
         lhs.entry_value_id == rhs.entry_value_id;
}

loom_value_fact_reference_origin_t loom_value_fact_reference_origin_meet(
    loom_value_fact_reference_origin_t lhs,
    loom_value_fact_reference_origin_t rhs) {
  if (lhs.kind != rhs.kind ||
      lhs.function_symbol_id != rhs.function_symbol_id ||
      lhs.region_index != rhs.region_index) {
    return (loom_value_fact_reference_origin_t){0};
  }
  if (lhs.entry_value_id != rhs.entry_value_id) {
    lhs.entry_value_id = LOOM_VALUE_ID_INVALID;
  }
  return lhs;
}

bool loom_value_fact_reference_origins_are_disjoint(
    loom_value_fact_reference_origin_t lhs,
    loom_value_fact_reference_origin_t rhs) {
  return lhs.kind != LOOM_VALUE_FACT_REFERENCE_ORIGIN_UNKNOWN &&
         rhs.kind != LOOM_VALUE_FACT_REFERENCE_ORIGIN_UNKNOWN &&
         lhs.kind != rhs.kind &&
         lhs.function_symbol_id == rhs.function_symbol_id &&
         lhs.region_index == rhs.region_index;
}
