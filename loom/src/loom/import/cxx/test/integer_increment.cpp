// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <hip/hip_runtime.h>

// Uniform and lane-varying locals exercise both native register placements.
__global__
    [[loom::workgroup_size(64, 1, 1), loom::workgroup_count(1, 1, 1)]] void
    increment_u8(unsigned char* output, unsigned input) {
  unsigned lane = threadIdx.x;
  unsigned char uniform = (unsigned char)input;
  ++uniform;
  output[lane * 2u] = uniform;
  unsigned char varying = (unsigned char)(input + lane);
  varying++;
  output[lane * 2u + 1u] = varying;
}

__global__
    [[loom::workgroup_size(64, 1, 1), loom::workgroup_count(1, 1, 1)]] void
    increment_u64(unsigned long long* output, unsigned long long input) {
  unsigned lane = threadIdx.x;
  unsigned long long uniform = input;
  ++uniform;
  output[lane * 2u] = uniform;
  unsigned long long varying = input + lane;
  varying++;
  output[lane * 2u + 1u] = varying;
}
