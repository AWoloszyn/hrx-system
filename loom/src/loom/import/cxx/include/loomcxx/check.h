// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOMCXX_CHECK_H_
#define LOOMCXX_CHECK_H_

// Declares a correctness case with immutable scalar inputs, ordinary function
// calls, and terminal expectations. Unsupported body constructs diagnose during
// import. Ordinary called functions retain the selected target's capabilities.
#define LOOM_CHECK_CASE(name) [[loom::check_case]] void name()

// Measures an existing correctness case after its correctness gate passes.
// Timing policy and iteration counts belong to the benchmark runner.
#define LOOM_CHECK_BENCHMARK(name, case_name) \
  [[loom::check_benchmark(case_name)]] void name()

namespace loom::check {

// Compares two values of the same scalar type. An expectation is a terminal
// observation; subsequent function invocations are not admitted in the case.
[[loom::op("check.expect.equal")]] void expect_equal(bool actual,
                                                     bool expected);
[[loom::op("check.expect.equal")]] void expect_equal(char actual,
                                                     char expected);
[[loom::op("check.expect.equal")]] void expect_equal(signed char actual,
                                                     signed char expected);
[[loom::op("check.expect.equal")]] void expect_equal(unsigned char actual,
                                                     unsigned char expected);
[[loom::op("check.expect.equal")]] void expect_equal(short actual,
                                                     short expected);
[[loom::op("check.expect.equal")]] void expect_equal(unsigned short actual,
                                                     unsigned short expected);
[[loom::op("check.expect.equal")]] void expect_equal(int actual, int expected);
[[loom::op("check.expect.equal")]] void expect_equal(unsigned int actual,
                                                     unsigned int expected);
[[loom::op("check.expect.equal")]] void expect_equal(long actual,
                                                     long expected);
[[loom::op("check.expect.equal")]] void expect_equal(unsigned long actual,
                                                     unsigned long expected);
[[loom::op("check.expect.equal")]] void expect_equal(long long actual,
                                                     long long expected);
[[loom::op("check.expect.equal")]] void expect_equal(
    unsigned long long actual, unsigned long long expected);
[[loom::op("check.expect.equal")]] void expect_equal(_Float16 actual,
                                                     _Float16 expected);
[[loom::op("check.expect.equal")]] void expect_equal(float actual,
                                                     float expected);
[[loom::op("check.expect.equal")]] void expect_equal(double actual,
                                                     double expected);

}  // namespace loom::check

#endif  // LOOMCXX_CHECK_H_
