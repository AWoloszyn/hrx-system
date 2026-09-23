// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <loomcxx/kernel.h>

using Bytes8 = unsigned char __attribute__((ext_vector_type(8)));
using SignedBytes8 = signed char __attribute__((ext_vector_type(8)));

// Distinct neighboring bytes exercise both physical words at every shift count.
[[loom::kernel, loom::workgroup_size(32, 1, 1), loom::workgroup_count(8, 1, 1)]]
void packed_byte_shifts(const Bytes8* input, Bytes8* output) {
  unsigned index = loom::workgroup_id.x * 32 + loom::workitem_id.x;
  Bytes8 values = input[index];
  [[loom::unroll(8)]]
  for (unsigned amount = 0; amount < 8; ++amount) {
    output[index * 19 + amount * 2] = values << amount;
    output[index * 19 + amount * 2 + 1] = values >> amount;
  }
  // A shared bound does not imply that every byte has the same shift amount.
  Bytes8 amounts = values & 7;
  output[index * 19 + 16] = values << amounts;
  output[index * 19 + 17] = values >> amounts;
  // The left shift can set the sign bit; its facts must preserve this
  // comparison.
  Bytes8 sign_bits = (values & 1) << 7;
  output[index * 19 + 18] = __builtin_convertvector(
      __builtin_convertvector(sign_bits, SignedBytes8) < 0, Bytes8);
}
