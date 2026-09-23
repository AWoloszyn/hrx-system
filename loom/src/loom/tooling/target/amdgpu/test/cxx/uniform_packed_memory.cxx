// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <loomcxx/check.h>
#include <loomcxx/kernel.h>
#include <loomcxx/numeric.h>

#include <stdfloat>

using Bytes8 = unsigned char __attribute__((ext_vector_type(8)));
using Half4 = std::float16_t __attribute__((ext_vector_type(4)));
using BFloat4 = std::bfloat16_t __attribute__((ext_vector_type(4)));
using Float8E4M3x4 =
    loom::type::float8_e4m3fn_t __attribute__((ext_vector_type(4)));
using Float8E5M2x4 =
    loom::type::float8_e5m2_t __attribute__((ext_vector_type(4)));
using Float4 = float __attribute__((ext_vector_type(4)));

[[loom::kernel, loom::workgroup_size(1, 1, 1), loom::workgroup_count(1, 1, 1)]]
void double_bytes([[loom::noalias,
                    loom::assume_aligned(64)]] const Bytes8* input,
                  [[loom::noalias]] Bytes8* output) {
  *output = *input + *input;
}

[[loom::kernel, loom::workgroup_size(1, 1, 1), loom::workgroup_count(1, 1, 1)]]
void mixed_byte_arithmetic([[loom::noalias,
                             loom::assume_aligned(64)]] const Bytes8* input,
                           [[loom::noalias]] const Bytes8* other,
                           [[loom::noalias]] Bytes8* output) {
  // Only input has the alignment proof needed for a scalar-memory load.
  const Bytes8 left = *input;
  const Bytes8 right = *other;
  output[0] = left + right;
  output[1] = right + left;
  output[2] = left - right;
  output[3] = right - left;
  output[4] = left << 3;
  output[5] = left >> 5;
}

[[loom::kernel, loom::workgroup_size(1, 1, 1), loom::workgroup_count(1, 1, 1)]]
void expand_float16(
    [[loom::noalias, loom::assume_aligned(64)]] const Half4* half,
    [[loom::noalias, loom::assume_aligned(64)]] const BFloat4* bfloat,
    [[loom::noalias]] Float4* output) {
  output[0] = __builtin_convertvector(*half, Float4);
  output[1] = __builtin_convertvector(*bfloat, Float4);
}

[[loom::kernel, loom::workgroup_size(1, 1, 1), loom::workgroup_count(1, 1, 1)]]
void expand_float8(
    [[loom::noalias, loom::assume_aligned(64)]] const Float8E4M3x4* e4m3,
    [[loom::noalias, loom::assume_aligned(64)]] const Float8E5M2x4* e5m2,
    [[loom::noalias]] Float4* output) {
  output[0] = __builtin_convertvector(*e4m3, Float4);
  output[1] = __builtin_convertvector(*e5m2, Float4);
}

LOOM_CHECK_CASE(uniform_byte_arithmetic) {
  // Different bytes exercise both words, wrapping and cross-byte carries.
  const auto input =
      loom::check::fill<unsigned long long, 1>(0x55C04001FF807F00ull);
  const auto storage =
      loom::check::fill<unsigned long long, 3>(0xA5A5A5A5A5A5A5A5ull);
  const auto output = loom::check::slice<1>(storage, 1);
  loom::check::launch<double_bytes>(input, output);
  loom::check::expect_bitwise(
      output, loom::check::fill<unsigned long long, 1>(0xAA808002FE00FE00ull));
  loom::check::expect_bitwise(
      input, loom::check::fill<unsigned long long, 1>(0x55C04001FF807F00ull));
  loom::check::expect_bitwise(
      loom::check::slice<1>(storage, 0),
      loom::check::fill<unsigned long long, 1>(0xA5A5A5A5A5A5A5A5ull));
  loom::check::expect_bitwise(
      loom::check::slice<1>(storage, 2),
      loom::check::fill<unsigned long long, 1>(0xA5A5A5A5A5A5A5A5ull));
  loom::check::expect_event("device", "type", "asan_report", "count", 0);
}

LOOM_CHECK_CASE(mixed_byte_arithmetic_banks) {
  const auto input =
      loom::check::fill<unsigned long long, 1>(0x55C04001FF807F00ull);
  const auto other =
      loom::check::fill<unsigned long long, 1>(0xC38112FE017F8001ull);
  const auto storage =
      loom::check::fill<unsigned long long, 8>(0xA5A5A5A5A5A5A5A5ull);
  const auto output = loom::check::slice<6>(storage, 1);
  loom::check::launch<mixed_byte_arithmetic>(input, other, output);
  loom::check::expect_bitwise(
      loom::check::slice<1>(output, 0),
      loom::check::fill<unsigned long long, 1>(0x184152FF00FFFF01ull));
  loom::check::expect_bitwise(
      loom::check::slice<1>(output, 1),
      loom::check::fill<unsigned long long, 1>(0x184152FF00FFFF01ull));
  loom::check::expect_bitwise(
      loom::check::slice<1>(output, 2),
      loom::check::fill<unsigned long long, 1>(0x923F2E03FE01FFFFull));
  loom::check::expect_bitwise(
      loom::check::slice<1>(output, 3),
      loom::check::fill<unsigned long long, 1>(0x6EC1D2FD02FF0101ull));
  loom::check::expect_bitwise(
      loom::check::slice<1>(output, 4),
      loom::check::fill<unsigned long long, 1>(0xA8000008F800F800ull));
  loom::check::expect_bitwise(
      loom::check::slice<1>(output, 5),
      loom::check::fill<unsigned long long, 1>(0x0206020007040300ull));
  loom::check::expect_bitwise(
      input, loom::check::fill<unsigned long long, 1>(0x55C04001FF807F00ull));
  loom::check::expect_bitwise(
      other, loom::check::fill<unsigned long long, 1>(0xC38112FE017F8001ull));
  loom::check::expect_bitwise(
      loom::check::slice<1>(storage, 0),
      loom::check::fill<unsigned long long, 1>(0xA5A5A5A5A5A5A5A5ull));
  loom::check::expect_bitwise(
      loom::check::slice<1>(storage, 7),
      loom::check::fill<unsigned long long, 1>(0xA5A5A5A5A5A5A5A5ull));
  loom::check::expect_event("device", "type", "asan_report", "count", 0);
}

LOOM_CHECK_CASE(uniform_float16_conversion) {
  // Literal payloads encode [1, -2, 0.5, -0.25] in each input format.
  const auto half =
      loom::check::fill<unsigned long long, 1>(0xB4003800C0003C00ull);
  const auto bfloat =
      loom::check::fill<unsigned long long, 1>(0xBE803F00C0003F80ull);
  const auto storage = loom::check::fill<float, 16>(99.0f);
  const auto output = loom::check::slice<8>(storage, 4);
  loom::check::launch<expand_float16>(half, bfloat, output);
  loom::check::expect_bitwise(loom::check::slice<1>(output, 0),
                              loom::check::fill<float, 1>(1.0f));
  loom::check::expect_bitwise(loom::check::slice<1>(output, 1),
                              loom::check::fill<float, 1>(-2.0f));
  loom::check::expect_bitwise(loom::check::slice<1>(output, 2),
                              loom::check::fill<float, 1>(0.5f));
  loom::check::expect_bitwise(loom::check::slice<1>(output, 3),
                              loom::check::fill<float, 1>(-0.25f));
  loom::check::expect_bitwise(loom::check::slice<1>(output, 4),
                              loom::check::fill<float, 1>(1.0f));
  loom::check::expect_bitwise(loom::check::slice<1>(output, 5),
                              loom::check::fill<float, 1>(-2.0f));
  loom::check::expect_bitwise(loom::check::slice<1>(output, 6),
                              loom::check::fill<float, 1>(0.5f));
  loom::check::expect_bitwise(loom::check::slice<1>(output, 7),
                              loom::check::fill<float, 1>(-0.25f));
  loom::check::expect_bitwise(
      half, loom::check::fill<unsigned long long, 1>(0xB4003800C0003C00ull));
  loom::check::expect_bitwise(
      bfloat, loom::check::fill<unsigned long long, 1>(0xBE803F00C0003F80ull));
  loom::check::expect_bitwise(loom::check::slice<4>(storage, 0),
                              loom::check::fill<float, 4>(99.0f));
  loom::check::expect_bitwise(loom::check::slice<4>(storage, 12),
                              loom::check::fill<float, 4>(99.0f));
  loom::check::expect_event("device", "type", "asan_report", "count", 0);
}

LOOM_CHECK_CASE(uniform_float8_conversion) {
  // Literal payloads encode [1, -2, 0.5, -0.25] in each input format.
  const auto e4m3 = loom::check::fill<unsigned, 1>(0xA830C038u);
  const auto e5m2 = loom::check::fill<unsigned, 1>(0xB438C03Cu);
  const auto storage = loom::check::fill<float, 16>(99.0f);
  const auto output = loom::check::slice<8>(storage, 4);
  loom::check::launch<expand_float8>(e4m3, e5m2, output);
  loom::check::expect_bitwise(loom::check::slice<1>(output, 0),
                              loom::check::fill<float, 1>(1.0f));
  loom::check::expect_bitwise(loom::check::slice<1>(output, 1),
                              loom::check::fill<float, 1>(-2.0f));
  loom::check::expect_bitwise(loom::check::slice<1>(output, 2),
                              loom::check::fill<float, 1>(0.5f));
  loom::check::expect_bitwise(loom::check::slice<1>(output, 3),
                              loom::check::fill<float, 1>(-0.25f));
  loom::check::expect_bitwise(loom::check::slice<1>(output, 4),
                              loom::check::fill<float, 1>(1.0f));
  loom::check::expect_bitwise(loom::check::slice<1>(output, 5),
                              loom::check::fill<float, 1>(-2.0f));
  loom::check::expect_bitwise(loom::check::slice<1>(output, 6),
                              loom::check::fill<float, 1>(0.5f));
  loom::check::expect_bitwise(loom::check::slice<1>(output, 7),
                              loom::check::fill<float, 1>(-0.25f));
  loom::check::expect_bitwise(e4m3,
                              loom::check::fill<unsigned, 1>(0xA830C038u));
  loom::check::expect_bitwise(e5m2,
                              loom::check::fill<unsigned, 1>(0xB438C03Cu));
  loom::check::expect_bitwise(loom::check::slice<4>(storage, 0),
                              loom::check::fill<float, 4>(99.0f));
  loom::check::expect_bitwise(loom::check::slice<4>(storage, 12),
                              loom::check::fill<float, 4>(99.0f));
  loom::check::expect_event("device", "type", "asan_report", "count", 0);
}
