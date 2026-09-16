#!/usr/bin/env bash
# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Executes the loop-tuning walkthrough's hardware-independent commands and
# publishes source and actual report excerpts beside the generated page.
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
repo_root="$(cd -- "${script_dir}/../../../../.." && pwd -P)"
output_dir="${1:-${repo_root}/build/loom-docs/examples/guide/functions-and-control}"
mkdir -p -- "${output_dir}"
output_dir="$(cd -- "${output_dir}" && pwd -P)"

"${repo_root}/build_tools/bin/iree-bazel-build" --config=asan \
  //loom/src/loom/tools/loom-format \
  //loom/src/loom/tools/loom-link \
  //loom/src/loom/tools/loom-compile \
  //loom/src/loom/tools/iree-benchmark-loom \
  //loom/py/loom/tools:loom-compile-report
loom_format="${repo_root}/bazel-bin/loom/src/loom/tools/loom-format/loom-format"
loom_link="${repo_root}/bazel-bin/loom/src/loom/tools/loom-link/loom-link"
loom_compile="${repo_root}/bazel-bin/loom/src/loom/tools/loom-compile/loom-compile"
loom_report="${repo_root}/bazel-bin/loom/py/loom/tools/loom-compile-report"
loom_benchmark="${repo_root}/bazel-bin/loom/src/loom/tools/iree-benchmark-loom/iree-benchmark-loom"

cp -- "${script_dir}/read-ahead.loom" "${output_dir}/read-ahead.loom"
cp -- "${script_dir}/read-ahead-tests.loom" "${output_dir}/read-ahead-tests.loom"
cp -- "${repo_root}/loom/src/loom/test/corpus/checked_benchmarks/streaming_packed_s8_dot.loom" \
  "${output_dir}/streaming-packed-dot.loom"

cd -- "${output_dir}"
"${loom_format}" --check read-ahead.loom
"${loom_format}" --check read-ahead-tests.loom
"${loom_link}" read-ahead.loom read-ahead-tests.loom \
  --mode=merge --to=bc --output=read-ahead.loombc

for depth in 1 3; do
  "${loom_compile}" read-ahead.loombc --root=@sum_rows \
    --target=amdgpu:gfx11-generic --format=amdgpu-hsaco \
    --config="read_ahead.depth=${depth}" --config=read_ahead.unroll=2 \
    --output="sum-rows-d${depth}.hsaco" --compile-report=details \
    --compile-report-output="sum-rows-d${depth}.report.json"
  "${loom_report}" show "sum-rows-d${depth}.report.json" \
    >"sum-rows-d${depth}.show.txt"
  "${loom_benchmark}" read-ahead.loombc --benchmark=@sum_rows_64 \
    --config="read_ahead.depth=${depth}" --config=read_ahead.unroll=2 \
    --dry-run --output="sum-rows-d${depth}.plan.json"
done

"${loom_report}" suggest sum-rows-d3.report.json >sum-rows-d3.suggest.txt
"${loom_report}" diff sum-rows-d1.report.json sum-rows-d3.report.json \
  --force >sum-rows.diff.txt
sed -n '/^Source loop pipelines/,$p' sum-rows-d3.show.txt >pipeline-schedule.txt
sed -n '/^\[scf.compare_pipeline_depth\]/,$p' sum-rows-d3.suggest.txt >pipeline-suggest.txt
grep -Fq 'depth=1 queue_records=0' sum-rows-d1.show.txt
grep -Fq 'depth=3 queue_records=2' pipeline-schedule.txt
grep -Fq 'read_ahead.depth' sum-rows.diff.txt
test -s pipeline-suggest.txt

"${loom_compile}" streaming-packed-dot.loom \
  --root=@streaming_packed_s8_dot_read_ahead \
  --target=amdgpu:gfx11-generic --format=amdgpu-hsaco \
  --config=packed_stream.depth=4 --config=packed_stream.unroll=2 \
  --output=packed-dot.hsaco --compile-report=details \
  --compile-report-output=packed-dot.report.json
"${loom_report}" show packed-dot.report.json >packed-dot.show.txt
"${loom_report}" suggest packed-dot.report.json >packed-dot.suggest.txt
"${loom_benchmark}" streaming-packed-dot.loom \
  --benchmark=@streaming_packed_s8_dot_read_ahead_n128_time \
  --config=packed_stream.depth=4 --config=packed_stream.unroll=2 \
  --dry-run --output=packed-dot.plan.json
