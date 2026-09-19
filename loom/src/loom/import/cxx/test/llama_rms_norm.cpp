// Copyright (c) 2023-2026 The ggml authors
//
// Licensed under the MIT License. See LICENSE.llama for license information.
// SPDX-License-Identifier: MIT
//
// Standalone specialization; source provenance and adaptations are in
// README.md.

#include <hip/hip_runtime.h>

__device__ __forceinline__ float warp_sum(float value) {
  for (unsigned offset = 16u; offset > 0u; offset >>= 1u) {
    __builtin_assume(offset < 32u);
    value += __shfl_xor(value, offset, 32);
  }
  return value;
}
__global__ [[loom::workgroup_size(64, 1, 1),
             loom::workgroup_count_range(1, 32768, 1, 1, 1, 1)]] void
llama_rms_norm(const float* input, float* output, unsigned ncols, float eps) {
  __shared__ float sums[2];
  unsigned row = blockIdx.x;
  unsigned long long row_offset = (unsigned long long)row * ncols;
  const float* row_input = input + row_offset;
  float* row_output = row_offset + output;
  unsigned tid = threadIdx.x;
  unsigned lane = tid % 32u;
  unsigned warp = tid / 32u;
  float total = 0.0f;
  for (unsigned col = tid; col < ncols; col += 64u) {
    float value = row_input[col];
    total += value * value;
  }
  total = warp_sum(total);
  if (lane == 0u) {
    sums[warp] = total;
  }
  __syncthreads();
  total = 0.0f;
  if (lane < 2u) {
    total = sums[lane];
  }
  total = warp_sum(total);
  float scale = rsqrtf(total / (float)ncols + eps);
  for (unsigned col = tid; col < ncols; col += 64u) {
    row_output[col] = scale * row_input[col];
  }
}
