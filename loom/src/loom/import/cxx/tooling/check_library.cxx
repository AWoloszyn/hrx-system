// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <loomcxx/check.h>

int library_increment(int input) { return input + 1; }

LOOM_CHECK_CASE(library_case) {
  const auto actual = library_increment(7);
  loom::check::expect_equal(actual, 8);
}
