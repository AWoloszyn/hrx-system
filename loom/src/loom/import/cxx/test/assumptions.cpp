// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <hip/hip_runtime.h>

[[loom::force_inline]] unsigned bound_pair(unsigned first, unsigned second) {
  __builtin_assume(first < 256u && second < 256u);
  return first * 257u + second;
}

[[loom::force_inline]] unsigned bound_seven(unsigned a, unsigned b, unsigned c,
                                            unsigned d, unsigned e, unsigned f,
                                            unsigned g) {
  loom::assume(a < 256u && b < 256u && c < 256u && d < 256u && e < 256u &&
               f < 256u && g < 256u);
  return a + 2u * b + 3u * c + 5u * d + 7u * e + 11u * f + 13u * g;
}

template <unsigned Limit>
[[loom::force_inline]] unsigned refine(unsigned value) {
  loom::assume(((value < 256u) && ((value) < (1u << Limit))) && value < 64u);
  return value * 17u;
}

[[loom::force_inline]] unsigned bound_repeated(unsigned value) {
  return refine<5>(value);
}

[[loom::force_inline]] unsigned bound_capacity(unsigned value) {
  constexpr unsigned capacity = 28672;
  constexpr unsigned stride = 16;
  loom::assume(value <
               ((capacity / sizeof(unsigned) - 16u - 320u) / stride + 1u));
  return value * 16u + 336u;
}

[[loom::force_inline]] unsigned bound_cast(unsigned value) {
  loom::assume(value < static_cast<unsigned char>(272u));
  return value + 5u;
}

[[loom::force_inline]] unsigned bound_byte(unsigned char value) {
  loom::assume(value < 256u);
  return value + (value >= 128u ? 1024u : 0u);
}

[[loom::force_inline]] unsigned bound_wide(unsigned long long value) {
  loom::assume(value < (0xffffffffu + 257u));
  return (unsigned)(value * 3u);
}

[[loom::force_inline]] unsigned bound_size(unsigned value) {
  loom::assume(value < sizeof(++value) * 4u);
  return value;
}

[[loom::force_inline]] unsigned bound_scoped(unsigned value) {
  if (value < 256u) {
    loom::assume(value < 256u);
    return value + 1u;
  }
  return value + 3u;
}

[[loom::kernel, loom::workgroup_size(64, 1, 1), loom::workgroup_count(1, 1, 1)]]
void assumption_kernel(unsigned* output, unsigned input) {
  unsigned lane = threadIdx.x;
  unsigned value = input + lane;
  output[lane * 9u] = bound_pair(value & 255u, (value >> 8u) & 255u);
  output[lane * 9u + 1u] = bound_seven(value & 255u, 1u, 2u, 3u, 5u, 7u, 11u);
  output[lane * 9u + 2u] = bound_repeated(value & 31u);
  output[lane * 9u + 3u] = bound_capacity(value % 428u);
  output[lane * 9u + 4u] = bound_cast(value & 15u);
  output[lane * 9u + 5u] = bound_byte((unsigned char)value);
  output[lane * 9u + 6u] = bound_wide(value & 255u);
  output[lane * 9u + 7u] = bound_size(value & 15u);
  output[lane * 9u + 8u] = bound_scoped(value);
}
