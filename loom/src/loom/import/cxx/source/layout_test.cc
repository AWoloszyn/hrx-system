// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <cxx/symbols.h>
#include <cxx/views/symbol_chain.h>

#include "iree/testing/gtest.h"
#include "loom/import/cxx/source/source.h"

namespace loom::cxx_import {
namespace {

class LayoutTest : public ::testing::TestWithParam<loom_cxx_data_model_t> {};

TEST_P(LayoutTest, PragmaPackedBitPositionsBelongToTheSourceLayout) {
  loom_cxx_import_options_t options;
  loom_cxx_import_options_initialize(&options);
  options.data_model = GetParam();
  Source source(IREE_SV("#pragma pack(push, 1)\n"
                        "struct Packet { unsigned first:31, second:2; };\n"
                        "#pragma pack(pop)\n"),
                IREE_SV("layout.cpp"), options);
  auto records = source.unit().globalScope()->find("Packet");
  ASSERT_FALSE(records.begin() == records.end());
  auto* record = cxx::symbol_cast<cxx::ClassSymbol>(*records.begin());
  ASSERT_NE(record, nullptr);
  EXPECT_EQ(record->sizeInBytes(), 5);
  EXPECT_EQ(record->alignment(), 1);
  auto fields = record->find("second");
  ASSERT_FALSE(fields.begin() == fields.end());
  auto* second = cxx::symbol_cast<cxx::FieldSymbol>(*fields.begin());
  ASSERT_NE(second, nullptr);
  auto position = record->layout()->getFieldInfo(second);
  ASSERT_TRUE(position.has_value());
  EXPECT_EQ(position->offset * 8 + position->bitOffset, 31u);
  EXPECT_EQ(position->bitWidth, 2u);
  EXPECT_EQ(position->allocUnitSizeBytes, 5u);
  EXPECT_EQ(second->localOffset() * 8 + second->bitFieldOffset(), 31);
}

INSTANTIATE_TEST_SUITE_P(DataModels, LayoutTest,
                         ::testing::Values(LOOM_CXX_DATA_MODEL_LP64,
                                           LOOM_CXX_DATA_MODEL_LLP64,
                                           LOOM_CXX_DATA_MODEL_ILP32));

}  // namespace
}  // namespace loom::cxx_import
