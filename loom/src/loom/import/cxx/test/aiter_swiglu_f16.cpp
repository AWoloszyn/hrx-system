// Copyright Advanced Micro Devices, Inc. All rights reserved.
//
// Licensed under the MIT License. See LICENSE.aiter for license information.
// SPDX-License-Identifier: MIT
//
// Standalone specialization; source provenance and adaptations are in
// README.md.

#include <hip/hip_runtime.h>

__global__ [[loom::workgroup_size(64, 1, 1),
             loom::workgroup_count_range(1, 32768, 1, 1, 1, 1)]] void
aiter_swiglu_f16(const _Float16* input, _Float16* output, unsigned d) {
  unsigned token_idx = blockIdx.x;
  for (unsigned idx = threadIdx.x; idx < d; idx += 64u) {
    float gate = fminf((float)input[token_idx * 2u * d + idx], 7.0f);
    float linear =
        fminf(fmaxf((float)input[token_idx * 2u * d + d + idx], -7.0f), 7.0f);
    float sig = __builtin_amdgcn_rcpf(1.0f + __ocml_exp_f32(-1.702f * gate));
    output[token_idx * d + idx] = (_Float16)(gate * sig * (linear + 1.0f));
  }
}
