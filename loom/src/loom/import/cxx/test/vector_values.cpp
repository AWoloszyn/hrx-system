// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

typedef unsigned U4 __attribute__((vector_size(16)));
typedef int I4 __attribute__((vector_size(16)));
typedef float F4 __attribute__((vector_size(16)));
typedef unsigned char B16 __attribute__((vector_size(16)));
typedef signed char C4 __attribute__((vector_size(4)));
typedef short S4 __attribute__((vector_size(8)));
typedef unsigned short H8 __attribute__((vector_size(16)));
typedef unsigned E4 __attribute__((ext_vector_type(4)));

unsigned vector_unsigned(unsigned input, unsigned lane) {
  U4 value = {input, 0x80000000u, 0xffffffffu, 7u};
  U4 result = ((value + 17u) >> 3u) / 3u;
  return result[lane];
}

int vector_mask(unsigned left, unsigned right, unsigned lane) {
  U4 a = {left, 0xffffffffu, 0u, 0x80000000u};
  U4 b = {right, 0u, 0u, 0x7fffffffu};
  I4 mask = a > b;
  return mask[lane];
}

unsigned vector_narrow(unsigned input, unsigned lane) {
  B16 value = {(unsigned char)input, 255u, 128u, 0u};
  B16 result = (value + 200u) >> 1u;
  return result[lane];
}

template <typename Vector, typename Element>
static int narrow_signed(int input, int divisor, unsigned lane) {
  Vector a = {(Element)input, -127, -1, 127};
  Vector b = {(Element)divisor, 3, -3, 3};
  Vector result = (a / b) >> 1;
  result += a % b;
  return result[lane];
}

int vector_signed_byte(int input, int divisor, unsigned lane) {
  return narrow_signed<C4, signed char>(input, divisor, lane);
}

int vector_signed_short(int input, int divisor, unsigned lane) {
  return narrow_signed<S4, short>(input, divisor, lane);
}

unsigned vector_unsigned_short(unsigned input, unsigned lane) {
  H8 value = {(unsigned short)input, 65535u, 32768u, 0u};
  H8 result = (value * 257u + 60000u) >> 5u;
  return result[lane];
}

unsigned vector_bitcast(unsigned first, unsigned second, unsigned lane) {
  U4 bits = {first, second, 0x3f800000u, 0x80000000u};
  F4 floats = __builtin_bit_cast(F4, bits);
  U4 result = (U4)(-floats);
  return result[lane];
}

int vector_float_ne(unsigned left, unsigned right, unsigned lane) {
  U4 a = {left, 0x7fc00000u, 0u, 0x80000000u};
  U4 b = {right, 0x3f800000u, 0x80000000u, 0u};
  F4 x = (F4)a;
  F4 y = (F4)b;
  I4 result = x != y;
  return result[lane];
}

static U4 advance(U4 value, unsigned count) {
  while (count > 0u) {
    value += 3u;
    --count;
  }
  return value;
}

unsigned vector_control(unsigned input, unsigned count, unsigned lane) {
  U4 value = {input, 1u};
  U4 result = count ? advance(value, count) : value;
  for (unsigned i = 0; i < count; ++i) {
    result += 1u;
  }
  if (count > 2u) {
    result = ~result;
  }
  return result[lane];
}

unsigned vector_ext_splat(unsigned input, unsigned lane) {
  E4 value = input;
  return value[lane];
}

[[loom::kernel, loom::workgroup_size(1, 1, 1), loom::workgroup_count(1, 1, 1)]]
void vector_control_kernel(U4* output, unsigned input, unsigned count) {
  U4 value = {input, 1u};
  U4 result = count ? advance(value, count) : value;
  for (unsigned i = 0; i < count; ++i) {
    result += 1u;
  }
  if (count > 2u) {
    result = ~result;
  }
  output[0] = result;
}

[[loom::kernel, loom::workgroup_size(1, 1, 1), loom::workgroup_count(1, 1, 1)]]
void vector_masks(const U4* input, I4* output) {
  U4 a = input[0];
  U4 b = input[1];
  output[0] = a < b;
  output[1] = a == b;
  output[2] = !a;
  I4 signed_a = (I4)a;
  I4 signed_b = (I4)b;
  output[3] = signed_a < signed_b;
  F4 float_a = (F4)a;
  F4 float_b = (F4)b;
  output[4] = float_a != float_b;
}
