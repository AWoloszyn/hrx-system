// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <loomcxx/check.h>

namespace offsets {
constexpr unsigned increment = 7;
static unsigned advance(unsigned value) { return value + increment; }
}  // namespace offsets

template <class T>
static unsigned aliases(unsigned input) {
  using Word = T;
  Word value = input;
  unsigned total = sizeof(Word) * 100 + value;
  {
    typedef unsigned char Word;
    Word narrowed = input;
    total += sizeof(Word) * 10 + narrowed;
  }
  return total + sizeof(Word);
}

static unsigned directive(unsigned input) {
  using namespace offsets;
  unsigned value = input + increment;
  {
    unsigned increment = 2;
    value += increment;
  }
  return value + increment;
}

static unsigned declaration(unsigned input) {
  using offsets::advance;
  using offsets::increment;
  return advance(input) + increment;
}

LOOM_CHECK_CASE(local_name_resolution) {
  const auto narrow = aliases<unsigned short>(0x101u);
  const auto wide = aliases<unsigned>(0x101u);
  const auto directed = directive(10u);
  const auto declared = declaration(10u);
  loom::check::expect_equal(narrow, 470u);
  loom::check::expect_equal(wide, 672u);
  loom::check::expect_equal(directed, 26u);
  loom::check::expect_equal(declared, 24u);
}
