// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <loomcxx/check.h>

#ifndef CHECK_RECORD_EXPECTED
#define CHECK_RECORD_EXPECTED 40u
#endif

struct Empty {};
struct Pair {
  unsigned first, second;
};
struct Payload {
  Pair pair;
  float scale;
  unsigned long long wide;
};
struct Observation {
  Empty tag;
  Payload payload;
  bool valid;
};

static Observation observe(unsigned initial, unsigned count) {
  Observation value{{}, {{initial, 11u}, 0.5f, 0x100000003ULL}, true};
  for (unsigned index = 0; index < count; ++index) {
    value.payload.pair.first += value.payload.pair.second;
    value.valid = !value.valid;
  }
  return value;
}

static Payload update(Payload value) {
  value.pair.second += 2u;
  value.scale *= 4.0f;
  value.wide += 0x200000000ULL;
  return value;
}

static Empty empty() { return {}; }
static Empty identity(Empty value) { return value; }
static unsigned empty_size(Empty value) { return sizeof(value); }

LOOM_CHECK_CASE(record_observations) {
  const auto actual = observe(7u, 3u);
  const auto copy(actual);
  const auto payload{copy.payload};
  const auto pair = payload.pair;
  const auto updated = update(payload);
  const auto tag = identity(actual.tag);
  const auto tag_size = empty_size(tag);
  loom::check::expect_equal(actual.payload.pair.first, CHECK_RECORD_EXPECTED);
  loom::check::expect_equal(pair.second, 11u);
  loom::check::expect_equal(payload.scale, 0.5f);
  loom::check::expect_equal(copy.payload.wide, 0x100000003ULL);
  loom::check::expect_equal(copy.valid, false);
  loom::check::expect_equal(updated.pair.first, 40u);
  loom::check::expect_equal(updated.pair.second, 13u);
  loom::check::expect_equal(updated.scale, 2.0f);
  loom::check::expect_equal(updated.wide, 0x300000003ULL);
  loom::check::expect_equal(tag_size, 1u);
}
LOOM_CHECK_BENCHMARK(record_observations_benchmark, record_observations);

LOOM_CHECK_CASE(temporary_record) {
  loom::check::expect_equal(observe(7u, 0u).payload.pair.first, 7u);
}

LOOM_CHECK_CASE(empty_record) {
  const auto value = empty();
  const auto copy = identity(value);
  const auto size = empty_size(copy);
  loom::check::expect_equal(size, 1u);
}
