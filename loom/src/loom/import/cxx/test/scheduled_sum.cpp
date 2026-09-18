// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <hip/hip_runtime.h>

#ifndef UNROLL
#define UNROLL 1
#endif
#ifndef DEPTH
#define DEPTH 1
#endif
template <unsigned Factor, unsigned Depth>
__device__ __forceinline__ int sum(const int* input, unsigned row,
                                   unsigned columns) {
  int total = 0;
  [[loom::unroll(Factor), loom::pipeline(Depth), loom::schedule("linear")]]
  for (unsigned column = 0; column < columns; ++column) {
    total += input[row * columns + column];
  }
  return total;
}
__global__ [[loom::workgroup_size(64, 1, 1), loom::workgroup_count(1, 1, 1)]]
void scheduled_sum(const int* input, int* output, unsigned rows,
                   unsigned columns) {
  unsigned row = threadIdx.x;
  if (row < rows) {
    output[row] = sum<UNROLL, DEPTH>(input, row, columns);
  }
}
