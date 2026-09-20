// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/binding/assumptions.h"

#include <cxx/ast.h>
#include <cxx/names.h>
#include <cxx/symbols.h>

#include "iree/testing/gtest.h"

namespace loom::cxx_import {
namespace {

cxx::CallExpressionAST* source_call(Source& source) {
  auto* root = cxx::ast_cast<cxx::TranslationUnitAST>(source.unit().ast());
  for (auto* declaration : cxx::ListView{root->declarationList}) {
    if (auto* function =
            cxx::ast_cast<cxx::FunctionDefinitionAST>(declaration)) {
      auto* body = cxx::ast_cast<cxx::CompoundStatementFunctionBodyAST>(
                       function->functionBody)
                       ->statement;
      auto* statement = cxx::ast_cast<cxx::ExpressionStatementAST>(
          body->statementList->value);
      return cxx::ast_cast<cxx::CallExpressionAST>(statement->expression);
    }
  }
  return nullptr;
}

TEST(AssumptionsTest, ConjunctionRetainsBindingsPromotionsAndConstantBounds) {
  loom_cxx_import_options_t options;
  loom_cxx_import_options_initialize(&options);
  Source source(
      IREE_SV(R"cpp(
        [[loom::assume]] void assume(bool);
        constexpr unsigned capacity = 28672;
        enum { stride = 16 };
        void entry(unsigned count, unsigned char byte, unsigned long long wide) {
          assume(
              ((count <
                ((capacity / sizeof(unsigned) - 16u - 320u) / stride + 1u))) &&
              ((byte) < 256u && count < 256u) &&
              wide < static_cast<unsigned char>(272u));
        }
      )cpp"),
      IREE_SV("bounds.cpp"), options);
  auto* call = source_call(source);
  ASSERT_NE(call, nullptr);
  auto bounds = assumption_bounds(source.unit(), source.diagnostics(), call);
  ASSERT_EQ(bounds.size(), 4u);
  EXPECT_EQ(bounds[0].upper_bound, 428);
  EXPECT_EQ(bounds[1].upper_bound, 256);
  EXPECT_EQ(bounds[2].upper_bound, 256);
  EXPECT_EQ(bounds[3].upper_bound, 16);
  EXPECT_EQ(cxx::to_string(bounds[0].binding->symbol->name()), "count");
  EXPECT_EQ(cxx::to_string(bounds[1].binding->symbol->name()), "byte");
  EXPECT_EQ(bounds[0].binding->symbol, bounds[2].binding->symbol);
  auto traits = source.unit().typeTraits();
  EXPECT_EQ(traits.integral_representation(bounds[1].binding->type)->bits, 8);
  EXPECT_EQ(traits.integral_representation(bounds[1].value->type)->bits, 32);
  EXPECT_EQ(traits.integral_representation(bounds[3].value->type)->bits, 64);
}

}  // namespace
}  // namespace loom::cxx_import
