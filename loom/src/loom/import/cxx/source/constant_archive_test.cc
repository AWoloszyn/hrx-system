// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <cxx/archive.h>
#include <cxx/ast.h>
#include <cxx/control.h>
#include <cxx/private/semantic_codec.h>
#include <cxx/symbols.h>
#include <cxx/views/symbols.h>

#include <array>

#include "iree/testing/gtest.h"
#include "loom/import/cxx/source/source.h"

namespace loom::cxx_import {
namespace {

TEST(ConstantArchiveTest, PreservesComplexAndIndeterminateValues) {
  loom_cxx_import_options_t options;
  loom_cxx_import_options_initialize(&options);
  const std::array<cxx::ConstValue, 2> values = {
      std::make_shared<cxx::ConstComplex>(1.25f, -2.5f),
      cxx::IndeterminateValue{},
  };
  for (const auto& value : values) {
    SCOPED_TRACE(value.index());
    std::vector<std::uint8_t> bytes;
    {
      Source source(IREE_SV("int constant;"), IREE_SV("constant.cxx"), options);
      auto symbols = source.unit().globalScope()->find("constant");
      ASSERT_FALSE(symbols.begin() == symbols.end());
      auto* variable = cxx::symbol_cast<cxx::VariableSymbol>(*symbols.begin());
      ASSERT_NE(variable, nullptr);
      if (std::holds_alternative<std::shared_ptr<cxx::ConstComplex>>(value)) {
        variable->setType(source.unit().control()->getComplexType(
            source.unit().control()->getFloatType()));
      }
      variable->setConstValue(value);
      cxx::ArchiveWriter writer;
      cxx::SemanticArchiveRoots roots;
      roots.ast = source.unit().ast();
      roots.globalScope = source.unit().globalScope();
      cxx::SemanticEncoder encoder(&source.unit());
      ASSERT_TRUE(encoder(roots, writer));
      bytes = writer();
    }
    Source destination(IREE_SV(""), IREE_SV("restored.cxx"), options);
    cxx::ArchiveReader reader;
    ASSERT_TRUE(reader(bytes)) << reader.error();
    cxx::SemanticArchiveRoots restored;
    cxx::SemanticDecoder decoder(&destination.unit());
    ASSERT_TRUE(decoder(reader, restored)) << decoder.error();
    auto symbols = restored.globalScope->find("constant");
    ASSERT_FALSE(symbols.begin() == symbols.end());
    auto* variable = cxx::symbol_cast<cxx::VariableSymbol>(*symbols.begin());
    ASSERT_NE(variable, nullptr);
    ASSERT_TRUE(variable->constValue());
    ASSERT_EQ(variable->constValue()->index(), value.index());
    if (auto* complex = std::get_if<std::shared_ptr<cxx::ConstComplex>>(
            &*variable->constValue())) {
      ASSERT_NE(*complex, nullptr);
      EXPECT_EQ(std::get<float>((*complex)->real()), 1.25f);
      EXPECT_EQ(std::get<float>((*complex)->imag()), -2.5f);
    }
  }
}

}  // namespace
}  // namespace loom::cxx_import
