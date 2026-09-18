// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// GENERATED FILE: DO NOT EDIT.
// Generator: loom.gen.cxx.intrinsics.
// Regenerate: python3 loom/py/loom/gen/run.py cxx_intrinsics --in-place
// clang-format off
#ifndef LOOMCXX_SCALAR_H_
#define LOOMCXX_SCALAR_H_

// Fixed scalar projections of the canonical Loom operation contracts.
// The approximate namespace explicitly permits AFN and no other flags.
namespace loom::scalar {

// Floating-point absolute value.
[[loom::op("scalar.absf")]] _Float16 absf(_Float16 input);
[[loom::op("scalar.absf")]] float absf(float input);
[[loom::op("scalar.absf")]] double absf(double input);

// Arccosine.
[[loom::op("scalar.acosf")]] _Float16 acosf(_Float16 input);
[[loom::op("scalar.acosf")]] float acosf(float input);
[[loom::op("scalar.acosf")]] double acosf(double input);

// Inverse hyperbolic cosine.
[[loom::op("scalar.acoshf")]] _Float16 acoshf(_Float16 input);
[[loom::op("scalar.acoshf")]] float acoshf(float input);
[[loom::op("scalar.acoshf")]] double acoshf(double input);

// Floating-point addition.
[[loom::op("scalar.addf")]] _Float16 addf(_Float16 lhs, _Float16 rhs);
[[loom::op("scalar.addf")]] float addf(float lhs, float rhs);
[[loom::op("scalar.addf")]] double addf(double lhs, double rhs);

// Arcsine.
[[loom::op("scalar.asinf")]] _Float16 asinf(_Float16 input);
[[loom::op("scalar.asinf")]] float asinf(float input);
[[loom::op("scalar.asinf")]] double asinf(double input);

// Inverse hyperbolic sine.
[[loom::op("scalar.asinhf")]] _Float16 asinhf(_Float16 input);
[[loom::op("scalar.asinhf")]] float asinhf(float input);
[[loom::op("scalar.asinhf")]] double asinhf(double input);

// Two-argument arctangent: atan2(y, x).
[[loom::op("scalar.atan2f")]] _Float16 atan2f(_Float16 lhs, _Float16 rhs);
[[loom::op("scalar.atan2f")]] float atan2f(float lhs, float rhs);
[[loom::op("scalar.atan2f")]] double atan2f(double lhs, double rhs);

// Arctangent.
[[loom::op("scalar.atanf")]] _Float16 atanf(_Float16 input);
[[loom::op("scalar.atanf")]] float atanf(float input);
[[loom::op("scalar.atanf")]] double atanf(double input);

// Inverse hyperbolic tangent.
[[loom::op("scalar.atanhf")]] _Float16 atanhf(_Float16 input);
[[loom::op("scalar.atanhf")]] float atanhf(float input);
[[loom::op("scalar.atanhf")]] double atanhf(double input);

// Cube root.
[[loom::op("scalar.cbrtf")]] _Float16 cbrtf(_Float16 input);
[[loom::op("scalar.cbrtf")]] float cbrtf(float input);
[[loom::op("scalar.cbrtf")]] double cbrtf(double input);

// Round toward positive infinity.
[[loom::op("scalar.ceilf")]] _Float16 ceilf(_Float16 input);
[[loom::op("scalar.ceilf")]] float ceilf(float input);
[[loom::op("scalar.ceilf")]] double ceilf(double input);

// Copy sign of rhs onto magnitude of lhs.
[[loom::op("scalar.copysignf")]] _Float16 copysignf(_Float16 lhs, _Float16 rhs);
[[loom::op("scalar.copysignf")]] float copysignf(float lhs, float rhs);
[[loom::op("scalar.copysignf")]] double copysignf(double lhs, double rhs);

// Cosine.
[[loom::op("scalar.cosf")]] _Float16 cosf(_Float16 input);
[[loom::op("scalar.cosf")]] float cosf(float input);
[[loom::op("scalar.cosf")]] double cosf(double input);

// Hyperbolic cosine.
[[loom::op("scalar.coshf")]] _Float16 coshf(_Float16 input);
[[loom::op("scalar.coshf")]] float coshf(float input);
[[loom::op("scalar.coshf")]] double coshf(double input);

// Cosine over turns: cos(2*pi*x), preserving finite-input periodicity and
// exact quarter-turn cardinals. Non-finite inputs produce NaN.
[[loom::op("scalar.costurnsf")]] _Float16 costurnsf(_Float16 input);
[[loom::op("scalar.costurnsf")]] float costurnsf(float input);
[[loom::op("scalar.costurnsf")]] double costurnsf(double input);

// Floating-point division.
[[loom::op("scalar.divf")]] _Float16 divf(_Float16 lhs, _Float16 rhs);
[[loom::op("scalar.divf")]] float divf(float lhs, float rhs);
[[loom::op("scalar.divf")]] double divf(double lhs, double rhs);

// Complementary error function: 1 - erf(x).
[[loom::op("scalar.erfcf")]] _Float16 erfcf(_Float16 input);
[[loom::op("scalar.erfcf")]] float erfcf(float input);
[[loom::op("scalar.erfcf")]] double erfcf(double input);

// Error function (used in GeLU activation).
[[loom::op("scalar.erff")]] _Float16 erff(_Float16 input);
[[loom::op("scalar.erff")]] float erff(float input);
[[loom::op("scalar.erff")]] double erff(double input);

// Base-2 exponential: 2^x.
[[loom::op("scalar.exp2f")]] _Float16 exp2f(_Float16 input);
[[loom::op("scalar.exp2f")]] float exp2f(float input);
[[loom::op("scalar.exp2f")]] double exp2f(double input);

// Exponential: e^x.
[[loom::op("scalar.expf")]] _Float16 expf(_Float16 input);
[[loom::op("scalar.expf")]] float expf(float input);
[[loom::op("scalar.expf")]] double expf(double input);

// Exponential minus one: e^x - 1 (numerically stable near 0).
[[loom::op("scalar.expm1f")]] _Float16 expm1f(_Float16 input);
[[loom::op("scalar.expm1f")]] float expm1f(float input);
[[loom::op("scalar.expm1f")]] double expm1f(double input);

// Round toward negative infinity.
[[loom::op("scalar.floorf")]] _Float16 floorf(_Float16 input);
[[loom::op("scalar.floorf")]] float floorf(float input);
[[loom::op("scalar.floorf")]] double floorf(double input);

// Fused multiply-add: a*b + c with single rounding.
[[loom::op("scalar.fmaf")]] _Float16 fmaf(_Float16 a, _Float16 b, _Float16 c);
[[loom::op("scalar.fmaf")]] float fmaf(float a, float b, float c);
[[loom::op("scalar.fmaf")]] double fmaf(double a, double b, double c);

// Base-10 logarithm.
[[loom::op("scalar.log10f")]] _Float16 log10f(_Float16 input);
[[loom::op("scalar.log10f")]] float log10f(float input);
[[loom::op("scalar.log10f")]] double log10f(double input);

// Natural logarithm of 1+x: ln(1+x) (numerically stable near 0).
[[loom::op("scalar.log1pf")]] _Float16 log1pf(_Float16 input);
[[loom::op("scalar.log1pf")]] float log1pf(float input);
[[loom::op("scalar.log1pf")]] double log1pf(double input);

// Base-2 logarithm.
[[loom::op("scalar.log2f")]] _Float16 log2f(_Float16 input);
[[loom::op("scalar.log2f")]] float log2f(float input);
[[loom::op("scalar.log2f")]] double log2f(double input);

// Natural logarithm: ln(x).
[[loom::op("scalar.logf")]] _Float16 logf(_Float16 input);
[[loom::op("scalar.logf")]] float logf(float input);
[[loom::op("scalar.logf")]] double logf(double input);

// Logistic sigmoid: 1 / (1 + exp(-x)).
[[loom::op("scalar.logisticf")]] _Float16 logisticf(_Float16 input);
[[loom::op("scalar.logisticf")]] float logisticf(float input);
[[loom::op("scalar.logisticf")]] double logisticf(double input);

// IEEE 754 maximum (NaN propagates).
[[loom::op("scalar.maximumf")]] _Float16 maximumf(_Float16 lhs, _Float16 rhs);
[[loom::op("scalar.maximumf")]] float maximumf(float lhs, float rhs);
[[loom::op("scalar.maximumf")]] double maximumf(double lhs, double rhs);

// C99 fmax (NaN ignored, returns the non-NaN operand).
[[loom::op("scalar.maxnumf")]] _Float16 maxnumf(_Float16 lhs, _Float16 rhs);
[[loom::op("scalar.maxnumf")]] float maxnumf(float lhs, float rhs);
[[loom::op("scalar.maxnumf")]] double maxnumf(double lhs, double rhs);

// IEEE 754 minimum (NaN propagates).
[[loom::op("scalar.minimumf")]] _Float16 minimumf(_Float16 lhs, _Float16 rhs);
[[loom::op("scalar.minimumf")]] float minimumf(float lhs, float rhs);
[[loom::op("scalar.minimumf")]] double minimumf(double lhs, double rhs);

// C99 fmin (NaN ignored, returns the non-NaN operand).
[[loom::op("scalar.minnumf")]] _Float16 minnumf(_Float16 lhs, _Float16 rhs);
[[loom::op("scalar.minnumf")]] float minnumf(float lhs, float rhs);
[[loom::op("scalar.minnumf")]] double minnumf(double lhs, double rhs);

// Floating-point multiplication.
[[loom::op("scalar.mulf")]] _Float16 mulf(_Float16 lhs, _Float16 rhs);
[[loom::op("scalar.mulf")]] float mulf(float lhs, float rhs);
[[loom::op("scalar.mulf")]] double mulf(double lhs, double rhs);

// Floating-point negation.
[[loom::op("scalar.negf")]] _Float16 negf(_Float16 input);
[[loom::op("scalar.negf")]] float negf(float input);
[[loom::op("scalar.negf")]] double negf(double input);

// Power: x^y.
[[loom::op("scalar.powf")]] _Float16 powf(_Float16 lhs, _Float16 rhs);
[[loom::op("scalar.powf")]] float powf(float lhs, float rhs);
[[loom::op("scalar.powf")]] double powf(double lhs, double rhs);

// Floating-point remainder (C fmod semantics).
[[loom::op("scalar.remf")]] _Float16 remf(_Float16 lhs, _Float16 rhs);
[[loom::op("scalar.remf")]] float remf(float lhs, float rhs);
[[loom::op("scalar.remf")]] double remf(double lhs, double rhs);

// Round to nearest, ties to even (IEEE 754 default rounding).
[[loom::op("scalar.roundevenf")]] _Float16 roundevenf(_Float16 input);
[[loom::op("scalar.roundevenf")]] float roundevenf(float input);
[[loom::op("scalar.roundevenf")]] double roundevenf(double input);

// Round to nearest, ties away from zero.
[[loom::op("scalar.roundf")]] _Float16 roundf(_Float16 input);
[[loom::op("scalar.roundf")]] float roundf(float input);
[[loom::op("scalar.roundf")]] double roundf(double input);

// Reciprocal square root: 1/sqrt(x).
[[loom::op("scalar.rsqrtf")]] _Float16 rsqrtf(_Float16 input);
[[loom::op("scalar.rsqrtf")]] float rsqrtf(float input);
[[loom::op("scalar.rsqrtf")]] double rsqrtf(double input);

// Floating-point sign: returns -1.0, 0.0, or 1.0.
[[loom::op("scalar.signf")]] _Float16 signf(_Float16 input);
[[loom::op("scalar.signf")]] float signf(float input);
[[loom::op("scalar.signf")]] double signf(double input);

// SiLU activation: x * logistic(x).
[[loom::op("scalar.siluf")]] _Float16 siluf(_Float16 input);
[[loom::op("scalar.siluf")]] float siluf(float input);
[[loom::op("scalar.siluf")]] double siluf(double input);

// Sine.
[[loom::op("scalar.sinf")]] _Float16 sinf(_Float16 input);
[[loom::op("scalar.sinf")]] float sinf(float input);
[[loom::op("scalar.sinf")]] double sinf(double input);

// Hyperbolic sine.
[[loom::op("scalar.sinhf")]] _Float16 sinhf(_Float16 input);
[[loom::op("scalar.sinhf")]] float sinhf(float input);
[[loom::op("scalar.sinhf")]] double sinhf(double input);

// Sine over turns: sin(2*pi*x), preserving finite-input periodicity and exact
// quarter-turn cardinals. Non-finite inputs produce NaN.
[[loom::op("scalar.sinturnsf")]] _Float16 sinturnsf(_Float16 input);
[[loom::op("scalar.sinturnsf")]] float sinturnsf(float input);
[[loom::op("scalar.sinturnsf")]] double sinturnsf(double input);

// Softplus activation: log(1 + exp(x)).
[[loom::op("scalar.softplusf")]] _Float16 softplusf(_Float16 input);
[[loom::op("scalar.softplusf")]] float softplusf(float input);
[[loom::op("scalar.softplusf")]] double softplusf(double input);

// Square root.
[[loom::op("scalar.sqrtf")]] _Float16 sqrtf(_Float16 input);
[[loom::op("scalar.sqrtf")]] float sqrtf(float input);
[[loom::op("scalar.sqrtf")]] double sqrtf(double input);

// Floating-point subtraction.
[[loom::op("scalar.subf")]] _Float16 subf(_Float16 lhs, _Float16 rhs);
[[loom::op("scalar.subf")]] float subf(float lhs, float rhs);
[[loom::op("scalar.subf")]] double subf(double lhs, double rhs);

// Tangent.
[[loom::op("scalar.tanf")]] _Float16 tanf(_Float16 input);
[[loom::op("scalar.tanf")]] float tanf(float input);
[[loom::op("scalar.tanf")]] double tanf(double input);

// Hyperbolic tangent.
[[loom::op("scalar.tanhf")]] _Float16 tanhf(_Float16 input);
[[loom::op("scalar.tanhf")]] float tanhf(float input);
[[loom::op("scalar.tanhf")]] double tanhf(double input);

// Round toward zero (C trunc).
[[loom::op("scalar.truncf")]] _Float16 truncf(_Float16 input);
[[loom::op("scalar.truncf")]] float truncf(float input);
[[loom::op("scalar.truncf")]] double truncf(double input);

namespace approximate {

// Floating-point absolute value.
[[loom::op("scalar.absf", "afn")]] _Float16 absf(_Float16 input);
[[loom::op("scalar.absf", "afn")]] float absf(float input);
[[loom::op("scalar.absf", "afn")]] double absf(double input);

// Arccosine.
[[loom::op("scalar.acosf", "afn")]] _Float16 acosf(_Float16 input);
[[loom::op("scalar.acosf", "afn")]] float acosf(float input);
[[loom::op("scalar.acosf", "afn")]] double acosf(double input);

// Inverse hyperbolic cosine.
[[loom::op("scalar.acoshf", "afn")]] _Float16 acoshf(_Float16 input);
[[loom::op("scalar.acoshf", "afn")]] float acoshf(float input);
[[loom::op("scalar.acoshf", "afn")]] double acoshf(double input);

// Floating-point addition.
[[loom::op("scalar.addf", "afn")]] _Float16 addf(_Float16 lhs, _Float16 rhs);
[[loom::op("scalar.addf", "afn")]] float addf(float lhs, float rhs);
[[loom::op("scalar.addf", "afn")]] double addf(double lhs, double rhs);

// Arcsine.
[[loom::op("scalar.asinf", "afn")]] _Float16 asinf(_Float16 input);
[[loom::op("scalar.asinf", "afn")]] float asinf(float input);
[[loom::op("scalar.asinf", "afn")]] double asinf(double input);

// Inverse hyperbolic sine.
[[loom::op("scalar.asinhf", "afn")]] _Float16 asinhf(_Float16 input);
[[loom::op("scalar.asinhf", "afn")]] float asinhf(float input);
[[loom::op("scalar.asinhf", "afn")]] double asinhf(double input);

// Two-argument arctangent: atan2(y, x).
[[loom::op("scalar.atan2f", "afn")]] _Float16 atan2f(_Float16 lhs, _Float16 rhs);
[[loom::op("scalar.atan2f", "afn")]] float atan2f(float lhs, float rhs);
[[loom::op("scalar.atan2f", "afn")]] double atan2f(double lhs, double rhs);

// Arctangent.
[[loom::op("scalar.atanf", "afn")]] _Float16 atanf(_Float16 input);
[[loom::op("scalar.atanf", "afn")]] float atanf(float input);
[[loom::op("scalar.atanf", "afn")]] double atanf(double input);

// Inverse hyperbolic tangent.
[[loom::op("scalar.atanhf", "afn")]] _Float16 atanhf(_Float16 input);
[[loom::op("scalar.atanhf", "afn")]] float atanhf(float input);
[[loom::op("scalar.atanhf", "afn")]] double atanhf(double input);

// Cube root.
[[loom::op("scalar.cbrtf", "afn")]] _Float16 cbrtf(_Float16 input);
[[loom::op("scalar.cbrtf", "afn")]] float cbrtf(float input);
[[loom::op("scalar.cbrtf", "afn")]] double cbrtf(double input);

// Round toward positive infinity.
[[loom::op("scalar.ceilf", "afn")]] _Float16 ceilf(_Float16 input);
[[loom::op("scalar.ceilf", "afn")]] float ceilf(float input);
[[loom::op("scalar.ceilf", "afn")]] double ceilf(double input);

// Copy sign of rhs onto magnitude of lhs.
[[loom::op("scalar.copysignf", "afn")]] _Float16 copysignf(_Float16 lhs, _Float16 rhs);
[[loom::op("scalar.copysignf", "afn")]] float copysignf(float lhs, float rhs);
[[loom::op("scalar.copysignf", "afn")]] double copysignf(double lhs, double rhs);

// Cosine.
[[loom::op("scalar.cosf", "afn")]] _Float16 cosf(_Float16 input);
[[loom::op("scalar.cosf", "afn")]] float cosf(float input);
[[loom::op("scalar.cosf", "afn")]] double cosf(double input);

// Hyperbolic cosine.
[[loom::op("scalar.coshf", "afn")]] _Float16 coshf(_Float16 input);
[[loom::op("scalar.coshf", "afn")]] float coshf(float input);
[[loom::op("scalar.coshf", "afn")]] double coshf(double input);

// Cosine over turns: cos(2*pi*x), preserving finite-input periodicity and
// exact quarter-turn cardinals. Non-finite inputs produce NaN.
[[loom::op("scalar.costurnsf", "afn")]] _Float16 costurnsf(_Float16 input);
[[loom::op("scalar.costurnsf", "afn")]] float costurnsf(float input);
[[loom::op("scalar.costurnsf", "afn")]] double costurnsf(double input);

// Floating-point division.
[[loom::op("scalar.divf", "afn")]] _Float16 divf(_Float16 lhs, _Float16 rhs);
[[loom::op("scalar.divf", "afn")]] float divf(float lhs, float rhs);
[[loom::op("scalar.divf", "afn")]] double divf(double lhs, double rhs);

// Complementary error function: 1 - erf(x).
[[loom::op("scalar.erfcf", "afn")]] _Float16 erfcf(_Float16 input);
[[loom::op("scalar.erfcf", "afn")]] float erfcf(float input);
[[loom::op("scalar.erfcf", "afn")]] double erfcf(double input);

// Error function (used in GeLU activation).
[[loom::op("scalar.erff", "afn")]] _Float16 erff(_Float16 input);
[[loom::op("scalar.erff", "afn")]] float erff(float input);
[[loom::op("scalar.erff", "afn")]] double erff(double input);

// Base-2 exponential: 2^x.
[[loom::op("scalar.exp2f", "afn")]] _Float16 exp2f(_Float16 input);
[[loom::op("scalar.exp2f", "afn")]] float exp2f(float input);
[[loom::op("scalar.exp2f", "afn")]] double exp2f(double input);

// Exponential: e^x.
[[loom::op("scalar.expf", "afn")]] _Float16 expf(_Float16 input);
[[loom::op("scalar.expf", "afn")]] float expf(float input);
[[loom::op("scalar.expf", "afn")]] double expf(double input);

// Exponential minus one: e^x - 1 (numerically stable near 0).
[[loom::op("scalar.expm1f", "afn")]] _Float16 expm1f(_Float16 input);
[[loom::op("scalar.expm1f", "afn")]] float expm1f(float input);
[[loom::op("scalar.expm1f", "afn")]] double expm1f(double input);

// Round toward negative infinity.
[[loom::op("scalar.floorf", "afn")]] _Float16 floorf(_Float16 input);
[[loom::op("scalar.floorf", "afn")]] float floorf(float input);
[[loom::op("scalar.floorf", "afn")]] double floorf(double input);

// Fused multiply-add: a*b + c with single rounding.
[[loom::op("scalar.fmaf", "afn")]] _Float16 fmaf(_Float16 a, _Float16 b, _Float16 c);
[[loom::op("scalar.fmaf", "afn")]] float fmaf(float a, float b, float c);
[[loom::op("scalar.fmaf", "afn")]] double fmaf(double a, double b, double c);

// Base-10 logarithm.
[[loom::op("scalar.log10f", "afn")]] _Float16 log10f(_Float16 input);
[[loom::op("scalar.log10f", "afn")]] float log10f(float input);
[[loom::op("scalar.log10f", "afn")]] double log10f(double input);

// Natural logarithm of 1+x: ln(1+x) (numerically stable near 0).
[[loom::op("scalar.log1pf", "afn")]] _Float16 log1pf(_Float16 input);
[[loom::op("scalar.log1pf", "afn")]] float log1pf(float input);
[[loom::op("scalar.log1pf", "afn")]] double log1pf(double input);

// Base-2 logarithm.
[[loom::op("scalar.log2f", "afn")]] _Float16 log2f(_Float16 input);
[[loom::op("scalar.log2f", "afn")]] float log2f(float input);
[[loom::op("scalar.log2f", "afn")]] double log2f(double input);

// Natural logarithm: ln(x).
[[loom::op("scalar.logf", "afn")]] _Float16 logf(_Float16 input);
[[loom::op("scalar.logf", "afn")]] float logf(float input);
[[loom::op("scalar.logf", "afn")]] double logf(double input);

// Logistic sigmoid: 1 / (1 + exp(-x)).
[[loom::op("scalar.logisticf", "afn")]] _Float16 logisticf(_Float16 input);
[[loom::op("scalar.logisticf", "afn")]] float logisticf(float input);
[[loom::op("scalar.logisticf", "afn")]] double logisticf(double input);

// IEEE 754 maximum (NaN propagates).
[[loom::op("scalar.maximumf", "afn")]] _Float16 maximumf(_Float16 lhs, _Float16 rhs);
[[loom::op("scalar.maximumf", "afn")]] float maximumf(float lhs, float rhs);
[[loom::op("scalar.maximumf", "afn")]] double maximumf(double lhs, double rhs);

// C99 fmax (NaN ignored, returns the non-NaN operand).
[[loom::op("scalar.maxnumf", "afn")]] _Float16 maxnumf(_Float16 lhs, _Float16 rhs);
[[loom::op("scalar.maxnumf", "afn")]] float maxnumf(float lhs, float rhs);
[[loom::op("scalar.maxnumf", "afn")]] double maxnumf(double lhs, double rhs);

// IEEE 754 minimum (NaN propagates).
[[loom::op("scalar.minimumf", "afn")]] _Float16 minimumf(_Float16 lhs, _Float16 rhs);
[[loom::op("scalar.minimumf", "afn")]] float minimumf(float lhs, float rhs);
[[loom::op("scalar.minimumf", "afn")]] double minimumf(double lhs, double rhs);

// C99 fmin (NaN ignored, returns the non-NaN operand).
[[loom::op("scalar.minnumf", "afn")]] _Float16 minnumf(_Float16 lhs, _Float16 rhs);
[[loom::op("scalar.minnumf", "afn")]] float minnumf(float lhs, float rhs);
[[loom::op("scalar.minnumf", "afn")]] double minnumf(double lhs, double rhs);

// Floating-point multiplication.
[[loom::op("scalar.mulf", "afn")]] _Float16 mulf(_Float16 lhs, _Float16 rhs);
[[loom::op("scalar.mulf", "afn")]] float mulf(float lhs, float rhs);
[[loom::op("scalar.mulf", "afn")]] double mulf(double lhs, double rhs);

// Floating-point negation.
[[loom::op("scalar.negf", "afn")]] _Float16 negf(_Float16 input);
[[loom::op("scalar.negf", "afn")]] float negf(float input);
[[loom::op("scalar.negf", "afn")]] double negf(double input);

// Power: x^y.
[[loom::op("scalar.powf", "afn")]] _Float16 powf(_Float16 lhs, _Float16 rhs);
[[loom::op("scalar.powf", "afn")]] float powf(float lhs, float rhs);
[[loom::op("scalar.powf", "afn")]] double powf(double lhs, double rhs);

// Floating-point remainder (C fmod semantics).
[[loom::op("scalar.remf", "afn")]] _Float16 remf(_Float16 lhs, _Float16 rhs);
[[loom::op("scalar.remf", "afn")]] float remf(float lhs, float rhs);
[[loom::op("scalar.remf", "afn")]] double remf(double lhs, double rhs);

// Round to nearest, ties to even (IEEE 754 default rounding).
[[loom::op("scalar.roundevenf", "afn")]] _Float16 roundevenf(_Float16 input);
[[loom::op("scalar.roundevenf", "afn")]] float roundevenf(float input);
[[loom::op("scalar.roundevenf", "afn")]] double roundevenf(double input);

// Round to nearest, ties away from zero.
[[loom::op("scalar.roundf", "afn")]] _Float16 roundf(_Float16 input);
[[loom::op("scalar.roundf", "afn")]] float roundf(float input);
[[loom::op("scalar.roundf", "afn")]] double roundf(double input);

// Reciprocal square root: 1/sqrt(x).
[[loom::op("scalar.rsqrtf", "afn")]] _Float16 rsqrtf(_Float16 input);
[[loom::op("scalar.rsqrtf", "afn")]] float rsqrtf(float input);
[[loom::op("scalar.rsqrtf", "afn")]] double rsqrtf(double input);

// SiLU activation: x * logistic(x).
[[loom::op("scalar.siluf", "afn")]] _Float16 siluf(_Float16 input);
[[loom::op("scalar.siluf", "afn")]] float siluf(float input);
[[loom::op("scalar.siluf", "afn")]] double siluf(double input);

// Sine.
[[loom::op("scalar.sinf", "afn")]] _Float16 sinf(_Float16 input);
[[loom::op("scalar.sinf", "afn")]] float sinf(float input);
[[loom::op("scalar.sinf", "afn")]] double sinf(double input);

// Hyperbolic sine.
[[loom::op("scalar.sinhf", "afn")]] _Float16 sinhf(_Float16 input);
[[loom::op("scalar.sinhf", "afn")]] float sinhf(float input);
[[loom::op("scalar.sinhf", "afn")]] double sinhf(double input);

// Sine over turns: sin(2*pi*x), preserving finite-input periodicity and exact
// quarter-turn cardinals. Non-finite inputs produce NaN.
[[loom::op("scalar.sinturnsf", "afn")]] _Float16 sinturnsf(_Float16 input);
[[loom::op("scalar.sinturnsf", "afn")]] float sinturnsf(float input);
[[loom::op("scalar.sinturnsf", "afn")]] double sinturnsf(double input);

// Softplus activation: log(1 + exp(x)).
[[loom::op("scalar.softplusf", "afn")]] _Float16 softplusf(_Float16 input);
[[loom::op("scalar.softplusf", "afn")]] float softplusf(float input);
[[loom::op("scalar.softplusf", "afn")]] double softplusf(double input);

// Square root.
[[loom::op("scalar.sqrtf", "afn")]] _Float16 sqrtf(_Float16 input);
[[loom::op("scalar.sqrtf", "afn")]] float sqrtf(float input);
[[loom::op("scalar.sqrtf", "afn")]] double sqrtf(double input);

// Floating-point subtraction.
[[loom::op("scalar.subf", "afn")]] _Float16 subf(_Float16 lhs, _Float16 rhs);
[[loom::op("scalar.subf", "afn")]] float subf(float lhs, float rhs);
[[loom::op("scalar.subf", "afn")]] double subf(double lhs, double rhs);

// Tangent.
[[loom::op("scalar.tanf", "afn")]] _Float16 tanf(_Float16 input);
[[loom::op("scalar.tanf", "afn")]] float tanf(float input);
[[loom::op("scalar.tanf", "afn")]] double tanf(double input);

// Hyperbolic tangent.
[[loom::op("scalar.tanhf", "afn")]] _Float16 tanhf(_Float16 input);
[[loom::op("scalar.tanhf", "afn")]] float tanhf(float input);
[[loom::op("scalar.tanhf", "afn")]] double tanhf(double input);

// Round toward zero (C trunc).
[[loom::op("scalar.truncf", "afn")]] _Float16 truncf(_Float16 input);
[[loom::op("scalar.truncf", "afn")]] float truncf(float input);
[[loom::op("scalar.truncf", "afn")]] double truncf(double input);

}  // namespace approximate

}  // namespace loom::scalar

#endif  // LOOMCXX_SCALAR_H_
