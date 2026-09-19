// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <hip/hip_runtime.h>

// F32, noncausal attention with head dimension 64 and 16-key shared tiles.
// Launch 64 threads per block and ceil(query_count / 2) blocks.
// Query counts are 0..65535 and key counts 1..65535.
// The online recurrence never materializes the query-by-key score matrix.
__global__ [[loom::workgroup_size(64, 1, 1),
             loom::workgroup_count_range(1, 32768, 1, 1, 1, 1)]] void
flash_attention(const float* query, const float* key, const float* value,
                float* output, unsigned query_count, unsigned key_count) {
  __builtin_assume(key_count < 65536u);
  __builtin_assume(query_count < 65536u);
  __shared__ float key_tile[1024];
  __shared__ float value_tile[1024];
  unsigned thread = threadIdx.x;
  unsigned lane = thread % 32u;
  unsigned query_row = blockIdx.x * 2u + thread / 32u;
  float query_low = 0.0f;
  float query_high = 0.0f;
  if (query_row < query_count) {
    query_low = query[query_row * 64u + lane];
    query_high = query[query_row * 64u + lane + 32u];
  }
  float running_max = -3.402823466e+38f;
  float normalizer = 0.0f;
  float accumulator_low = 0.0f;
  float accumulator_high = 0.0f;

  unsigned tile_count = key_count / 16u;
  if (0u < key_count % 16u) {
    ++tile_count;
  }
  for (unsigned tile_index = 0u; tile_index < tile_count; ++tile_index) {
    unsigned tile_base = tile_index * 16u;
    for (unsigned element = thread; element < 1024u; element += 64u) {
      unsigned key_row = tile_base + element / 64u;
      float key_element = 0.0f;
      float value_element = 0.0f;
      if (key_row < key_count) {
        key_element = key[key_row * 64u + element % 64u];
        value_element = value[key_row * 64u + element % 64u];
      }
      key_tile[element] = key_element;
      value_tile[element] = value_element;
    }
    __syncthreads();

    for (unsigned tile_row = 0u; tile_row < 16u; ++tile_row) {
      if (tile_base + tile_row < key_count) {
        float partial_score = query_low * key_tile[tile_row * 64u + lane];
        partial_score += query_high * key_tile[tile_row * 64u + lane + 32u];
        // Each 32-lane segment owns one query row and two channels per lane.
        partial_score += __shfl_xor(partial_score, 16, 32);
        partial_score += __shfl_xor(partial_score, 8, 32);
        partial_score += __shfl_xor(partial_score, 4, 32);
        partial_score += __shfl_xor(partial_score, 2, 32);
        partial_score += __shfl_xor(partial_score, 1, 32);
        float score = partial_score * 0.125f;
        float next_max = fmaxf(running_max, score);
        float rescale = __expf(running_max - next_max);
        float weight = __expf(score - next_max);
        normalizer = normalizer * rescale + weight;
        accumulator_low = accumulator_low * rescale +
                          weight * value_tile[tile_row * 64u + lane];
        accumulator_high = accumulator_high * rescale +
                           weight * value_tile[tile_row * 64u + lane + 32u];
        running_max = next_max;
      }
    }
    __syncthreads();
  }
  if (query_row < query_count) {
    output[query_row * 64u + lane] = accumulator_low / normalizer;
    output[query_row * 64u + lane + 32u] = accumulator_high / normalizer;
  }
}
