// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <loomcxx/kernel.h>

LOOM_FORCE_INLINE static unsigned sum(unsigned count, unsigned factor) {
  unsigned total = 0;
  [[loom::unroll(factor)]]
  for (unsigned index = 0; index < count; ++index) {
    total += index;
  }
  return total;
}

unsigned schedule_call(unsigned count) {
  unsigned factor = 4;
  --factor;
  return sum(count, factor);
}

unsigned schedule_snapshot(unsigned count) {
  unsigned factor = 3;
  unsigned total = 0;
  [[loom::unroll(factor)]]
  for (unsigned index = 0; index < count; ++index) {
    total += index;
    ++factor;
  }
  return total + factor;
}

unsigned schedule_wide(unsigned count) {
  unsigned long long factor = (1ULL << 32) + 3;
  unsigned total = 0;
  [[loom::unroll(factor - (1ULL << 32))]]
  for (unsigned index = 0; index < count; ++index) {
    total += index;
  }
  return total;
}

unsigned schedule_narrow(unsigned count) {
  unsigned char factor = 131;
  factor -= 128;
  unsigned total = 0;
  [[loom::unroll(factor)]]
  for (unsigned index = 0; index < count; ++index) {
    total += index;
  }
  return total;
}

unsigned schedule_signed(unsigned count) {
  signed char factor = -1;
  factor += 4;
  unsigned total = 0;
  [[loom::unroll(factor)]]
  for (unsigned index = 0; index < count; ++index) {
    total += index;
  }
  return total;
}

unsigned schedule_unevaluated(unsigned count) {
  unsigned total = 0;
  [[loom::unroll(sizeof(++count) / alignof(unsigned))]]
  for (unsigned index = 0; index < count; ++index) {
    total += index;
  }
  return total + count + sizeof(long) + 'A';
}

unsigned schedule_initializer(unsigned count) {
  int factor = -1;
  unsigned total = 0;
  [[loom::unroll(factor)]]
  for (unsigned index = ++factor; index < count; ++index) {
    total += index;
  }
  return total + factor;
}

unsigned schedule_serial(unsigned count) { return sum(count, 0); }
