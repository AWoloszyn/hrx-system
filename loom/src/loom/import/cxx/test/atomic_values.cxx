// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// The observers exercise the same implementations as the native kernel cases.
// Source-root selection keeps this module's checks on the ordinary callable
// ABI.
#include "atomic_builtins.cxx"
#include "atomics.cxx"

template <class T>
struct SequenceObservation {
  // Bitmask of independently verified atomic return values.
  unsigned passed;
  // Guard immediately before the updated word.
  T before;
  // Final value of the updated word.
  T value;
  // Guard immediately after the updated word.
  T after;
};

static SequenceObservation<int> observe_i32() {
  int storage[3] = {37, 37, 37};
  unsigned passed = atomic_i32(storage);
  return {passed, storage[0], storage[1], storage[2]};
}

static SequenceObservation<unsigned> observe_u32() {
  unsigned storage[3] = {37, 37, 37};
  unsigned passed = atomic_u32(storage);
  return {passed, storage[0], storage[1], storage[2]};
}

static SequenceObservation<long long> observe_i64() {
  long long storage[3] = {37, 37, 37};
  unsigned passed = atomic_i64(storage);
  return {passed, storage[0], storage[1], storage[2]};
}

static SequenceObservation<unsigned long long> observe_u64() {
  unsigned long long storage[3] = {37, 37, 37};
  unsigned passed = atomic_u64(storage);
  return {passed, storage[0], storage[1], storage[2]};
}

LOOM_CHECK_CASE(sequence_i32) {
  const auto actual = observe_i32();
  loom::check::expect_equal(actual.passed, 2047u);
  loom::check::expect_equal(actual.before, 37);
  loom::check::expect_equal(actual.value, 13);
  loom::check::expect_equal(actual.after, 37);
}

LOOM_CHECK_CASE(sequence_u32) {
  const auto actual = observe_u32();
  loom::check::expect_equal(actual.passed, 2047u);
  loom::check::expect_equal(actual.before, 37u);
  loom::check::expect_equal(actual.value, 13u);
  loom::check::expect_equal(actual.after, 37u);
}

LOOM_CHECK_CASE(sequence_i64) {
  const auto actual = observe_i64();
  loom::check::expect_equal(actual.passed, 2047u);
  loom::check::expect_equal(actual.before, 37LL);
  loom::check::expect_equal(actual.value, 13LL);
  loom::check::expect_equal(actual.after, 37LL);
}

LOOM_CHECK_CASE(sequence_u64) {
  const auto actual = observe_u64();
  loom::check::expect_equal(actual.passed, 2047u);
  loom::check::expect_equal(actual.before, 37ULL);
  loom::check::expect_equal(actual.value, 13ULL);
  loom::check::expect_equal(actual.after, 37ULL);
}

template <class T>
struct CompareObservation {
  // Failed comparisons of atomic return and expected-value semantics.
  unsigned failures;
  // Guard immediately before the atomic and expected-value slots.
  T before;
  // Final value of the atomically updated word.
  T value;
  // Expected-value slot written by a failed compare-exchange.
  T expected;
  // Guard immediately after the atomic and expected-value slots.
  T after;
};

struct CountedObservation {
  // Atomic values observed after all compare-exchange operations.
  CompareObservation<unsigned> compare;
  // Mismatches in argument evaluation counts and their neighboring guards.
  unsigned evaluation_failures;
};

static CountedObservation observe_builtin_compare() {
  // The trailing counters and their guards start at zero. Both helper
  // arguments name disjoint regions of this same automatic allocation.
  unsigned storage[10] = {37, 37, 37, 37};
  unsigned failures = builtin_compare_values(storage, storage + 4);
  unsigned evaluation_failures = 0;
  for (unsigned index = 4; index < 10; ++index) {
    unsigned wanted = index >= 5 && index < 9 ? 2 : 0;
    evaluation_failures += storage[index] != wanted;
  }
  return {{failures, storage[0], storage[1], storage[2], storage[3]},
          evaluation_failures};
}

static CompareObservation<unsigned long long> observe_builtin_wide() {
  unsigned long long storage[4] = {37, 37, 37, 37};
  unsigned failures = builtin_wide_values(storage);
  return {failures, storage[0], storage[1], storage[2], storage[3]};
}

LOOM_CHECK_CASE(builtin_compare_semantics) {
  const auto actual = observe_builtin_compare();
  loom::check::expect_equal(actual.compare.failures, 0u);
  loom::check::expect_equal(actual.compare.before, 37u);
  loom::check::expect_equal(actual.compare.value, 13u);
  loom::check::expect_equal(actual.compare.expected, 7u);
  loom::check::expect_equal(actual.compare.after, 37u);
  loom::check::expect_equal(actual.evaluation_failures, 0u);
}

LOOM_CHECK_CASE(builtin_wide_semantics) {
  const auto actual = observe_builtin_wide();
  loom::check::expect_equal(actual.failures, 0u);
  loom::check::expect_equal(actual.before, 37ULL);
  loom::check::expect_equal(actual.value, 0x100000004ULL);
  loom::check::expect_equal(actual.expected, 0x100000003ULL);
  loom::check::expect_equal(actual.after, 37ULL);
}
