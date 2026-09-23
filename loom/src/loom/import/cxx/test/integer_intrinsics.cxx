// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <loomcxx/check.h>
#include <loomcxx/scalar.h>

// Each two-digit field records one count, with the source width retained
// through template deduction.
template <class T>
static unsigned counts(T value) {
  return static_cast<unsigned>(loom::scalar::ctlzi(value)) * 10000u +
         static_cast<unsigned>(loom::scalar::cttzi(value)) * 100u +
         static_cast<unsigned>(loom::scalar::ctpopi(value));
}

static unsigned once(unsigned value) {
  unsigned count = loom::scalar::cttzi(value++);
  return value * 100u + count;
}

// Constant narrow operands exercise source-type deduction and the shared
// scalar folds independently of any VM narrow-operation representation.
static unsigned narrow_counts() {
  return static_cast<unsigned>(
             loom::scalar::ctlzi(static_cast<unsigned char>(0))) *
             10000u +
         static_cast<unsigned>(
             loom::scalar::cttzi(static_cast<signed char>(-128))) *
             100u +
         static_cast<unsigned>(
             loom::scalar::ctpopi(static_cast<unsigned short>(0x8000)));
}

LOOM_CHECK_CASE(bit_counts) {
  const auto narrow = narrow_counts();
  const auto word_zero = counts(0u);
  const auto word_high = counts(0x80000000u);
  const auto word_all = counts(-1);
  const auto wide_zero = counts(0ull);
  const auto wide_high = counts(0x8000000000000000ull);
  const auto wide_sparse = counts(0x8000000100000000ull);
  const auto wide_all = counts(-1ll);
  const auto evaluated_once = once(8u);
  loom::check::expect_equal(narrow, 80701u);
  loom::check::expect_equal(word_zero, 323200u);
  loom::check::expect_equal(word_high, 3101u);
  loom::check::expect_equal(word_all, 32u);
  loom::check::expect_equal(wide_zero, 646400u);
  loom::check::expect_equal(wide_high, 6301u);
  loom::check::expect_equal(wide_sparse, 3202u);
  loom::check::expect_equal(wide_all, 64u);
  loom::check::expect_equal(evaluated_once, 903u);
}
