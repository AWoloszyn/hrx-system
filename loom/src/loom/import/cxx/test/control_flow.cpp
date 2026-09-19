// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <hip/hip_runtime.h>

__device__ __forceinline__ void store(int* output, unsigned index, int value) {
  output[index] = value;
}

__device__ __forceinline__ bool next_condition(int* output, unsigned index,
                                               unsigned count) {
  output[index] = output[index] + 1;
  return (unsigned)output[index] < count;
}

// Each lane independently exercises zero-trip, post-test and nested loops.
__global__
    [[loom::workgroup_size(64, 1, 1), loom::workgroup_count(1, 1, 1)]] void
    control_flow(const int* counts, int* output, unsigned length) {
  unsigned lane = threadIdx.x;
  if (lane < length) {
    unsigned count = (unsigned)counts[lane];
    unsigned base = lane * 10u;
    unsigned index = 0;
    int total = 0;
    while (index < count) {
      total += (int)index + 1;
      ++index;
    }
    store(output, base, total);
    store(output, base + 1u, (int)index);

    index = 0;
    total = 0;
    do {
      total += (int)index + 1;
      ++index;
    } while (index < count);
    store(output, base + 2u, total);
    store(output, base + 3u, (int)index);

    index = 0;
    total = 0;
    while (index < count) {
      unsigned inner = 0;
      do {
        total += (int)(index + inner + 1u);
        ++inner;
      } while (inner < (count & 3u));
      ++index;
    }
    store(output, base + 4u, total);

    for (index = 0; index < count;) {
      ++index;
    }
    store(output, base + 5u, (int)index);

    store(output, base + 6u, 0);
    total = 0;
    while (next_condition(output, base + 6u, count)) {
      ++total;
    }
    store(output, base + 7u, total);

    store(output, base + 8u, 0);
    total = 0;
    do {
      ++total;
    } while (next_condition(output, base + 8u, count));
    store(output, base + 9u, total);
  }
}
