// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/value/representation.h"

#include <array>

#include "iree/testing/gtest.h"

namespace loom::cxx_import {
namespace {

TEST(ValueTest, IdentityIsIndependentOfComponentCount) {
  const Partition empty{ValueKind::Record, 0};
  const Partition pair{ValueKind::Record, 2};
  ValueArena arena;
  std::array<loom_value_id_t, 2> components{7, 11};
  auto pointer = arena.capture(kPointerPartition, components);
  auto record = arena.capture(pair, components);
  EXPECT_TRUE(pointer.is_pointer());
  EXPECT_FALSE(record.is_pointer());
  EXPECT_TRUE(record.is_record());
  EXPECT_EQ(record.components().size(), 2u);
  auto zero = arena.capture(empty, {});
  EXPECT_TRUE(zero.is_record());
  EXPECT_TRUE(zero.components().empty());
}

TEST(ValueTest,
     CapturesTransientComponentsAndPreservesCopiesAcrossReplacement) {
  const Partition record{ValueKind::Record, 6};
  const Partition member{ValueKind::Record, 3};
  ValueArena arena;
  std::array<loom_value_id_t, 6> transient{1, 2, 3, 4, 5, 6};
  auto original = arena.capture(record, transient);
  auto snapshot = original;
  auto projection = original.project(member, 3);
  transient.fill(91);
  auto modified = arena.replace(original, 1, Value(42));
  for (unsigned index = 0; index < 10000; ++index) {
    transient[0] = index;
    arena.capture(record, transient);
  }
  EXPECT_EQ(snapshot.components()[1], 2u);
  EXPECT_EQ(modified.components()[1], 42u);
  EXPECT_EQ(modified.components()[0], 1u);
  EXPECT_EQ(modified.components()[5], 6u);
  EXPECT_EQ(projection.components()[0], 4u);
  EXPECT_EQ(projection.components()[2], 6u);
  EXPECT_EQ(projection.components().data(), original.components().data() + 3);
}

TEST(ValueTest, EqualPartitionsDoNotShareDistinctBindings) {
  const Partition member{ValueKind::Record, 3};
  const Partition record{ValueKind::Record, 6};
  ValueArena arena;
  const loom_value_id_t components[] = {1, 2, 3, 4, 5, 6};
  auto original = arena.capture(record, components);
  auto first = original.project(member, 0);
  auto second = original.project(member, 3);
  EXPECT_EQ(&first.partition(), &second.partition());
  EXPECT_NE(first.components()[0], second.components()[0]);
  auto copy = arena.replace(original, 0, second);
  EXPECT_EQ(copy.components()[0], 4u);
  EXPECT_EQ(copy.components()[3], 4u);
  EXPECT_EQ(original.components()[0], 1u);
}

}  // namespace
}  // namespace loom::cxx_import
