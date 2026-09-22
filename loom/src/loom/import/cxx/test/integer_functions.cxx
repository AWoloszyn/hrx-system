// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Fixed-point rescaling keeps the widened product until after the shift.
int fixed_multiply(int left, int right) {
  long long product = (long long)left * (long long)right;
  return (int)(product >> 16);
}

unsigned byte_increment(unsigned input) {
  unsigned char value = (unsigned char)input;
  ++value;
  return value;
}

unsigned byte_decrement(unsigned input) {
  unsigned char value = (unsigned char)input;
  value--;
  return value;
}

int short_decrement(int input) {
  short value = (short)input;
  --value;
  return value;
}

unsigned long long wide_increment(unsigned long long input) {
  unsigned long long value = input;
  value++;
  return value;
}

// C++ promotes shift operands independently, including counts wider than the
// left operand. All callers keep counts within the defined source domain.
unsigned shift_left_narrow(unsigned value, unsigned long long count) {
  return value << count;
}

unsigned long long shift_left_wide(unsigned long long value, unsigned count) {
  return value << count;
}

long long shift_right_signed(long long value, unsigned count) {
  return value >> count;
}

unsigned long long shift_right_unsigned(unsigned long long value,
                                        unsigned count) {
  return value >> count;
}
