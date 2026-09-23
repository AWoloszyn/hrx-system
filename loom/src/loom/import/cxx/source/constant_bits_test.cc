// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <cxx/ast_interpreter.h>
#include <cxx/constant_bits.h>
#include <cxx/control.h>
#include <cxx/memory_layout.h>
#include <cxx/types.h>

#include "iree/testing/gtest.h"
#include "loom/import/cxx/source/source.h"

namespace loom::cxx_import {
namespace {

TEST(ConstantBitsTest, EveryNarrowEncodingSurvivesCopiesAndBitCasts) {
  loom_cxx_import_options_t options;
  loom_cxx_import_options_initialize(&options);
  Source source(IREE_SV(""), IREE_SV("constants.cxx"), options);
  auto* control = source.unit().control();
  cxx::ASTInterpreter interpreter(&source.unit());
  const cxx::Type* formats[] = {
      control->getFloat16Type(),
      control->getBFloat16Type(),
      control->getFloat8E4M3FNType(),
      control->getFloat8E5M2Type(),
  };
  for (auto* type : formats) {
    auto format = *cxx::ConstFloat::formatFor(type->kind());
    auto width = cxx::ConstFloat::fromBits(format, 0).bitWidth();
    const cxx::Type* storage =
        width == 8
            ? static_cast<const cxx::Type*>(control->getUnsignedCharType())
            : control->getUnsignedShortIntType();
    for (uint32_t bits = 0; bits < (1u << width); ++bits) {
      SCOPED_TRACE(bits);
      SCOPED_TRACE(static_cast<unsigned>(format));
      auto floating =
          cxx::bitCastConstant(interpreter, std::intmax_t(bits), storage, type);
      ASSERT_TRUE(floating);
      auto raw = std::get<cxx::ConstFloat>(*floating);
      EXPECT_EQ(raw.bits(), bits);
      auto copy = interpreter.cloneValue(*floating);
      auto converted = interpreter.convertArithmetic(copy, type, type);
      ASSERT_TRUE(converted);
      auto restored =
          cxx::bitCastConstant(interpreter, *converted, type, storage);
      ASSERT_TRUE(restored);
      EXPECT_EQ(interpreter.toUInt(*restored), bits);
      if (!raw.isNaN()) {
        auto numeric = interpreter.toArithmeticType(raw.toDouble(), type);
        ASSERT_TRUE(numeric);
        EXPECT_EQ(std::get<cxx::ConstFloat>(*numeric).bits(), bits);
      }
    }
  }
}

TEST(ConstantBitsTest, RegroupsTargetBytesIndependentlyOfHostByteOrder) {
  loom_cxx_import_options_t options;
  loom_cxx_import_options_initialize(&options);
  Source source(IREE_SV(""), IREE_SV("bytes.cxx"), options);
  auto* control = source.unit().control();
  auto* layout = control->memoryLayout();
  auto* storage = control->getUnsignedIntType();
  auto* vector = control->getVectorType(control->getUnsignedShortIntType(), 2,
                                        cxx::VectorKind::kExt);
  cxx::ASTInterpreter interpreter(&source.unit());
  for (auto order : {cxx::MemoryLayout::ByteOrder::kLittleEndian,
                     cxx::MemoryLayout::ByteOrder::kBigEndian}) {
    layout->setByteOrder(order);
    auto value = cxx::bitCastConstant(interpreter, std::intmax_t(0x12345678),
                                      storage, vector);
    ASSERT_TRUE(value);
    auto elements = std::get<std::shared_ptr<cxx::InitializerList>>(*value);
    ASSERT_EQ(elements->elements.size(), 2u);
    bool little = order == cxx::MemoryLayout::ByteOrder::kLittleEndian;
    EXPECT_EQ(interpreter.toUInt(std::get<0>(elements->elements[0])),
              little ? 0x5678u : 0x1234u);
    EXPECT_EQ(interpreter.toUInt(std::get<0>(elements->elements[1])),
              little ? 0x1234u : 0x5678u);
    auto restored = cxx::bitCastConstant(interpreter, *value, vector, storage);
    ASSERT_TRUE(restored);
    EXPECT_EQ(interpreter.toUInt(*restored), 0x12345678u);
  }
}

TEST(ConstantBitsTest, RejectsInvalidOrUnsupportedObjectRepresentations) {
  loom_cxx_import_options_t options;
  loom_cxx_import_options_initialize(&options);
  Source source(IREE_SV(""), IREE_SV("objects.cxx"), options);
  auto* control = source.unit().control();
  cxx::ASTInterpreter interpreter(&source.unit());
  EXPECT_FALSE(cxx::bitCastConstant(interpreter, std::intmax_t(2),
                                    control->getUnsignedCharType(),
                                    control->getBoolType()));
  EXPECT_FALSE(cxx::bitCastConstant(interpreter, cxx::IndeterminateValue{},
                                    control->getUnsignedIntType(),
                                    control->getFloatType()));
  EXPECT_FALSE(cxx::bitCastConstant(
      interpreter, std::intmax_t(0), control->getUnsignedLongLongIntType(),
      control->getPointerType(control->getUnsignedIntType())));
  auto* packed_bool =
      control->getVectorType(control->getBoolType(), 8, cxx::VectorKind::kExt);
  EXPECT_FALSE(cxx::bitCastConstant(interpreter, std::intmax_t(0),
                                    control->getUnsignedCharType(),
                                    packed_bool));
  auto* padded =
      control->getVectorType(control->getFloatType(), 3, cxx::VectorKind::kExt);
  auto* full = control->getVectorType(control->getUnsignedIntType(), 4,
                                      cxx::VectorKind::kExt);
  auto zeros = interpreter.zeroInitialize(full);
  ASSERT_TRUE(zeros);
  EXPECT_FALSE(cxx::bitCastConstant(interpreter, *zeros, full, padded));
}

}  // namespace
}  // namespace loom::cxx_import
