# Tune loop unrolling and pipelining

A streaming reduction often loads the next record from memory while it still
has arithmetic to perform on the current record. Loom lets the author express
that opportunity on the original `scf.for`: `pipeline(%depth)` requests ordinary
read-ahead, and `unroll(%factor)` groups iterations. Both values can come from
the caller of a reusable motif, or from arithmetic on specialized arguments and
target properties. Each instantiation can choose its own schedule.

This walkthrough starts with a reusable vector row sum, then uses a config-driven
experiment harness to compare schedules without editing the source. Global
schedule configuration is convenient for these sweeps; a production motif
library takes per-instantiation choices so its callers remain independent.
The same experiment workflow extends to packed signed-byte dequantization and
dot accumulation. It assumes [Loom tools on `PATH`](../getting-started/acquiring-loom.md).
Compilation and report inspection need no GPU. Execution examples use an AMDGPU
device compatible with `gfx11-generic`; use one compatible target consistently
when adapting the commands to another device.

## Choose the control that describes the experiment

| Intent | Loop policy |
| --- | --- |
| Expand a small loop whose trip count is known at compilation | `unroll` |
| Group a fixed number of iterations while retaining runtime bounds and tails | `unroll(%factor)` |
| Read future inputs ahead of an ordered recurrence | `pipeline(%depth)` |
| Combine read-ahead with grouped iterations | `pipeline(%depth) unroll(%factor)` |
| Group independent work across unrolled copies before advancing a recurrence | Add `schedule(recurrence)` after `unroll(...)` |

Pipelining happens first. Depth three retains two original iterations of input;
unroll factor two then groups two advances of that queue. The sum still visits
records in source order. The [control-flow guide](../guide/functions-and-control.md#pipeline-reads-ahead-of-ordered-computation)
owns the complete policy contract, including full unrolling and scheduling
choices.

Ordinary read-ahead fits flat bodies with loads and pure computation whose load
addresses depend on the induction variable and outer values. A load address
that depends on the previous accumulator cannot be issued ahead. Stores,
explicit async groups, nested control flow, and ordered effects need a different
ownership contract and receive diagnostics at depth greater than one. These
policies are explicit; an unannotated loop receives no read-ahead transform.

## Give each motif its own schedule

This motif sums four adjacent values per row for each work-item. Its template
signature takes `%depth` and `%factor` alongside the data. The kernel caller
chooses depth four and factor six; another application of the same motif can
pass different values in the same compilation.

Save [`vector-read-ahead.loom`](../generated/examples/guide/functions-and-control/vector-read-ahead.loom)
and [`vector-read-ahead-tests.loom`](../generated/examples/guide/functions-and-control/vector-read-ahead-tests.loom):

```loom title="vector-read-ahead.loom"
--8<-- "examples/guide/functions-and-control/vector-read-ahead.loom"
```

The policies must specialize to positive exact values before transformation.
A caller can calculate them with `index` arithmetic from compile-time arguments
or target facts such as `target.subgroup.size`. A configuration value describing
a universal target property can also feed that calculation. A global
algorithm-specific depth would force every instance of the motif to share it.
The constants above are this caller's explicit choice, not a target-wide rule.

The checks exercise empty, short, steady, and remainder paths. A composed
kernel in the test file applies this motif at depth four/factor six and at depth
one/factor two in the same entry, verifying that both choices coexist.

```shell
loom-link vector-read-ahead.loom vector-read-ahead-tests.loom \
  --mode=merge --to=bc --output=vector-read-ahead.loombc
iree-test-loom vector-read-ahead.loombc --device=amdgpu \
  --target=amdgpu:gfx11-generic --sanitizer=access
loom-compile vector-read-ahead.loombc --root=@sum_vector_rows \
  --target=amdgpu:gfx11-generic --format=amdgpu-hsaco \
  --output=vector-rows.hsaco --compile-report=details \
  --compile-report-output=vector-rows.report.json
loom-compile-report show vector-rows.report.json
loom-compile-report suggest vector-rows.report.json
```

The applied policy is visible even though it came through a template argument.
This finding is generated from the example during the documentation build:

```text
--8<-- "generated/examples/guide/functions-and-control/vector-pipeline-suggest.txt"
```

## Keep one checked source for experiments

Each of 32 work-items sums one column of up to 64 input rows. Save
[`read-ahead.loom`](../generated/examples/guide/functions-and-control/read-ahead.loom)
and its sibling
[`read-ahead-tests.loom`](../generated/examples/guide/functions-and-control/read-ahead-tests.loom)
in the same directory:

```loom title="read-ahead.loom"
--8<-- "examples/guide/functions-and-control/read-ahead.loom"
```

This experiment harness defaults to depth three and unroll factor four with
`schedule(recurrence)`. `config.def` provides those defaults; `--config` overrides
them for a particular compilation. These global controls make iterative sweeps
convenient; they are not the recommended interface for a production motif. The
loop body continues to describe one load and one addition.

The checks use `input[row, lane] = 1 + 32 * row + lane`. For `N` rows the exact
answer is `N * (lane + 1) + 16 * N * (N - 1)`. Distinct rows expose skipped,
duplicated, or stale queued values. Counts 0, 1, 2, 3, 4, 5, 63, and 64 cover the
empty path, startup boundary, steady body, and remainders. A negative output
sentinel also detects missing stores.

??? example "Complete correctness cases and benchmark"

    ```loom title="read-ahead-tests.loom"
    --8<-- "examples/guide/functions-and-control/read-ahead-tests.loom"
    ```

Format both files and combine them into one reusable checked module:

```shell
loom-format --check read-ahead.loom
loom-format --check read-ahead-tests.loom
loom-link read-ahead.loom read-ahead-tests.loom \
  --mode=merge --to=bc --output=read-ahead.loombc
```

The module retains the source kernel, configuration, checks, and benchmark. Each
tool selects the part it needs; the checks do not enter the native kernel.

## Separate depth from unrolling

These configurations answer different questions:

| Depth | Unroll factor | Experiment |
| --- | --- | --- |
| 1 | 1 | Serial control. |
| 1 | 2 | Unrolling alone. |
| 3 | 1 | Read-ahead without body expansion. |
| 3 | 2 | Read-ahead and unrolling together. |
| 3 | 4 | A larger recurrence tile that can preserve pending loads across its backedge. |

Depth one consumes the read-ahead policy with serial iteration. Factor one
keeps one copy of the body. To compare depths, hold the unroll factor fixed;
to compare unrolling, hold depth fixed. Run every correctness case for each
candidate before accepting a timing result:

```shell
for depth in 1 3; do
  for factor in 1 2 4; do
    iree-test-loom read-ahead.loombc \
      --device=amdgpu --target=amdgpu:gfx11-generic --sanitizer=access \
      --config=read_ahead.depth="$depth" --config=read_ahead.unroll="$factor" \
      >"sum-rows-d${depth}-u${factor}.test.json"
  done
done
```

Empty loops perform no loads. Loops shorter than the requested depth take a
serial path; longer loops get guarded startup, a steady loop, and a drain.
Partial unrolling retains the final iterations even when they do not fill a
whole unrolled body.

## Inspect the schedule and its cost

Compile depth one and depth three with factor four and the same target:

```shell
for depth in 1 3; do
  loom-compile read-ahead.loombc --root=@sum_rows \
    --target=amdgpu:gfx11-generic --format=amdgpu-hsaco \
    --config=read_ahead.depth="$depth" --config=read_ahead.unroll=4 \
    --output="sum-rows-d${depth}.hsaco" --compile-report=details \
    --compile-report-output="sum-rows-d${depth}.report.json"
done
loom-compile-report show sum-rows-d1.report.json
loom-compile-report show sum-rows-d3.report.json
loom-compile-report suggest sum-rows-d3.report.json
```

The depth-three `show` output includes this source schedule, generated from the
example during the documentation build:

```text
--8<-- "generated/examples/guide/functions-and-control/pipeline-schedule.txt"
```

Address calculation and the load belong to the producer with lookahead two.
The addition remains in the ordered consumer. A queue record contains one SSA
value here; a vector-valued record can occupy several physical registers.

The same compilation produces this `suggest` finding:

```text
--8<-- "generated/examples/guide/functions-and-control/pipeline-suggest.txt"
```

The finding establishes that pipelining was used and reports final resource
consumption. A change in registers or runtime requires a matched baseline.
Compare final registers, spills, modeled occupancy, code size, compile time,
and measured runtime together; a larger queue can buy overlap at the cost of
more live values and startup/drain code.

The two configurations intentionally have different report identities.
`diff --force` displays that mismatch and the resource deltas for this explicit
single-entry pair:

```shell
loom-compile-report diff sum-rows-d1.report.json sum-rows-d3.report.json --force
```

The result is labeled observational. The experiment establishes that only depth
changed; the tool does not infer that from two arbitrary reports. The
[report comparison contract](compile-reports.md#diff-one-controlled-change)
explains the identity checks.

## Check that read-ahead survives native code generation

The source schedule records how far values travel between iterations. Hardware
overlap also depends on their final register assignments. A load can remain
pending until its value is read, but a register-to-register queue copy reads
that value too. A full wait before such a copy can finish future loads earlier
than the arithmetic needs them. Increasing depth alone may then leave the
steady loop with the same amount of useful overlap.

`unroll(%factor) schedule(recurrence)` gives allocation a larger repeating body
in which old values can be consumed before their registers receive future
loads. In the checked row sum, depth three with factor four produces a
copy-free steady backedge on gfx1151 and gfx1250. Its waits allow two loads to
remain pending across it. On gfx1151 the repeating body has this shape:

```text
issue two future loads
wait vmcnt(3); consume first carried value
issue another future load into the released register
wait vmcnt(3); consume second carried value
issue another future load into the released register
wait vmcnt(3); consume first load issued in this body
wait vmcnt(2); consume second load issued in this body
branch allowing two loads to remain pending
```

A partial wait allows younger requests to remain outstanding. Gfx1250 expresses
the same thresholds with `s_wait_loadcnt`. Startup and exit waits still complete
the work those paths require. This register pattern is a compiled result for
this example; changing the payload, target, or unroll factor can change it.

The vector motif above exercises the same boundary with four-register payloads.
At depth four/factor six on gfx1151, its steady backedge carries three vector
loads without register copies or a wait at the edge. The following iteration
issues more loads and waits with `vmcnt(4)` before consuming older values.
Coalesced slices keep their positions within the carried vectors, so extracting
lanes does not introduce a new copy consumer. Entry and exit transfers still
have costs; the final report exposes those along with registers and code size.

Detailed AMDGPU reports retain each wait's block, producer, consumer, and
outstanding counts. `suggest` identifies full load waits whose actual consumers
are branch-payload copies. For the packed-dot example below it reports:

```text
--8<-- "generated/examples/guide/functions-and-control/pipeline-copy-waits.txt"
```

The finding associates native evidence with the compiled entry. It does not
assign every branch to a particular source loop: startup, steady-state, and
tail paths all have edges. Inspect the cited blocks in the native artifact and
compare queue moves, full and partial waits, registers, and code size while
varying explicit unrolling at a fixed depth. The
[wait-report queries](compile-report-queries.md) expose the individual rows.
A deeper source queue or fewer wait instructions alone does not establish a
runtime improvement.

## Measure the checked workload

The example's `@sum_rows_64` benchmark selects the 64-row correctness case.
First inspect its plan without executing a device:

```shell
iree-benchmark-loom read-ahead.loombc --benchmark=@sum_rows_64 \
  --config=read_ahead.depth=3 --config=read_ahead.unroll=4 \
  --dry-run --output=sum-rows.plan.json
```

With optimized, uninstrumented tools and a quiet compatible device, measure
each configuration under the same policy:

```shell
for depth in 1 3; do
  iree-benchmark-loom read-ahead.loombc --benchmark=@sum_rows_64 \
    --device=amdgpu --target=amdgpu:gfx11-generic \
    --config=read_ahead.depth="$depth" --config=read_ahead.unroll=4 \
    --measure=dispatch_complete --batch-size=64 \
    --output="sum-rows-d${depth}.benchmark.json"
done
```

`dispatch_complete` measures host submission through device completion and
normalizes the prepared batch per logical operation. This small kernel launches
one workgroup, so it is useful for learning the workflow but is not a throughput
proxy for a large reduction. Repeat comparisons in alternating order and retain
warnings, workload, target, and timing policy with the result. The
[benchmark workflow](benchmark.md) covers controlled timing and interleaved
comparisons.

## Apply the same controls to a packed dot product

The maintained
[`streaming-packed-dot.loom`](../generated/examples/guide/functions-and-control/streaming-packed-dot.loom)
example computes 2,048 independent outputs across up to 128 records. Each record
loads four packed signed bytes, an `f16` scale, and four `f32` activation values.
The consumer unpacks and scales the weights, then advances an ordered dot
accumulation. Sixty-four activation streams are shared across the output rows.

Its experiment entry, `@streaming_packed_s8_dot_read_ahead`, uses global config
for schedule sweeps and defaults to depth four and factor two. A reusable
packed-dot motif would receive those choices from each caller. The relevant
body is:

```loom
--8<-- "generated/examples/guide/functions-and-control/streaming-packed-dot.loom:read-ahead-loop"
```

`%unroll_factor` comes from `@packed_stream.unroll`; the views and bounds are
established outside this excerpt. Three loaded SSA values cross the cut, so
depth four queues nine values, including vector values. This is a more
substantial live-state tradeoff than the row sum.

The file also retains serial and handwritten schedules. Its independently
varying packed fields, scales, and activations compare bitwise against the
serial recurrence across 14 lengths, including zero, short loops, and tails.
After saving the file, run those checks with an author-selected configuration:

```shell
iree-test-loom streaming-packed-dot.loom --device=amdgpu \
  --target=amdgpu:gfx11-generic --sanitizer=access \
  --config=packed_stream.depth=4 --config=packed_stream.unroll=2

loom-compile streaming-packed-dot.loom \
  --root=@streaming_packed_s8_dot_read_ahead \
  --target=amdgpu:gfx11-generic --format=amdgpu-hsaco \
  --config=packed_stream.depth=4 --config=packed_stream.unroll=2 \
  --output=packed-dot.hsaco --compile-report=details \
  --compile-report-output=packed-dot.report.json
loom-compile-report show packed-dot.report.json
loom-compile-report suggest packed-dot.report.json

iree-benchmark-loom streaming-packed-dot.loom \
  --benchmark=@streaming_packed_s8_dot_read_ahead_n128_time \
  --device=amdgpu --target=amdgpu:gfx11-generic \
  --config=packed_stream.depth=4 --config=packed_stream.unroll=2 \
  --measure=dispatch_complete --batch-size=128 \
  --output=packed-dot.benchmark.json
```

Depth one provides the matched serial control without changing that entry or
its benchmark workload. Hold factor two fixed while comparing depth one, two,
and four, then investigate unrolling separately. The default demonstrates the
policy; selecting a winner requires measurements for the intended device,
workload, and data-reuse policy.

## Carry the experiment into a kernel

After a sweep, put the selected policy in the caller or its target-derived
calculation and pass the values into the motif. Keep the logical loop, boundary
cases, and named checked workload. Reports confirm each instantiation's schedule
and expose its resource cost; controlled measurements decide whether that cost
is useful. The [agent development workflow](agent-driven-kernel-development.md)
places this experiment inside a production kernel search.
