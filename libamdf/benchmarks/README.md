# Memory benchmarks

The XDNA memory benchmark exercises the public shared-library API on Linux and
Windows. One device is reused across all cases and repetitions. Discovery,
device activation, profile selection, and full-range host correctness checks
are outside timing.

`XdnaMemory/Lifecycle` measures allocation, host mapping, DMA address lookup,
initialization, explicit cache publication, and complete release.
`XdnaMemory/Publication` measures initialization and cache publication of an
already-resident mapping. Both cover 4 KiB and 1 MiB ranges. Neither includes
command submission or device completion; native computation is covered by CTS.

Build the optimized executable before collecting fixed-count measurements:

```sh
build_tools/bin/iree-bazel-build \
  //libamdf/benchmarks/xdna:memory_benchmark -c opt
bazel-bin/libamdf/benchmarks/xdna/memory_benchmark \
  --benchmark_min_time=64x --benchmark_repetitions=5 \
  --benchmark_out=memory.json --benchmark_out_format=json
```

Google Benchmark reports per-operation elapsed time and byte throughput. The
generated `memory_benchmark_test` performs a single-iteration smoke run through
the same XDNA hardware resource group as CTS. Missing native capabilities are
reported as skips; native operation or byte-check failures terminate the run.

Measurements use nonsanitized optimized binaries, a quiet machine, and the same
compiler options and allocation/publication contract for every comparator.
CPU scaling, concurrent activity, and repetition spread remain part of the
result. Sanitizer and smoke-test output establish correctness, not performance.
The benchmark executable is not part of the installed libamdf distribution.
