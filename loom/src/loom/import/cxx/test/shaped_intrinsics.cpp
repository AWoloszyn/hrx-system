// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

typedef unsigned U4 __attribute__((vector_size(16)));
typedef unsigned U16 __attribute__((vector_size(64)));
typedef int I4 __attribute__((vector_size(16)));
typedef signed char S16 __attribute__((vector_size(16)));
typedef unsigned char B16 __attribute__((vector_size(16)));
typedef float F4 __attribute__((vector_size(16)));
typedef float F16 __attribute__((vector_size(64)));

[[loom::op("vector.table.lookup")]] U4 lookup(U16 table, U4 indices);
[[loom::op("vector.table.lookup")]] F4 lookup(F16 table, U4 indices);
[[loom::op("vector.dot4i", "s8s8")]] I4 dot(S16 lhs, S16 rhs, I4 acc);
[[loom::op("vector.dot4i", "u8s8")]] I4 dot(B16 lhs, S16 rhs, I4 acc);
[[loom::op("vector.dot4i", "s8u8")]] I4 dot(S16 lhs, B16 rhs, I4 acc);
[[loom::op("vector.dot4i", "u8u8")]] I4 dot(B16 lhs, B16 rhs, I4 acc);

unsigned lookup_lane(unsigned input, unsigned index, unsigned lane) {
  U16 table = {input, 1u, 0xffffffffu, 0x80000000u, 4u,  5u,  6u,  7u,
               8u,    9u, 10u,         11u,         12u, 13u, 14u, 15u};
  U4 indices = {index, 15u - index, 0u, 15u};
  U4 values = lookup(table, indices);
  return values[lane];
}

int dot_lane(unsigned input, int accumulator, unsigned lane) {
  B16 lhs = {(unsigned char)input,
             255u,
             128u,
             127u,
             0u,
             1u,
             255u,
             128u,
             23u,
             45u,
             67u,
             89u,
             255u,
             255u,
             255u,
             255u};
  S16 rhs = {-128, 127, -1, 1,  127, -128, 1,   -1,
             -3,   5,   -7, 11, 127, 127,  127, 127};
  I4 acc = {accumulator, -1, 2147483647, (-2147483647 - 1)};
  I4 values = dot(lhs, rhs, acc);
  return values[lane];
}

[[loom::kernel, loom::workgroup_size(1, 1, 1), loom::workgroup_count(1, 1, 1)]]
void register_lookup(const U16* table, const U4* indices, U4* output,
                     unsigned count) {
  for (unsigned i = 0; i < count; ++i) {
    output[i] = lookup(table[0], indices[i]);
  }
}

[[loom::kernel, loom::workgroup_size(1, 1, 1), loom::workgroup_count(1, 1, 1)]]
void register_lookup_float(const F16* table, const U4* indices, F4* output,
                           unsigned count) {
  for (unsigned i = 0; i < count; ++i) {
    output[i] = lookup(table[0], indices[i]);
  }
}

// Each input group computes four useful four-product sums. Signedness changes
// interpretation of the same bytes; every result adds its original accumulator.
[[loom::kernel, loom::workgroup_size(1, 1, 1), loom::workgroup_count(1, 1, 1)]]
void mixed_dot(const S16* lhs, const S16* rhs, const I4* acc, I4* output,
               unsigned count) {
  for (unsigned i = 0; i < count; ++i) {
    S16 a = lhs[i];
    S16 b = rhs[i];
    I4 initial = acc[i];
    output[4u * i] = dot(a, b, initial);
    output[4u * i + 1u] = dot((B16)a, b, initial);
    output[4u * i + 2u] = dot(a, (B16)b, initial);
    output[4u * i + 3u] = dot((B16)a, (B16)b, initial);
  }
}
