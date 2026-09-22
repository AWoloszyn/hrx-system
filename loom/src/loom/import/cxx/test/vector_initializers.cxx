// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

typedef unsigned U4 __attribute__((vector_size(16)));
typedef int I4 __attribute__((vector_size(16)));
typedef float F4 __attribute__((vector_size(16)));
typedef unsigned char B16 __attribute__((vector_size(16)));
typedef unsigned E4 __attribute__((ext_vector_type(4)));

[[loom::force_inline]] static I4 offsets(int input) {
  return I4{input, input + 1, input + 2, input + 3};
}

[[loom::force_inline]] static U4 add(U4 first, U4 second) {
  return first + second;
}

template <typename Vector>
[[loom::force_inline]] static Vector first(unsigned input) {
  return Vector{input};
}

int constructor_return(int input, unsigned lane) {
  return offsets(input)[lane];
}

unsigned constructor_argument(unsigned input, unsigned lane) {
  return add(U4{input, 7u}, U4{})[lane];
}

unsigned constructor_single(unsigned input, unsigned lane) {
  return first<E4>(input)[lane];
}

unsigned constructor_narrow(unsigned input, unsigned lane) {
  return B16{(unsigned char)input, 255u, 128u}[lane];
}

unsigned constructor_float(int input, unsigned lane) {
  return __builtin_bit_cast(U4, F4{float(input), -0.0f, 1.5f})[lane];
}

unsigned constructor_nested(unsigned input, unsigned lane) {
  return (U4{input, 1u, 2u, 3u} + U4{9u, 8u, 7u, 6u})[lane];
}

[[loom::force_inline]] static unsigned record(unsigned* trace, unsigned digit,
                                              unsigned value) {
  trace[0] = trace[0] * 10u + digit;
  return value;
}

[[loom::kernel, loom::workgroup_size(1, 1, 1), loom::workgroup_count(1, 1, 1)]]
void vector_initializers(U4* output, unsigned input) {
  output[0] = (U4)offsets((int)input);
  output[1] = add(U4{input, 7u}, U4{});
  output[2] = (U4)first<E4>(input);
  output[3] = __builtin_bit_cast(U4, B16{(unsigned char)input, 255u, 128u});
  output[4] = __builtin_bit_cast(U4, F4{float((int)input), -0.0f, 1.5f});
  output[5] = U4{input, 1u, 2u, 3u} + U4{9u, 8u, 7u, 6u};
  output[7] = U4{};
  unsigned* trace = reinterpret_cast<unsigned*>(output + 7);
  output[6] = U4{record(trace, 1, input), record(trace, 2, input + 1u),
                 record(trace, 3, input + 2u), record(trace, 4, input + 3u)};
}
