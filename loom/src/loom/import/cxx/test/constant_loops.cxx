// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <hip/hip_runtime.h>

#define PIXELS 320u

__forceinline__ unsigned counted_stride(unsigned start) {
  unsigned total = 0;
  [[loom::unroll(3)]]
  for (unsigned index = start; index < (PIXELS / 16u); index += (1u << 2)) {
    total += index;
  }
  return total;
}

__forceinline__ unsigned counted_edge(unsigned start) {
  unsigned total = 0;
  [[loom::unroll(2)]]
  for (unsigned index = start; index < (~0u - 3u); index += (1u << 2)) {
    total += index;
  }
  return total;
}

__forceinline__ unsigned counted_empty(unsigned start) {
  unsigned total = 7;
  [[loom::unroll(2)]]
  for (unsigned index = start; index < (4u - 4u); index += (1u << 2)) {
    total += index;
  }
  return total;
}

__forceinline__ unsigned counted_maximum_step(unsigned start) {
  unsigned total = 0;
  [[loom::unroll(2)]]
  for (unsigned index = start; index < (4u / 4u); index += ~0u) {
    total += index + 7u;
  }
  return total;
}

template <unsigned Width, unsigned Step, unsigned Unroll, unsigned Pipeline>
__forceinline__ unsigned sum(const unsigned* input, unsigned start) {
  constexpr unsigned lanes = 16;
  unsigned total = 0;
  [[loom::unroll(Unroll), loom::pipeline(Pipeline)]]
  for (unsigned index = start; index < (Width / lanes); index += Step / 2u) {
    total += input[index];
  }
  return total;
}

__forceinline__ unsigned sum_wrapped(const unsigned* input, unsigned start) {
  unsigned total = 0;
  [[loom::unroll(3), loom::pipeline(2)]]
  for (unsigned index = start; index < (0xffffffffu + 21u);
       index += (1u << 2)) {
    total += input[index];
  }
  return total;
}

__forceinline__ unsigned sum_sized(const unsigned* input, unsigned start) {
  unsigned total = 0;
  [[loom::unroll(3), loom::pipeline(2)]]
  for (unsigned index = start;
       index < static_cast<unsigned>(sizeof(++start) * 5u); index++) {
    total += input[index];
  }
  return total + start;
}

__forceinline__ unsigned sum_mutable(const unsigned* input, unsigned start) {
  unsigned bound = 20;
  unsigned total = 0;
  for (unsigned index = start; index < --bound; ++index) {
    total += input[index];
  }
  return total + bound;
}

__global__ [[loom::workgroup_size(64, 1, 1), loom::workgroup_count(1, 1, 1)]]
void constant_loops(const unsigned* input, unsigned* output, unsigned start) {
  unsigned lane = threadIdx.x;
  const unsigned* row = input + lane * 20u;
  output[lane * 12u] = sum<PIXELS, 4, 1, 1>(row, start);
  output[lane * 12u + 1u] = sum<PIXELS, 4, 3, 1>(row, start);
  output[lane * 12u + 2u] = sum<PIXELS, 4, 1, 2>(row, 0u);
  output[lane * 12u + 3u] = sum<PIXELS, 4, 3, 2>(row, 0u);
  output[lane * 12u + 4u] = sum_wrapped(row, 0u);
  output[lane * 12u + 5u] = sum_sized(row, 0u);
  output[lane * 12u + 6u] = sum_mutable(row, start);
  output[lane * 12u + 7u] = counted_edge(0xfffffff0u + (lane & 7u));
  output[lane * 12u + 8u] = sum<0, 4, 3, 2>(row, 0u);
  output[lane * 12u + 9u] = sum<16, 4, 3, 2>(row, 0u);
  output[lane * 12u + 10u] = sum<32, 4, 3, 2>(row, 0u);
  output[lane * 12u + 11u] = sum<80, 4, 3, 2>(row, 0u);
}
