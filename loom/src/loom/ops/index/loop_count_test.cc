// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/ops/index/loop_count.h"

#include "iree/testing/gtest.h"
#include "loom/ops/index/ops.h"

namespace loom {
namespace {

void ExpectCount(uint8_t predicate, uint8_t bitwidth, uint64_t initial,
                 uint64_t bound, uint64_t step, uint64_t expected) {
  uint64_t actual = UINT64_MAX;
  ASSERT_TRUE(loom_index_loop_trip_count(predicate, bitwidth, initial, bound,
                                         step, &actual));
  EXPECT_EQ(actual, expected);
}

void ExpectUnknown(uint8_t predicate, uint8_t bitwidth, uint64_t initial,
                   uint64_t bound, uint64_t step) {
  uint64_t actual = UINT64_MAX;
  EXPECT_FALSE(loom_index_loop_trip_count(predicate, bitwidth, initial, bound,
                                          step, &actual));
  EXPECT_EQ(actual, 0u);
}

TEST(IndexLoopCountTest, PredicateSignedness) {
  ExpectCount(LOOM_INDEX_CMP_PREDICATE_SLT, 32, -1, 4, 1, 5);
  ExpectCount(LOOM_INDEX_CMP_PREDICATE_ULT, 32, -1, 4, 1, 0);
  ExpectCount(LOOM_INDEX_CMP_PREDICATE_SLE, 32, -1, 4, 1, 6);
  ExpectCount(LOOM_INDEX_CMP_PREDICATE_ULE, 32, -1, 4, 1, 0);
  ExpectCount(LOOM_INDEX_CMP_PREDICATE_SLT, 64, -1, 4, 1, 5);
  ExpectCount(LOOM_INDEX_CMP_PREDICATE_ULT, 64, -1, 4, 1, 0);
  ExpectCount(LOOM_INDEX_CMP_PREDICATE_ULT, 32, INT32_MAX, INT32_MIN, 1, 1);
  ExpectCount(LOOM_INDEX_CMP_PREDICATE_SLT, 32, INT32_MAX, INT32_MIN, 1, 0);
}

TEST(IndexLoopCountTest, NonzeroStartsAndPartialFinalSteps) {
  ExpectCount(LOOM_INDEX_CMP_PREDICATE_SLT, 32, 7, 19, 4, 3);
  ExpectCount(LOOM_INDEX_CMP_PREDICATE_SLE, 32, 7, 19, 4, 4);
  ExpectCount(LOOM_INDEX_CMP_PREDICATE_SLT, 32, 7, 20, 4, 4);
  ExpectCount(LOOM_INDEX_CMP_PREDICATE_SLE, 32, 7, 20, 4, 4);
  ExpectCount(LOOM_INDEX_CMP_PREDICATE_SLT, 64, -9, 4, 5, 3);
  ExpectCount(LOOM_INDEX_CMP_PREDICATE_SLE, 64, -9, -4, 5, 2);
}

TEST(IndexLoopCountTest, TerminalIncrementMustFitCarrier) {
  ExpectCount(LOOM_INDEX_CMP_PREDICATE_SLT, 32, INT32_MAX - 1, INT32_MAX, 1, 1);
  ExpectUnknown(LOOM_INDEX_CMP_PREDICATE_SLT, 32, INT32_MAX - 1, INT32_MAX, 2);
  ExpectUnknown(LOOM_INDEX_CMP_PREDICATE_SLE, 32, INT32_MAX, INT32_MAX, 1);
  ExpectCount(LOOM_INDEX_CMP_PREDICATE_SLE, 32, INT32_MAX - 1, INT32_MAX - 1, 1,
              1);
  ExpectCount(LOOM_INDEX_CMP_PREDICATE_ULT, 32, UINT32_MAX - 1, UINT32_MAX, 1,
              1);
  ExpectUnknown(LOOM_INDEX_CMP_PREDICATE_ULT, 32, UINT32_MAX - 1, UINT32_MAX,
                2);
  ExpectUnknown(LOOM_INDEX_CMP_PREDICATE_ULE, 32, UINT32_MAX, UINT32_MAX, 1);
  ExpectCount(LOOM_INDEX_CMP_PREDICATE_SLT, 64, INT64_MAX - 1, INT64_MAX, 1, 1);
  ExpectUnknown(LOOM_INDEX_CMP_PREDICATE_SLT, 64, INT64_MAX - 1, INT64_MAX, 2);
  ExpectUnknown(LOOM_INDEX_CMP_PREDICATE_SLE, 64, INT64_MAX, INT64_MAX, 1);
  ExpectCount(LOOM_INDEX_CMP_PREDICATE_ULT, 64, UINT64_MAX - 1, UINT64_MAX, 1,
              1);
  ExpectUnknown(LOOM_INDEX_CMP_PREDICATE_ULT, 64, UINT64_MAX - 1, UINT64_MAX,
                2);
  ExpectUnknown(LOOM_INDEX_CMP_PREDICATE_ULE, 64, UINT64_MAX, UINT64_MAX, 1);
}

TEST(IndexLoopCountTest, FullCarrierSpans) {
  ExpectCount(LOOM_INDEX_CMP_PREDICATE_SLT, 64, INT64_MIN, INT64_MAX, 1,
              UINT64_MAX);
  ExpectCount(LOOM_INDEX_CMP_PREDICATE_SLT, 64, INT64_MIN, 0, UINT64_C(1) << 63,
              1);
  ExpectCount(LOOM_INDEX_CMP_PREDICATE_ULT, 64, 0, UINT64_MAX, 1, UINT64_MAX);
  ExpectUnknown(LOOM_INDEX_CMP_PREDICATE_SLE, 64, INT64_MIN, INT64_MAX, 1);
  ExpectUnknown(LOOM_INDEX_CMP_PREDICATE_ULE, 64, 0, UINT64_MAX, 1);
  ExpectUnknown(LOOM_INDEX_CMP_PREDICATE_ULT, 64, 0, UINT64_MAX, 2);
}

TEST(IndexLoopCountTest, EmptyLoopsAndZeroIncrements) {
  ExpectCount(LOOM_INDEX_CMP_PREDICATE_SLT, 32, 4, 4, 0, 0);
  ExpectCount(LOOM_INDEX_CMP_PREDICATE_ULE, 32, 5, 4, -1, 0);
  ExpectUnknown(LOOM_INDEX_CMP_PREDICATE_SLT, 32, 3, 4, 0);
  ExpectUnknown(LOOM_INDEX_CMP_PREDICATE_SLE, 32, 4, 4, 0);
  ExpectUnknown(LOOM_INDEX_CMP_PREDICATE_ULT, 32, 0, 4, UINT64_C(1) << 32);
}

TEST(IndexLoopCountTest, TruncatesAllInputsToSelectedCarrier) {
  ExpectCount(LOOM_INDEX_CMP_PREDICATE_SLT, 32, (UINT64_C(1) << 32) + 3,
              (UINT64_C(1) << 33) + 9, (UINT64_C(1) << 34) + 2, 3);
  ExpectCount(LOOM_INDEX_CMP_PREDICATE_ULT, 32, UINT64_MAX, 4, 1, 0);
  ExpectCount(LOOM_INDEX_CMP_PREDICATE_SLT, 32, UINT32_MAX, 4, 1, 5);
}

TEST(IndexLoopCountTest, OtherPredicatesHaveNoCountProof) {
  for (uint8_t predicate :
       {LOOM_INDEX_CMP_PREDICATE_EQ, LOOM_INDEX_CMP_PREDICATE_NE,
        LOOM_INDEX_CMP_PREDICATE_SGT, LOOM_INDEX_CMP_PREDICATE_SGE,
        LOOM_INDEX_CMP_PREDICATE_UGT, LOOM_INDEX_CMP_PREDICATE_UGE}) {
    ExpectUnknown(predicate, 32, 0, 4, 1);
  }
}

struct ObservedLoop {
  // Whether modular execution reaches a false guard before revisiting a value.
  bool terminates;
  // Whether every executed addition advances in the predicate's comparison
  // order.
  bool increases;
  // Number of body executions before exit or recurrence.
  uint64_t trip_count;
};

// Small-width interpretation is independent of the closed-form count proof.
// Execute carrier additions and comparisons, stopping when a value repeats.
ObservedLoop InterpretLoop(uint8_t predicate, uint8_t bitwidth,
                           uint64_t initial, uint64_t bound, uint64_t step) {
  const int64_t modulus = INT64_C(1) << bitwidth;
  const bool is_signed = predicate == LOOM_INDEX_CMP_PREDICATE_SLT ||
                         predicate == LOOM_INDEX_CMP_PREDICATE_SLE;
  auto ordered_value = [&](uint64_t bits) -> int64_t {
    return is_signed && bits >= uint64_t(modulus / 2) ? int64_t(bits) - modulus
                                                      : int64_t(bits);
  };
  ObservedLoop observed = {true, true, 0};
  uint64_t value = initial;
  do {
    bool more = predicate == LOOM_INDEX_CMP_PREDICATE_SLE ||
                        predicate == LOOM_INDEX_CMP_PREDICATE_ULE
                    ? ordered_value(value) <= ordered_value(bound)
                    : ordered_value(value) < ordered_value(bound);
    if (!more) {
      return observed;
    }
    uint64_t next = (value + step) % modulus;
    observed.increases &= ordered_value(next) > ordered_value(value);
    ++observed.trip_count;
    value = next;
  } while (value != initial);
  observed.terminates = false;
  return observed;
}

TEST(IndexLoopCountTest, ExhaustiveModularExecution) {
  for (uint8_t bitwidth = 1; bitwidth <= 6; ++bitwidth) {
    const uint64_t limit = UINT64_C(1) << bitwidth;
    for (uint8_t predicate :
         {LOOM_INDEX_CMP_PREDICATE_SLT, LOOM_INDEX_CMP_PREDICATE_SLE,
          LOOM_INDEX_CMP_PREDICATE_ULT, LOOM_INDEX_CMP_PREDICATE_ULE}) {
      for (uint64_t initial = 0; initial < limit; ++initial) {
        for (uint64_t bound = 0; bound < limit; ++bound) {
          for (uint64_t step = 0; step < limit; ++step) {
            const ObservedLoop expected =
                InterpretLoop(predicate, bitwidth, initial, bound, step);
            uint64_t actual = UINT64_MAX;
            const bool exact = loom_index_loop_trip_count(
                predicate, bitwidth, initial, bound, step, &actual);
            const bool expected_exact =
                expected.terminates && expected.increases;
            if (exact != expected_exact ||
                actual != (expected_exact ? expected.trip_count : 0)) {
              FAIL() << "width=" << int(bitwidth)
                     << " predicate=" << int(predicate)
                     << " initial=" << initial << " bound=" << bound
                     << " step=" << step << " exact=" << exact
                     << " count=" << actual
                     << " expected_exact=" << expected_exact
                     << " expected_count=" << expected.trip_count;
            }
          }
        }
      }
    }
  }
}

}  // namespace
}  // namespace loom
