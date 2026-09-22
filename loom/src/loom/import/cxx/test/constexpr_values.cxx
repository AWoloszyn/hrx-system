// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <loomcxx/check.h>

template <bool Selected>
static unsigned selected_return(unsigned value) {
  if constexpr (++value; Selected) {
    if (value < 10) {
      return value + 20;
    }
  } else {
    return value + 30;
  }
  return value + 40;
}

static unsigned discarded_statement(unsigned value) {
  if constexpr (++value; false) {
    switch (value) {
      case 1:
        return 100;
      default:
        return 200;
    }
  }
  return value;
}

static unsigned runtime_initializer(unsigned value, bool selected) {
  if (unsigned original = value++; selected) {
    return value + original;
  }
  return value;
}

template <bool Filter>
static unsigned selected_continue(unsigned count) {
  unsigned total = 0;
  unsigned visits = 0;
  for (unsigned index = 0; index < count; ++index) {
    if constexpr (++visits; Filter) {
      if (unsigned next = index + 1; next & 1u) {
        continue;
      }
    } else {
      if constexpr (false) {
        --count;
        return 100;
      }
    }
    total += index;
  }
  return total + 100 * visits;
}

static unsigned selected_continue_tail(unsigned count) {
  unsigned total = 0;
  unsigned visits = 0;
  for (unsigned index = 0; index < count; ++index) {
    if (unsigned previous = visits++; index & 1u) {
      total += previous;
    } else if constexpr (true) {
      continue;
    }
    total += 10;
  }
  return total + 100 * visits;
}

static unsigned runtime_initializer_join(unsigned value, bool selected) {
  if (unsigned original = value++; selected) {
    value += original;
  } else {
    value += 10;
  }
  return value;
}

LOOM_CHECK_CASE(initializers_and_returns) {
  const auto selected_returned = selected_return<true>(2);
  const auto selected_continued = selected_return<true>(12);
  const auto else_returned = selected_return<false>(2);
  const auto discarded = discarded_statement(2);
  const auto initialized_then = runtime_initializer(10, true);
  const auto initialized_else = runtime_initializer(10, false);
  const auto joined_then = runtime_initializer_join(10, true);
  const auto joined_else = runtime_initializer_join(12, false);
  loom::check::expect_equal(selected_returned, 23u);
  loom::check::expect_equal(selected_continued, 53u);
  loom::check::expect_equal(else_returned, 33u);
  loom::check::expect_equal(discarded, 3u);
  loom::check::expect_equal(initialized_then, 21u);
  loom::check::expect_equal(initialized_else, 11u);
  loom::check::expect_equal(joined_then, 21u);
  loom::check::expect_equal(joined_else, 23u);
}

LOOM_CHECK_CASE(loop_selection) {
  const auto empty_filtered = selected_continue<true>(0);
  const auto empty_unfiltered = selected_continue<false>(0);
  const auto filtered = selected_continue<true>(8);
  const auto unfiltered = selected_continue<false>(8);
  const auto empty_tail = selected_continue_tail(0);
  const auto tail = selected_continue_tail(8);
  loom::check::expect_equal(empty_filtered, 0u);
  loom::check::expect_equal(empty_unfiltered, 0u);
  // Eight initializers run. Filtering keeps source indices 1, 3, 5 and 7.
  loom::check::expect_equal(filtered, 816u);
  loom::check::expect_equal(unfiltered, 828u);
  loom::check::expect_equal(empty_tail, 0u);
  loom::check::expect_equal(tail, 856u);
}
