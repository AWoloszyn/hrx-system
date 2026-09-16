// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "benchmark/benchmark.h"
#include "iree/testing/benchmark.h"

namespace {

void BM_Success(benchmark::State& state) {
  for (auto _ : state) benchmark::DoNotOptimize(state.iterations());
}
BENCHMARK(BM_Success);

void BM_Error(benchmark::State& state) {
  state.SkipWithError("C++ benchmark failed");
}
BENCHMARK(BM_Error);

void BM_Skip(benchmark::State& state) {
  state.SkipWithMessage("benchmark intentionally skipped");
}
BENCHMARK(BM_Skip);

IREE_BENCHMARK_FN(CError) {
  return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                          "C benchmark failed");
}
IREE_BENCHMARK_REGISTER(CError);

}  // namespace
