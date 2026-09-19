// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <hip/hip_runtime.h>

__device__ __forceinline__ int first(const int* input, unsigned index,
                                     unsigned length) {
  if (index >= length) {
    return -7;
  }
  return input[index];
}

__device__ __forceinline__ int classify(const int* input, unsigned index,
                                        unsigned length, int* trace,
                                        unsigned position) {
  if (index >= length) {
    trace[position] = 1;
    return -7;
  }
  int value = input[index];
  trace[position] = 2;
  if (value < 0) {
    return -value;
  }
  if (value == 0) {
    trace[position] = 3;
    return 3;
  }
  return value + 4;
}

__device__ __forceinline__ int choose(int value, unsigned odd) {
  if (value < 0) {
    if (odd) {
      return -11;
    }
  } else {
    if (odd) {
      return 17;
    }
    return 19;
  }
  return -13;
}

__device__ __forceinline__ void store(int* output, unsigned index, int value) {
  output[index] = value;
}

__device__ __forceinline__ void publish(int* output, unsigned index,
                                        int value) {
  if (value < 0) {
    return store(output, index, 11);
  }
  if (value == 0) {
    return;
  }
  output[index] = value;
}

__device__ __forceinline__ int state(int value) {
  int adjusted = 1;
  if (value < 0) {
    adjusted += value;
    return adjusted;
  } else {
    adjusted += 4;
  }
  {
    if (value == 0) {
      return adjusted;
    }
    adjusted += 2;
  }
  return adjusted + value;
}

// Kernel guards and helper returns preserve per-lane memory effects. The
// guarded helper also inlines into an ordinary structured counted loop.
__global__
    [[loom::workgroup_size(64, 1, 1), loom::workgroup_count(1, 1, 1)]] void
    early_returns(const int* input, int* output, unsigned length) {
  unsigned lane = threadIdx.x;
  if (lane >= length) {
    return;
  }
  unsigned base = lane * 6u;
  output[base] = classify(input, lane + 1u, length, output, base + 1u);
  int total = 0;
  for (unsigned step = 0; step < 3u; ++step) {
    total += first(input, lane + step, length);
  }
  output[base + 2u] = total;
  int value = input[lane];
  output[base + 3u] = choose(value, lane & 1u);
  output[base + 4u] = 0;
  publish(output, base + 4u, value);
  output[base + 5u] = state(value);
}
