// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/source/constants.h"

#include <cxx/ast.h>

#include "iree/testing/gtest.h"
#include "loom/import/cxx/source/source.h"

namespace loom::cxx_import {
namespace {

cxx::ExpressionAST* returned(Source& source) {
  auto* root = cxx::ast_cast<cxx::TranslationUnitAST>(source.unit().ast());
  for (auto* declaration : cxx::ListView{root->declarationList}) {
    if (auto* function =
            cxx::ast_cast<cxx::FunctionDefinitionAST>(declaration)) {
      auto* body = cxx::ast_cast<cxx::CompoundStatementFunctionBodyAST>(
                       function->functionBody)
                       ->statement;
      return cxx::ast_cast<cxx::ReturnStatementAST>(body->statementList->value)
          ->expression;
    }
  }
  return nullptr;
}

TEST(IntegerConstantTest, PreservesSourceArithmeticAndUnevaluatedOperands) {
  loom_cxx_import_options_t options;
  loom_cxx_import_options_initialize(&options);
  for (auto expression :
       {"16u", "(1u << 4)", "(0xffffffffu + 17u)", "(unsigned char)272u",
        "unsigned(16)", "sizeof(++value) * 4u", "alignof(unsigned) * 4u",
        "((~0u & 15u) + 1u)", "(true ? 16 : 8)", "capacity / stride",
        "static_cast<unsigned>(0x100000010ULL)", "width * 4u"}) {
    SCOPED_TRACE(expression);
    std::string text =
        "constexpr unsigned capacity = 320; enum { stride = 20 }; "
        "const unsigned width = 4; auto entry(unsigned value) { return " +
        std::string(expression) + "; }";
    Source source(view(text), IREE_SV("constants.cpp"), options);
    auto* result = returned(source);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(integer_constant(source.unit(), result), 16);
  }
}

TEST(IntegerConstantTest,
     RejectsRuntimeReadsCallsMutationAndInvalidArithmetic) {
  loom_cxx_import_options_t options;
  loom_cxx_import_options_initialize(&options);
  for (auto expression :
       {"value", "flag", "(false ? value : 16u)", "opaque()", "constant_call()",
        "(false ? constant_call() : 16u)", "16_words", "(opaque(), 16u)",
        "++value", "(value = 16u)", "16.5f", "(1u / 0u)", "(1u << 32)",
        "(1u << (1ULL << 32))", "(2147483647 + 1)", "(-2147483647 - 1) / -1",
        "0xffffffffffffffffULL"}) {
    SCOPED_TRACE(expression);
    std::string text =
        "unsigned opaque(); constexpr unsigned constant_call(); "
        "constexpr unsigned operator\"\"_words(unsigned long long); "
        "auto entry(unsigned value, volatile unsigned flag) { return " +
        std::string(expression) + "; }";
    Source source(view(text), IREE_SV("rejected.cpp"), options);
    auto* result = returned(source);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(integer_constant(source.unit(), result), std::nullopt);
  }
}

}  // namespace
}  // namespace loom::cxx_import
