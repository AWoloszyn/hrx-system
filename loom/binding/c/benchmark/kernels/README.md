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
remain guarded by their production target capabilities. Disabling AMDGPU,
SPIR-V, or XDNA therefore removes that backend benchmark without coupling the
common runner or the other backends to unavailable target code.

## Comparable FFN compilation

`BM_FfnGateUpQuadraticBF16` compiles the same one-output gate/up operation on
AMDGPU and XDNA at K=512, 1,024, and 4,096. Inputs are one K-element BF16
activation row and two K-element BF16 weight rows, ordered gate then up. Each
projection uses 16 F32 partial sums, accumulating two ordered products per lane
per 32-element chunk, followed by an ordered scalar reduction. The projections
are rounded to BF16 before the epilogue:

```text
gate * (0.5 + 0.25 * gate) * up
```

Each epilogue operation rounds to BF16; the single F32 output exactly represents
the resulting BF16 value. This is the quadratic gate used by the F32 benchmark
family, not SwiGLU. The BF16 family has one output rather than the F32 family's
runtime token/channel grid. Comparisons within a family have matching shapes.

The target sources share their arithmetic body. AMDGPU supplies a one-workitem
kernel; XDNA supplies a streamed worker and array entry. Both retain their loops
and use a 64-byte private buffer for partial-sum reduction. This is a compiler
workload, not a claim that either source is an optimized device implementation.
XDNA emission includes tile code, transport configuration, binding records, and
native invocation commands in a complete `.xdna` image.

`SourceLow`, `PreparedLow`, and `CompileAndEmit` clone and specialize a parsed
module inside the timed region. Target/context setup and parsing occur before
timing; the workspace is warmed and reused. `CompileAndEmit` includes native
emission and artifact release, with reports disabled. The same harness and
operation/size counters apply to both targets.

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

## Unroll scaling

`synthetic/unroll_recurrence.loom` feeds three AMDGPU benchmark families:
`BM_ScfUnrollFenced` and `BM_ScfUnrollFree` vary the number of materialized
iterations in a carried arithmetic recurrence. `BM_ScfUnrollNestedReads` varies
the length of two distinct input rows reduced through nested loops. The outer
scheduled loop first clones two structured units, then their inner linear
loops expand. This provides a nested-control counterpart to the flat recurrence
and distinguishes its IR growth from repeated candidate scanning.

`benchmark.unroll_count` is an experiment control. The source/prepared-Low
sweeps cover 8 through 1,024 copies or reads, and native emission uses a subset
of those sizes. `SourceLowSmoke` runs the smallest source case in CI. All phases
clone the parsed source and apply the exact configuration inside the timed
compilation; target setup and source parsing stay outside timing. The existing
IR-size and workspace counters describe the resulting expansion.
