// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/ops/index/loop_count.h"

#include "loom/ops/index/ops.h"

bool loom_index_loop_trip_count(uint8_t predicate, uint8_t bitwidth,
                                uint64_t initial_value, uint64_t upper_bound,
                                uint64_t step, uint64_t* out_trip_count) {
  *out_trip_count = 0;
  bool is_signed = false;
  bool is_inclusive = false;
  switch ((loom_index_cmp_predicate_t)predicate) {
    case LOOM_INDEX_CMP_PREDICATE_SLT:
      is_signed = true;
      break;
    case LOOM_INDEX_CMP_PREDICATE_SLE:
      is_signed = true;
      is_inclusive = true;
      break;
    case LOOM_INDEX_CMP_PREDICATE_ULT:
      break;
    case LOOM_INDEX_CMP_PREDICATE_ULE:
      is_inclusive = true;
      break;
    default:
      return false;
  }

  // Flipping the sign bit maps signed order to unsigned order. Modular addition
  // is unchanged by this rotation, so both predicate domains share one proof.
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
