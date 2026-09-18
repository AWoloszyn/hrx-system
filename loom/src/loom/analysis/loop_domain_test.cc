// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/analysis/loop_domain.h"

#include <cstdint>

#include "iree/base/internal/arena.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/ir/facts.h"
#include "loom/ir/float_facts.h"

namespace loom {
namespace {

class LoopDomainTest : public ::testing::Test {
 protected:
  void SetUp() override {
    iree_arena_block_pool_initialize(4096, iree_allocator_system(),
                                     &block_pool_);
    iree_arena_initialize(&block_pool_, &arena_);
    IREE_ASSERT_OK(loom_value_fact_table_initialize(&fact_table_, &arena_, 16));
  }

  void TearDown() override {
    iree_arena_deinitialize(&arena_);
    iree_arena_block_pool_deinitialize(&block_pool_);
  }

  void DefineFacts(loom_value_id_t value_id, loom_value_facts_t facts) {
    IREE_ASSERT_OK(loom_value_fact_table_define(&fact_table_, value_id, facts));
  }

  iree_arena_block_pool_t block_pool_;
  iree_arena_allocator_t arena_;
  loom_value_fact_table_t fact_table_;
};

TEST_F(LoopDomainTest, SameSsaValuesAreEqualWithoutFacts) {
  loom_loop_domain_t domain = {
      /*.lower_bound=*/1,
      /*.upper_bound=*/2,
      /*.step=*/3,
  };

  EXPECT_TRUE(loom_loop_domain_equal(nullptr, domain, domain));
}

TEST_F(LoopDomainTest, InvalidValuesAreNotEqual) {
  loom_loop_domain_t domain = {
      /*.lower_bound=*/LOOM_VALUE_ID_INVALID,
      /*.upper_bound=*/2,
      /*.step=*/3,
  };

  EXPECT_FALSE(loom_loop_domain_equal(nullptr, domain, domain));
}

TEST_F(LoopDomainTest, ExactIntegerFactsProveEquivalentValues) {
  DefineFacts(1, loom_value_facts_exact_i64(0));
  DefineFacts(2, loom_value_facts_exact_i64(16));
  DefineFacts(3, loom_value_facts_exact_i64(1));
  DefineFacts(4, loom_value_facts_exact_i64(0));
  DefineFacts(5, loom_value_facts_exact_i64(16));
  DefineFacts(6, loom_value_facts_exact_i64(1));

  loom_loop_domain_t lhs = {
      /*.lower_bound=*/1,
      /*.upper_bound=*/2,
      /*.step=*/3,
  };
  loom_loop_domain_t rhs = {
      /*.lower_bound=*/4,
      /*.upper_bound=*/5,
      /*.step=*/6,
  };

  EXPECT_TRUE(loom_loop_domain_equal(&fact_table_, lhs, rhs));
}

TEST_F(LoopDomainTest, RangeFactsDoNotProveEquality) {
  DefineFacts(1, loom_value_facts_make(0, 16, 1));
  DefineFacts(2, loom_value_facts_exact_i64(16));
  DefineFacts(3, loom_value_facts_exact_i64(1));
  DefineFacts(4, loom_value_facts_make(0, 16, 1));

  loom_loop_domain_t lhs = {
      /*.lower_bound=*/1,
      /*.upper_bound=*/2,
      /*.step=*/3,
  };
  loom_loop_domain_t rhs = {
      /*.lower_bound=*/4,
      /*.upper_bound=*/2,
      /*.step=*/3,
  };

  EXPECT_FALSE(loom_loop_domain_equal(&fact_table_, lhs, rhs));
}

TEST_F(LoopDomainTest, FloatFactsDoNotProveEquality) {
  DefineFacts(1, loom_value_facts_exact_float(LOOM_SCALAR_TYPE_F32, 0.0));
  DefineFacts(2, loom_value_facts_exact_i64(16));
  DefineFacts(3, loom_value_facts_exact_i64(1));
  DefineFacts(4, loom_value_facts_exact_float(LOOM_SCALAR_TYPE_F32, 0.0));

  loom_loop_domain_t lhs = {
      /*.lower_bound=*/1,
      /*.upper_bound=*/2,
      /*.step=*/3,
  };
  loom_loop_domain_t rhs = {
      /*.lower_bound=*/4,
      /*.upper_bound=*/2,
      /*.step=*/3,
  };

  EXPECT_FALSE(loom_loop_domain_equal(&fact_table_, lhs, rhs));
}

TEST_F(LoopDomainTest, RangeFactsProveNonemptyDomain) {
  DefineFacts(1, loom_value_facts_make(0, 4, 1));
  DefineFacts(2, loom_value_facts_make(8, 16, 1));
  DefineFacts(3, loom_value_facts_make(1, 4, 1));
  loom_loop_domain_t domain = {
      /*.lower_bound=*/1,
      /*.upper_bound=*/2,
      /*.step=*/3,
  };

  EXPECT_TRUE(loom_loop_domain_proven_nonempty(&fact_table_, domain));
  EXPECT_FALSE(loom_loop_domain_proven_empty(&fact_table_, domain));
}

TEST_F(LoopDomainTest, RangeFactsProveEmptyDomain) {
  DefineFacts(1, loom_value_facts_make(16, 24, 1));
  DefineFacts(2, loom_value_facts_make(0, 16, 1));
  DefineFacts(3, loom_value_facts_exact_i64(1));
  loom_loop_domain_t domain = {
      /*.lower_bound=*/1,
      /*.upper_bound=*/2,
      /*.step=*/3,
  };

  EXPECT_TRUE(loom_loop_domain_proven_empty(&fact_table_, domain));
  EXPECT_FALSE(loom_loop_domain_proven_nonempty(&fact_table_, domain));
}

TEST_F(LoopDomainTest, OverlappingBoundsProveNeitherDomainState) {
  DefineFacts(1, loom_value_facts_make(0, 12, 1));
  DefineFacts(2, loom_value_facts_make(8, 16, 1));
  DefineFacts(3, loom_value_facts_exact_i64(1));
  loom_loop_domain_t domain = {
      /*.lower_bound=*/1,
      /*.upper_bound=*/2,
      /*.step=*/3,
  };

  EXPECT_FALSE(loom_loop_domain_proven_empty(&fact_table_, domain));
  EXPECT_FALSE(loom_loop_domain_proven_nonempty(&fact_table_, domain));
}

TEST_F(LoopDomainTest, NonpositiveStepProvesNeitherDomainState) {
  DefineFacts(1, loom_value_facts_exact_i64(0));
  DefineFacts(2, loom_value_facts_exact_i64(16));
  DefineFacts(3, loom_value_facts_exact_i64(0));
  loom_loop_domain_t domain = {
      /*.lower_bound=*/1,
      /*.upper_bound=*/2,
      /*.step=*/3,
  };

  EXPECT_FALSE(loom_loop_domain_proven_empty(&fact_table_, domain));
  EXPECT_FALSE(loom_loop_domain_proven_nonempty(&fact_table_, domain));
}

constexpr loom_loop_bound_flags_t kUnsignedExclusive = LOOM_LOOP_BOUND_NONE;
constexpr loom_loop_bound_flags_t kUnsignedInclusive =
    LOOM_LOOP_BOUND_INCLUSIVE;
constexpr loom_loop_bound_flags_t kSignedExclusive = LOOM_LOOP_BOUND_SIGNED;
constexpr loom_loop_bound_flags_t kSignedInclusive =
    LOOM_LOOP_BOUND_SIGNED | LOOM_LOOP_BOUND_INCLUSIVE;

void ExpectCount(loom_loop_bound_flags_t bound_flags, uint8_t bitwidth,
                 uint64_t initial, uint64_t bound, uint64_t step,
                 uint64_t expected) {
  uint64_t actual = UINT64_MAX;
  ASSERT_TRUE(loom_loop_domain_trip_count(bound_flags, bitwidth, initial, bound,
                                          step, &actual));
  EXPECT_EQ(actual, expected);
}

void ExpectUnknown(loom_loop_bound_flags_t bound_flags, uint8_t bitwidth,
                   uint64_t initial, uint64_t bound, uint64_t step) {
  uint64_t actual = UINT64_MAX;
  EXPECT_FALSE(loom_loop_domain_trip_count(bound_flags, bitwidth, initial,
                                           bound, step, &actual));
  EXPECT_EQ(actual, 0u);
}

TEST(LoopDomainTripCountTest, ComparisonSignedness) {
  ExpectCount(kSignedExclusive, 32, -1, 4, 1, 5);
  ExpectCount(kUnsignedExclusive, 32, -1, 4, 1, 0);
  ExpectCount(kSignedInclusive, 32, -1, 4, 1, 6);
  ExpectCount(kUnsignedInclusive, 32, -1, 4, 1, 0);
  ExpectCount(kSignedExclusive, 64, -1, 4, 1, 5);
  ExpectCount(kUnsignedExclusive, 64, -1, 4, 1, 0);
  ExpectCount(kUnsignedExclusive, 32, INT32_MAX, INT32_MIN, 1, 1);
  ExpectCount(kSignedExclusive, 32, INT32_MAX, INT32_MIN, 1, 0);
}

TEST(LoopDomainTripCountTest, NonzeroStartsAndPartialFinalSteps) {
  ExpectCount(kSignedExclusive, 32, 7, 19, 4, 3);
  ExpectCount(kSignedInclusive, 32, 7, 19, 4, 4);
  ExpectCount(kSignedExclusive, 32, 7, 20, 4, 4);
  ExpectCount(kSignedInclusive, 32, 7, 20, 4, 4);
  ExpectCount(kSignedExclusive, 64, -9, 4, 5, 3);
  ExpectCount(kSignedInclusive, 64, -9, -4, 5, 2);
}

TEST(LoopDomainTripCountTest, TerminalIncrementMustFitCarrier) {
  ExpectCount(kSignedExclusive, 32, INT32_MAX - 1, INT32_MAX, 1, 1);
  ExpectUnknown(kSignedExclusive, 32, INT32_MAX - 1, INT32_MAX, 2);
  ExpectUnknown(kSignedInclusive, 32, INT32_MAX, INT32_MAX, 1);
  ExpectCount(kSignedInclusive, 32, INT32_MAX - 1, INT32_MAX - 1, 1, 1);
  ExpectCount(kUnsignedExclusive, 32, UINT32_MAX - 1, UINT32_MAX, 1, 1);
  ExpectUnknown(kUnsignedExclusive, 32, UINT32_MAX - 1, UINT32_MAX, 2);
  ExpectUnknown(kUnsignedInclusive, 32, UINT32_MAX, UINT32_MAX, 1);
  ExpectCount(kSignedExclusive, 64, INT64_MAX - 1, INT64_MAX, 1, 1);
  ExpectUnknown(kSignedExclusive, 64, INT64_MAX - 1, INT64_MAX, 2);
  ExpectUnknown(kSignedInclusive, 64, INT64_MAX, INT64_MAX, 1);
  ExpectCount(kUnsignedExclusive, 64, UINT64_MAX - 1, UINT64_MAX, 1, 1);
  ExpectUnknown(kUnsignedExclusive, 64, UINT64_MAX - 1, UINT64_MAX, 2);
  ExpectUnknown(kUnsignedInclusive, 64, UINT64_MAX, UINT64_MAX, 1);
}

TEST(LoopDomainTripCountTest, FullCarrierSpans) {
  ExpectCount(kSignedExclusive, 64, INT64_MIN, INT64_MAX, 1, UINT64_MAX);
  ExpectCount(kSignedExclusive, 64, INT64_MIN, 0, UINT64_C(1) << 63, 1);
  ExpectCount(kUnsignedExclusive, 64, 0, UINT64_MAX, 1, UINT64_MAX);
  ExpectUnknown(kSignedInclusive, 64, INT64_MIN, INT64_MAX, 1);
  ExpectUnknown(kUnsignedInclusive, 64, 0, UINT64_MAX, 1);
  ExpectUnknown(kUnsignedExclusive, 64, 0, UINT64_MAX, 2);
}

TEST(LoopDomainTripCountTest, EmptyLoopsAndZeroIncrements) {
  ExpectCount(kSignedExclusive, 32, 4, 4, 0, 0);
  ExpectCount(kUnsignedInclusive, 32, 5, 4, -1, 0);
  ExpectUnknown(kSignedExclusive, 32, 3, 4, 0);
  ExpectUnknown(kSignedInclusive, 32, 4, 4, 0);
  ExpectUnknown(kUnsignedExclusive, 32, 0, 4, UINT64_C(1) << 32);
}

TEST(LoopDomainTripCountTest, TruncatesAllInputsToSelectedCarrier) {
  ExpectCount(kSignedExclusive, 32, (UINT64_C(1) << 32) + 3,
              (UINT64_C(1) << 33) + 9, (UINT64_C(1) << 34) + 2, 3);
  ExpectCount(kUnsignedExclusive, 32, UINT64_MAX, 4, 1, 0);
  ExpectCount(kSignedExclusive, 32, UINT32_MAX, 4, 1, 5);
}

struct ObservedLoop {
  // Whether modular execution reaches a false guard before revisiting a value.
  bool terminates;
  // Whether every executed addition advances in the bound's comparison order.
  bool increases;
  // Number of body executions before exit or recurrence.
  uint64_t trip_count;
};

// Small-width interpretation is independent of the closed-form count proof.
// Execute carrier additions and comparisons, stopping when a value repeats.
ObservedLoop InterpretLoop(loom_loop_bound_flags_t bound_flags,
                           uint8_t bitwidth, uint64_t initial, uint64_t bound,
                           uint64_t step) {
  const int64_t modulus = INT64_C(1) << bitwidth;
  const bool is_signed =
      bound_flags == kSignedExclusive || bound_flags == kSignedInclusive;
  auto ordered_value = [&](uint64_t bits) -> int64_t {
    return is_signed && bits >= uint64_t(modulus / 2) ? int64_t(bits) - modulus
                                                      : int64_t(bits);
  };
  ObservedLoop observed = {true, true, 0};
  uint64_t value = initial;
  do {
    bool more =
        bound_flags == kSignedInclusive || bound_flags == kUnsignedInclusive
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

TEST(LoopDomainTripCountTest, ExhaustiveModularExecution) {
  for (uint8_t bitwidth = 1; bitwidth <= 6; ++bitwidth) {
    const uint64_t limit = UINT64_C(1) << bitwidth;
    for (loom_loop_bound_flags_t bound_flags :
         {kSignedExclusive, kSignedInclusive, kUnsignedExclusive,
          kUnsignedInclusive}) {
      for (uint64_t initial = 0; initial < limit; ++initial) {
        for (uint64_t bound = 0; bound < limit; ++bound) {
          for (uint64_t step = 0; step < limit; ++step) {
            const ObservedLoop expected =
                InterpretLoop(bound_flags, bitwidth, initial, bound, step);
            uint64_t actual = UINT64_MAX;
            const bool exact = loom_loop_domain_trip_count(
                bound_flags, bitwidth, initial, bound, step, &actual);
            const bool expected_exact =
                expected.terminates && expected.increases;
            if (exact != expected_exact ||
                actual != (expected_exact ? expected.trip_count : 0)) {
              FAIL() << "width=" << int(bitwidth)
                     << " bound_flags=" << int(bound_flags)
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
