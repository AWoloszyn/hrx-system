# Net Conformance Workloads

The net CTS exercises application-shaped traffic through real transport
factories, sessions, and channels. The same workload powers correctness tests
and fixed-work benchmarks, so timing a transport also checks its output and
ownership joins. These workloads model communication useful to remoting and
collectives without depending on the HAL or a device driver.

Carrier runners live under `runtime/src/iree/net/carrier/{loopback,tcp,shm}/cts/`.
Each links one `TransportBackend` from
`runtime/src/iree/net/cts/transport_backend.h` with explicit async CTS proactor
configurations. Linux runners include io_uring and its capability-masked variants,
poll, and epoll; Windows includes IOCP and its legacy wait variant; macOS includes
poll and kqueue. An unavailable configuration is reported as a skip, not replaced
with a different stack. Errors after listener creation fail the trial.

## Checked Transfer

`runtime/src/iree/net/cts/transfer_trial.h` declares the shared workload. A producer
and consumer each own a proactor on a separate application thread in the same
process. One or sixteen connections share those two poll owners. Each connection
carries two independent application timelines through one queue channel.

The producer sends patterned records in borrowed scatter/gather COMMAND payloads.
The consumer checks every byte before advancing that record's timeline. In this
workload, ordered consumption establishes a completed prefix for each
timeline. ADVANCE messages report those witnessed coordinates; progress is never
inferred from unrelated queue submission or callback order. The producer merges
coordinates componentwise and admits more records within a per-timeline window.

The `inline` consumer checks bytes inside the receive callback. The
`retained_window` consumer moves timeline 0's original payload/frontier views
and leases out of the callback while timeline 1 continues independently. Once
a complete timeline-0 window arrives and timeline 1's covering progress report
is admitted, the poll owner reads and releases the retained messages. The
producer observes that independent progress before the later timeline-0 report.
Partial windows at warm-up and measurement boundaries release as phase tails.

Retained descriptors are reserved once, bounded by the record window. Payloads
are not copied by the workload. A carrier may use independent framing storage
to preserve receive headroom when native leases are held; the workload neither
requires nor pretends to measure a particular storage strategy.

Two feedback policies expose the tradeoff between prompt reporting and coalescing:

- `immediate` attempts feedback from each receive callback. Admission pressure
  can still merge progress while feedback is pending.
- `poll_turn` merges progress until the consumer is about to poll again. It
  flushes an idle tail without a timer or a fixed coalescing delay.

Logical window return and source retirement are independent. Receiving progress
does not retire a borrowed source; every accepted producer send must also finish
its local callback. Consumer feedback callbacks and both session deactivations
join before the trial releases storage, including on failure. There are no
internal wall-clock deadlines for valid work; the outer test harness catches
hangs.

Work counts, payload pattern, batching limits, SG shape, and window bounds are
fixed. Thread scheduling, completion grouping, actual batch tails, feedback
counts, and timings may vary. CTS assertions cover payload, progress, source
completion, and bounds, not a particular completion schedule.

## Correctness Runs

From the repository root:

```sh
iree-bazel-test --config=asan \
  //runtime/src/iree/net/carrier/loopback/cts:transfer_trial_tests \
  //runtime/src/iree/net/carrier/tcp/cts:transfer_trial_tests \
  //runtime/src/iree/net/carrier/shm/cts:transfer_trial_tests
```

The cases cover single-record windows, partial batches and isolated tails,
borrowed fragments crossing receive-buffer boundaries, and sixteen connections
sharing poll owners under admission pressure. Retained-window cases hold original
views beyond callbacks, span native receive-storage recycling, and verify
independent progress before consuming those views. `--test_arg=--gtest_repeat=20`
repeats the fixed trials. Each carrier also has a `transfer_benchmarks_test`
smoke target that runs every benchmark profile and propagates reported errors.

## Benchmark Runs

Correctness and performance use different builds. For an optimized local run:

```sh
iree-bazel-build //runtime/src/iree/net/carrier/tcp/cts:transfer_benchmarks \
  -c opt --features=thin_lto \
  --copt=-O3 --cxxopt=-O3 --host_copt=-O3 --host_cxxopt=-O3 \
  --copt=-march=native --cxxopt=-march=native \
  --host_copt=-march=native --host_cxxopt=-march=native

bazel-bin/runtime/src/iree/net/carrier/tcp/cts/transfer_benchmarks \
  '--benchmark_filter=^CheckedTransfer/tcp/io_uring/same_process/' \
  --benchmark_repetitions=5 \
  --benchmark_report_aggregates_only=true \
  --benchmark_out=transfer-results.json --benchmark_out_format=json
```

Replace `tcp` with `loopback` or `shm` and select the intended proactor explicitly.
`--benchmark_list_tests=true` lists the platform's profiles. A filter matching no
profiles fails. Build the exact executable immediately before running it; a
build of a sibling target does not refresh an old benchmark binary. Architecture
flags such as `-march=native` describe a local measurement, not a portable binary.

Each row runs one complete trial with 256 warm-up records per timeline followed
by the named fixed measured count. Repetitions repeat that whole trial; benchmark
minimum-duration flags do not silently change the work count. In each row name,
the consumer mode (`inline` or `retained_window`) and feedback policy precede
the numeric dimensions:

| Dimension | Meaning |
| --- | --- |
| `peers` | Connections sharing the producer and consumer proactors. |
| `bytes` | Payload bytes per record, excluding protocol overhead. |
| `batch` | Maximum records of one timeline in a COMMAND. |
| `window` | Maximum submitted but unobserved records per timeline. |
| `sg` | Borrowed fragments dividing each COMMAND's host payload. |
| `records` | Measured records per timeline and connection. |

The matrix compares one and sixteen connections, batch sizes of one and 32,
unit and 128-record windows, and 4-KiB records in four-fragment batches. Both
consumer modes and progress policies use identical work dimensions. Retained
consumption deliberately changes dependency and storage pressure, so its timing
is application workload cost, not isolated lease-move overhead.

Wall time covers measured submission through the producer observing all completed
progress and joining all source callbacks. Process CPU time covers both poll
owners over that same phase. Connection setup, thread startup, payload
initialization, warm-up, final consumer callback drain, and teardown are outside
measurement. Payload validation and progress feedback are inside measurement.

`items_per_second` and `bytes_per_second` normalize all records across both
timelines and all connections. `amortized_ns_per_record` is elapsed time divided
by that record count, not individual request latency. `command_messages`,
`source_completions`, `progress_messages`, and `records_per_progress` expose the
actual batching and feedback costs. `window_high_water` is the largest observed
per-timeline window occupancy.

`independent_progress_messages` counts observations with timeline 1 ahead of
timeline 0. `retained_messages_high_water` and `retained_bytes_high_water` report
the largest held message count and payload footprint on any one connection,
excluding warm-up. They are not simultaneous process-wide memory totals.

`payload_storage_bytes` counts the producer's immutable source and consumer's
expected-byte image, not carrier rings, receive storage, or total process memory.
`proactor_capabilities` records the enabled mask, not proof that every enabled
optimization was used. Compare matching profiles and record hardware, OS/kernel,
build configuration, load, and repetition medians alongside the JSON. Sanitizers,
tracing, concurrent builds, and uncontrolled CPU scheduling change the result.

## Qualification Boundary

These are same-process host-memory trials using raw, unregistered SG sources and
inline or poll-owner-deferred receive consumption. TCP uses the local network
stack; SHM and loopback use their real carriers. This measures checked-transfer
application work, not pure link bandwidth or device execution latency.

Process isolation, independently executing device consumers, registered
opaque/device memory, RDMA placement, and native DMA visibility require trials
crossing those specific ownership boundaries. An enabled zero-copy capability
or a fast host result alone establishes none of them.
