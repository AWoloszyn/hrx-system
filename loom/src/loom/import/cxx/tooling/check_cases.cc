// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "check_cases.h"

#ifndef CHECK_EXPECTED
#define CHECK_EXPECTED 0u
#endif

unsigned byte_increment(unsigned input) {
  return (unsigned char)next_value((unsigned char)input);
}

#define EXPECT_EQUAL(actual, expected) \
  loom::check::expect_equal(actual, expected)
LOOM_CHECK_CASE(wraps_byte) {
  const auto actual = byte_increment(255u);
  EXPECT_EQUAL(actual, CHECK_EXPECTED);
}
LOOM_CHECK_BENCHMARK(wraps_byte_benchmark, wraps_byte);

#ifdef CHECK_MATH
LOOM_CHECK_CASE(math_policy) {
  const auto actual = exact_sine(0.0f);
  loom::check::expect_equal(actual, 0.0f);
}
LOOM_CHECK_BENCHMARK(math_policy_benchmark, math_policy);
#endif
