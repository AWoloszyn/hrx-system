// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <hip/hip_runtime.h>

template <int Factor>
__device__ __forceinline__ float transform(float value) {
  return fmaf(value, float(Factor), 1.0f);
}

__global__ [[loom::workgroup_size(64, 1, 1), loom::workgroup_count(2, 1, 1)]]
void affine(const float* input, float* output) {
  unsigned index = blockIdx.x * blockDim.x + threadIdx.x;
  __builtin_assume(index < 128u);
  output[index] = transform<3>(input[index]);
}

__global__ [[loom::workgroup_size(64, 1, 1),
             loom::workgroup_count_range(1, 4, 1, 1, 1, 1)]]
void residual(const float* input, float* output) {
  unsigned index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index < 128u) {
    output[index] = transform<3>(input[index]) + input[index];
  }
}
