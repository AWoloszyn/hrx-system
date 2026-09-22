// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/source/constants.h"

#include <cxx/ast.h>
#include <cxx/ast_interpreter.h>
#include <cxx/names.h>
#include <cxx/symbols.h>

#include <cmath>

#include "iree/testing/gtest.h"
#include "loom/import/cxx/source/source.h"

namespace loom::cxx_import {
namespace {

cxx::ExpressionAST* returned(Source& source) {
  auto* root = cxx::ast_cast<cxx::TranslationUnitAST>(source.unit().ast());
  for (auto* declaration : cxx::ListView{root->declarationList}) {
    if (auto* function =
            cxx::ast_cast<cxx::FunctionDefinitionAST>(declaration)) {
      if (cxx::to_string(function->symbol->name()) != "entry") {
        continue;
      }
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
        "0xffffffffffffffffULL", "unsigned_enum_max"}) {
    SCOPED_TRACE(expression);
    std::string text =
        "unsigned opaque(); constexpr unsigned constant_call() { return 16; } "
        "constexpr unsigned operator\"\"_words(unsigned long long) { return "
        "16; } "
        "enum : unsigned long long { unsigned_enum_max = "
        "0xffffffffffffffffULL }; "
        "auto entry(unsigned value, volatile unsigned flag) { return " +
        std::string(expression) + "; }";
    Source source(view(text), IREE_SV("rejected.cpp"), options);
    auto* result = returned(source);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(integer_constant(source.unit(), result), std::nullopt);
  }
}

TEST(IntegerConstantTest, PreservesNegativeSignedValues) {
  loom_cxx_import_options_t options;
  loom_cxx_import_options_initialize(&options);
  for (auto expression : {"-1", "negative_enum", "Signed::negative"}) {
    SCOPED_TRACE(expression);
    std::string text =
        "enum { negative_enum = -1 }; "
        "enum class Signed : long long { negative = -1 }; "
        "auto entry() { return " +
        std::string(expression) + "; }";
    Source source(view(text), IREE_SV("signed.cpp"), options);
    auto* result = returned(source);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(integer_constant(source.unit(), result), -1);
  }
}

TEST(IntegerConstantTest, ZeroExtendsUnsignedComplementAtSourceWidth) {
  loom_cxx_import_options_t options;
  loom_cxx_import_options_initialize(&options);
  for (auto expression : {"~0u", "static_cast<unsigned long long>(~0u)",
                          "~0xffffffff00000000ULL"}) {
    SCOPED_TRACE(expression);
    std::string text =
        "auto entry() { return " + std::string(expression) + "; }";
    Source source(view(text), IREE_SV("complement.cpp"), options);
    auto* result = returned(source);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(integer_constant(source.unit(), result), INT64_C(0xffffffff));
  }
}

TEST(FloatingConstantTest, Float16RoundsAtSourceConversionBoundaries) {
  loom_cxx_import_options_t options;
  loom_cxx_import_options_initialize(&options);
  const struct {
    // Source expression evaluated through the production frontend.
    const char* expression;
    // Independently specified binary16 result, exactly represented in double.
    double expected;
  } cases[] = {
      {"(_Float16)1.00048828125", 1.0},
      {"(_Float16)1.00146484375", 1.001953125},
      {"(_Float16)0x1.0020000000001p0", 1.0009765625},
      {"(_Float16)0x1p-25", 0.0},
      {"(_Float16)0x1.8p-24", 0x1p-23},
      {"(_Float16)0x1.ffcp-15", 0x1p-14},
      {"(_Float16)65519.0", 65504.0},
      {"(_Float16)-0.0", -0.0},
      {"1.00048828125f16", 1.0},
      {"1.00146484375F16", 1.001953125},
      {"+((_Float16)1.0 + (_Float16)0x1p-11)", 1.0},
      {"-((_Float16)1.5 * (_Float16)2.0)", -3.0},
      {"(_Float16)1.0 / (_Float16)3.0", 0.333251953125},
      {"(float)(_Float16)1.00048828125", 1.0},
      {"(_Float16)4095u", 4096.0},
  };
  for (const auto& test : cases) {
    SCOPED_TRACE(test.expression);
    std::string text =
        "auto entry() { return " + std::string(test.expression) + "; }";
    Source source(view(text), IREE_SV("float16.cpp"), options);
    auto* result = returned(source);
    ASSERT_NE(result, nullptr);
    auto value = scalar_constant(source.unit(), result);
    ASSERT_TRUE(value.has_value());
    cxx::ASTInterpreter interpreter(&source.unit());
    auto number = interpreter.toDouble(*value);
    ASSERT_TRUE(number.has_value());
    EXPECT_EQ(*number, test.expected);
    EXPECT_EQ(std::signbit(*number), std::signbit(test.expected));
  }
}

}  // namespace
}  // namespace loom::cxx_import
