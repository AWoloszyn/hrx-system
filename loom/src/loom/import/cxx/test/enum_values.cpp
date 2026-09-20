// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <hip/hip_runtime.h>

enum class Command : unsigned { world, water, sky, fill, depth, alias, sprite };
enum class Byte : unsigned char { zero, high = 255 };
enum class SignedByte : signed char { low = -128, high = 127 };
enum class Short : unsigned short { zero, high = 65535 };
enum class Word : unsigned { zero, high = 0xffffffffu };
enum class Long : unsigned long long { zero, high = 0xffffffffffffffffULL };
enum Inferred { low = -1, high = 1ULL << 40 };
enum InferredUnsigned { maximum = 0xffffffffffffffffULL };

unsigned enum_dispatch(Command kind) {
  if (kind == Command::water) {
    return 128u;
  }
  if (kind >= Command::alias) {
    return 255u;
  }
  return static_cast<unsigned>(kind) + 7u;
}

unsigned enum_byte(Byte value) {
  Byte next = static_cast<Byte>(static_cast<unsigned>(value) + 1u);
  return static_cast<unsigned>(next);
}

long long enum_signed(SignedByte value) {
  return static_cast<long long>(value) * 65537;
}

unsigned long long enum_unsigned(Word value) {
  return static_cast<unsigned long long>(value) + 1ULL;
}

unsigned enum_compare64(Long left, Long right) { return left < right; }

long long enum_inferred(unsigned choose) { return choose ? high : low; }

unsigned enum_inferred_unsigned(unsigned long long value) {
  return value < maximum;
}

template <unsigned long long Value>
static unsigned long long specialized(unsigned choose) {
  enum Kind { first = Value, next, last = next + 1 };
  return choose ? last : first;
}

unsigned long long enum_specialization(unsigned choose) {
  return specialized<(1ULL << 40)>(choose) + specialized<0xffffffffu>(choose);
}

unsigned enum_bool(unsigned value) {
  enum class Flag : bool { no, yes };
  Flag flag = value ? Flag::yes : Flag::no;
  return flag == Flag::yes;
}

template <class T>
static T add(T value, T delta) {
  return static_cast<T>(static_cast<unsigned long long>(value) +
                        static_cast<unsigned long long>(delta));
}

__global__ [[loom::workgroup_size(64, 1, 1), loom::workgroup_count(1, 1, 1)]]
void enum_storage_u8(const Byte* input, Byte* output, Byte delta) {
  Byte* cursor = output + threadIdx.x + 1;
  cursor[-1] = add(input[threadIdx.x], delta);
}

__global__ [[loom::workgroup_size(64, 1, 1), loom::workgroup_count(1, 1, 1)]]
void enum_storage_u16(const Short* input, Short* output, Short delta) {
  Short* cursor = output + threadIdx.x + 1;
  cursor[-1] = add(input[threadIdx.x], delta);
}

__global__ [[loom::workgroup_size(64, 1, 1), loom::workgroup_count(1, 1, 1)]]
void enum_storage_u32(const Word* input, Word* output, Word delta) {
  Word* cursor = output + threadIdx.x + 1;
  cursor[-1] = add(input[threadIdx.x], delta);
}

__global__ [[loom::workgroup_size(64, 1, 1), loom::workgroup_count(1, 1, 1)]]
void enum_storage_u64(const Long* input, Long* output, Long delta) {
  Long* cursor = output + threadIdx.x + 1;
  cursor[-1] = add(input[threadIdx.x], delta);
}
