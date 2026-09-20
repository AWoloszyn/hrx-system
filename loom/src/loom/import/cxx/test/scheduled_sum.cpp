// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <hip/hip_runtime.h>

__device__ __forceinline__ int sum(const int* input, unsigned row,
                                   unsigned columns, unsigned factor,
                                   unsigned depth) {
  int total = 0;
  [[loom::unroll(factor), loom::pipeline(depth), loom::schedule("linear")]]
  for (unsigned column = 0; column < columns; ++column) {
    total += input[row * columns + column];
  }
  return total;
}

// Each entry exercises one schedule through the same source expressions.
#define SCHEDULED_SUM(unroll, pipeline_depth)                               \
  __global__                                                                \
      [[loom::workgroup_size(64, 1, 1), loom::workgroup_count(1, 1, 1)]]    \
      void scheduled_sum_unroll_##unroll##_depth_##pipeline_depth(          \
          const int* input, int* output, unsigned rows, unsigned columns) { \
    unsigned row = threadIdx.x;                                             \
    if (row < rows) {                                                       \
      unsigned factor = unroll + 1u;                                        \
      --factor;                                                             \
      unsigned depth = blockDim.x / 64u * pipeline_depth;                   \
      output[row] = sum(input, row, columns, factor, depth);                \
    }                                                                       \
  }

SCHEDULED_SUM(1, 1)
SCHEDULED_SUM(1, 2)
SCHEDULED_SUM(3, 1)
SCHEDULED_SUM(3, 2)
