// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOMCXX_MATH_H_
#define LOOMCXX_MATH_H_

#include <loomcxx/kernel.h>
#include <loomcxx/scalar.h>

namespace loom {

// Declares the reciprocal-approximation permission without weakening other
// floating-point contracts. The importer validates this signature against divf.
[[loom::op("scalar.divf", "arcp")]] float divide_reciprocal(float numerator,
                                                            float denominator);

[[loom::device, loom::force_inline]] static inline float reciprocal(
    float value) {
  return divide_reciprocal(1.0f, value);
}

[[loom::device, loom::force_inline]] static inline float infinity() {
  return __builtin_bit_cast(float, 0x7f800000u);
}

}  // namespace loom

#endif  // LOOMCXX_MATH_H_
