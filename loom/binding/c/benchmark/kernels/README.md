# Compiler benchmark smoke kernels

This package contains the small, fixed inputs used to prove that compiler
throughput benchmarks exercise the production compilation path. It is a smoke
suite, not a benchmark corpus or model zoo. Representative model collections,
target tuning studies, and broad performance datasets have different ownership
and scale requirements and do not accumulate here.

The layout groups fixtures by algorithm:

- `attention/` contains fused attention workloads.
- `ffn/` contains fused feed-forward workloads.
- `synthetic/` contains minimal inputs that isolate benchmark-harness behavior.

Each logical benchmark cluster has one exported `iree_c_embed_data` target.
Target-specific implementations of the same algorithm live in that cluster and
use algorithmic names. The target suffix distinguishes implementation details;
model provenance is intentionally absent because it is not part of the measured
contract.

A fixture belongs here only when a registered smoke benchmark consumes it to
exercise a distinct compiler phase, scaling dimension, or target path. New
coverage normally extends or replaces an existing cluster. A collection whose
value comes from its breadth is a corpus and needs a separately designed
package rather than another entry in this one.

The embedded-data targets contain source text only and carry no target-library
dependencies. Target benchmark binaries select the clusters they consume and
remain guarded by their production target capabilities. Disabling AMDGPU or
SPIR-V therefore removes that backend benchmark without coupling the common
runner or the other backend to unavailable target code.

## Pipeline scaling

`synthetic/segmented_read_ahead.loom` feeds the `BM_ScfPipeline` benchmarks in
both target benchmark binaries. Setup uses the public unroll pass to expand a
configurable number of live reduction loops in one kernel. The measured input
therefore grows with `loops`; changing a runtime trip count alone would not
exercise compiler scaling. The configuration values here are benchmark sweep
controls, while the measured loops carry explicit per-loop pipeline policies.

The benchmark families share the same expanded input:

| Family | Timed work |
| --- | --- |
| `Clone` | Clone and release the source module. |
| `Transform` | Clone, run `pipeline-scf-for`, and release. |
| `CompileAndEmit` | Clone, run the target's prepared-low pipeline, emit and validate the native artifact, and release. |
| `CompileAndEmitSummary` / `CompileAndEmitDetails` | The same native path, including compile report generation. |
| `ColdWorkspace` | One native compile-and-emit job in a workspace with no prior compilation. |

Parsing, source expansion, target setup and shape inspection happen before
timing. `ColdWorkspace` describes workspace allocation, not a cold process;
setup qualifies the workload using separate storage. Other families warm the
measured workspace. Every result includes input/output printed operation counts,
IR amplification, primary artifact bytes, and workspace allocation counters.
Depths 1, 2 and 4 separate loop-count scaling from requested pipeline expansion;
depth 1 clears the policy without adding pipeline state.

Native phases select the emitter's control-flow form: CFG for AMDGPU and
structured Low for SPIR-V. Both compile the same source loop policies.

For example, after an optimized build of the AMDGPU benchmark target:

```bash
bazel-bin/loom/binding/c/benchmark/target/amdgpu/compile_throughput_benchmark \
  --benchmark_filter='BM_ScfPipeline/Transform/' \
  --benchmark_min_time=100x --benchmark_repetitions=5 \
  --benchmark_out=pipeline-scaling.json --benchmark_out_format=json
```

Use the SPIR-V target binary for the same comparisons on that backend. Preserve
raw results and build settings, hold machine load steady, and compare geometric
sizes with fixed policy depth. Time per input/output operation and doubling
ratios help distinguish repeated analysis from deliberate IR growth. The
`Smoke` registrations qualify the transform and native/report paths in CI;
wall-clock timing thresholds are kept out of correctness tests.
