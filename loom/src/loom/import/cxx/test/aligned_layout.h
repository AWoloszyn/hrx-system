// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

struct __attribute__((packed, aligned(8))) AlignedPacket {
  unsigned char tag;
  unsigned value;
};
static_assert(sizeof(AlignedPacket) == 8);
static_assert(alignof(AlignedPacket) == 8);
static_assert(__builtin_offsetof(AlignedPacket, value) == 1);

struct [[gnu::packed]] AlignedField {
  unsigned char tag;
  unsigned value __attribute__((aligned(2)));
};
static_assert(sizeof(AlignedField) == 6);
static_assert(alignof(AlignedField) == 2);
static_assert(__builtin_offsetof(AlignedField, value) == 2);

struct AlignedAliases {
  unsigned char tag;
  unsigned value __attribute__((unused, __aligned__(16)));
  [[using gnu: aligned(8)]] unsigned tail;
};
static_assert(sizeof(AlignedAliases) == 32);
static_assert(__builtin_offsetof(AlignedAliases, value) == 16);
static_assert(__builtin_offsetof(AlignedAliases, tail) == 24);

struct [[gnu::aligned(2)]] NaturalAlignment {
  unsigned value;
};
static_assert(alignof(NaturalAlignment) == alignof(unsigned));

struct [[gnu::aligned(8), gnu::aligned(16)]] RepeatedAlignment {
  alignas(8) unsigned first __attribute__((aligned(32), aligned(16)));
};
static_assert(sizeof(RepeatedAlignment) == 32);
static_assert(alignof(RepeatedAlignment) == 32);

#pragma pack(push, 1)
struct CappedAlignment {
  unsigned char tag;
  unsigned value __attribute__((aligned(16)));
} __attribute__((aligned(8)));
#pragma pack(pop)
static_assert(sizeof(CappedAlignment) == 8);
static_assert(alignof(CappedAlignment) == 8);
static_assert(__builtin_offsetof(CappedAlignment, value) == 1);

template <unsigned Alignment>
struct AlignedBlock {
  unsigned char tag;
  unsigned value __attribute__((aligned(Alignment)));
} __attribute__((packed, aligned(Alignment * 2)));
static_assert(sizeof(AlignedBlock<8>) == 16);
static_assert(alignof(AlignedBlock<8>) == 16);
static_assert(__builtin_offsetof(AlignedBlock<8>, value) == 8);
static_assert(sizeof(AlignedBlock<2>) == 8);
static_assert(alignof(AlignedBlock<2>) == 4);
static_assert(__builtin_offsetof(AlignedBlock<2>, value) == 2);
static_assert(sizeof(AlignedBlock<8>) == 16);

struct __attribute__((aligned(16))) ForwardAlignment;
struct ForwardAlignment {
  unsigned value;
};
static_assert(sizeof(ForwardAlignment) == 16);
static_assert(alignof(ForwardAlignment) == 16);

struct __attribute__((packed)) AlignedPackedBits {
  char tag;
  __attribute__((aligned(2))) unsigned bits : 3;
  char tail;
};
static_assert(sizeof(AlignedPackedBits) == 4 &&
              alignof(AlignedPackedBits) == 2 &&
              __builtin_offsetof(AlignedPackedBits, tail) == 3);
struct AlignedBits {
  char tag;
  __attribute__((aligned(16))) unsigned bits : 3;
  char tail;
};
static_assert(sizeof(AlignedBits) == 32 && alignof(AlignedBits) == 16 &&
              __builtin_offsetof(AlignedBits, tail) == 17);
struct __attribute__((packed)) AlignedZeroWidth {
  char tag;
  __attribute__((aligned(8))) unsigned : 0;
  char tail;
};
static_assert(sizeof(AlignedZeroWidth) == 9 && alignof(AlignedZeroWidth) == 1 &&
              __builtin_offsetof(AlignedZeroWidth, tail) == 8);

enum AlignmentValue { AlignmentBytes = 8 };
struct [[gnu::aligned(AlignmentBytes)]] EnumAligned {
  unsigned value;
};
static_assert(alignof(EnumAligned) == 8);
