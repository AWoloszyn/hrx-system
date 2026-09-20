// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <cxx/archive.h>
#include <cxx/private/semantic_codec.h>
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

TEST_P(LayoutTest, PackedRequestsAndResolvedLayoutSurviveSemanticArchives) {
  loom_cxx_import_options_t options;
  loom_cxx_import_options_initialize(&options);
  options.data_model = GetParam();
  Source source(IREE_SV("struct [[gnu::packed, gnu::aligned(64)]] Packet {\n"
                        "  unsigned char tag;\n"
                        "  alignas(16) unsigned value;\n"
                        "  unsigned tail [[gnu::packed]];\n"
                        "};\n"),
                IREE_SV("layout.cpp"), options);
  cxx::ArchiveWriter writer;
  cxx::SemanticArchiveRoots roots;
  roots.globalScope = source.unit().globalScope();
  roots.ast = source.unit().ast();
  cxx::SemanticEncoder encoder(&source.unit());
  ASSERT_TRUE(encoder(roots, writer));
  auto bytes = writer();
  cxx::ArchiveReader reader;
  ASSERT_TRUE(reader(bytes)) << reader.error();

  Source destination(IREE_SV(""), IREE_SV("destination.cpp"), options);
  cxx::SemanticArchiveRoots restored;
  cxx::SemanticDecoder decoder(&destination.unit());
  ASSERT_TRUE(decoder(reader, restored)) << decoder.error();
  auto records = restored.globalScope->find("Packet");
  ASSERT_FALSE(records.begin() == records.end());
  auto* record = cxx::symbol_cast<cxx::ClassSymbol>(*records.begin());
  ASSERT_NE(record, nullptr);
  EXPECT_TRUE(record->isPacked());
  EXPECT_EQ(record->packAlignment(), 0);
  EXPECT_EQ(record->minimumAlignment(), 64);
  EXPECT_EQ(record->sizeInBytes(), 64);
  EXPECT_EQ(record->alignment(), 64);
  auto fields = record->find("value");
  ASSERT_FALSE(fields.begin() == fields.end());
  auto* value = cxx::symbol_cast<cxx::FieldSymbol>(*fields.begin());
  ASSERT_NE(value, nullptr);
  EXPECT_EQ(value->explicitAlignment(), 16);
  ASSERT_TRUE(record->layout()->getFieldInfo(value).has_value());
  EXPECT_EQ(record->layout()->getFieldInfo(value)->offset, 16u);
  auto tails = record->find("tail");
  ASSERT_FALSE(tails.begin() == tails.end());
  auto* tail = cxx::symbol_cast<cxx::FieldSymbol>(*tails.begin());
  ASSERT_NE(tail, nullptr);
  EXPECT_TRUE(tail->isPacked());
}

INSTANTIATE_TEST_SUITE_P(DataModels, LayoutTest,
                         ::testing::Values(LOOM_CXX_DATA_MODEL_LP64,
                                           LOOM_CXX_DATA_MODEL_LLP64,
                                           LOOM_CXX_DATA_MODEL_ILP32));

}  // namespace
}  // namespace loom::cxx_import
