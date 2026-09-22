// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <hip/hip_runtime.h>

__device__ __forceinline__ const int* advance(const int* pointer,
                                              long long count) {
  if (count == 0) {
    return pointer;
  }
  return +(pointer + count);
}

__global__ [[loom::workgroup_size(64, 1, 1), loom::workgroup_count(1, 1, 1)]]
void pointer_walk(const int* input, const int* counts, int* output,
                  unsigned length, long long start, long long displacement) {
  unsigned lane = threadIdx.x;
  if (lane < length) {
    unsigned count = (unsigned)counts[lane];
    const int* base = advance(input, start + (long long)lane * 64);
    const int* selected = (lane & 1u) ? base + 2 : 4 + base;
    int* result = output + (unsigned long long)lane * 10;
    result[0] = selected[-1];
    result[1] = *advance(selected, displacement);
    result[2] = *(&2 [base]);
    const int* cursor = base;
    int total = 0;
    for (unsigned index = 0; index < count; ++index) {
      total += *advance(base, index);
      ++cursor;
    }
    result[3] = total;
    result[4] = *cursor;
    unsigned index = 0;
    total = 0;
    while (index < count) {
      --cursor;
      total += *cursor;
      ++index;
    }
    result[5] = total;
    result[6] = *cursor;
    index = 0;
    do {
      cursor += 1;
      ++index;
    } while (index < count);
    result[7] = *cursor;
    cursor -= 1;
    result[8] = *cursor;
    (*(result + 9)) = *(cursor - 1);
  }
}
