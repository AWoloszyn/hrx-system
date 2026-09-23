// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Main entry point for CTS benchmark binaries.
//
// Link-time composition: benchmark suites and backends register at static init
// time. This main() calls InstantiateAll() to pair suites with backends and
// register benchmarks with Google Benchmark.
//
// Usage:
//   iree_runtime_cc_benchmark(
//       name = "buffer_benchmarks",
//       deps = [
//           ":backends",
//           "//runtime/src/iree/async/cts/buffer:all_benchmarks",
//           "//runtime/src/iree/async/cts/util:benchmark_main",
//           ...
//       ],
//   )

#include "benchmark/benchmark.h"
#include "iree/async/cts/util/registry.h"
#include "iree/testing/benchmark.h"

int main(int argc, char** argv) {
  IREE_TRACE_APP_ENTER();

  // Instantiate benchmark suites for all registered backends.
  // This must happen before benchmark::Initialize() so that benchmarks
  // are registered before the framework parses command-line filters.
  ::iree::async::cts::CtsRegistry::InstantiateAll();

  iree_benchmark_initialize(&argc, argv);
  if (::benchmark::ReportUnrecognizedArguments(argc, argv)) {
    IREE_TRACE_APP_EXIT(1);
    return 1;
  }
  const int exit_code = iree_benchmark_run_specified() ? 0 : 1;
  ::benchmark::Shutdown();
  IREE_TRACE_APP_EXIT(exit_code);
  return exit_code;
}
