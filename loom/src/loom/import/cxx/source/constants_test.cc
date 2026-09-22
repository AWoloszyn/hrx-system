// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/source/constants.h"

#include <cxx/ast.h>
#include <cxx/ast_interpreter.h>
#include <cxx/control.h>
#include <cxx/names.h>
#include <cxx/symbols.h>
#include <cxx/types.h>

#include <cmath>
#include <limits>

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

TEST(FloatingConstantTest, NarrowFormatsRoundAtSourceConversionBoundaries) {
  loom_cxx_import_options_t options;
  loom_cxx_import_options_initialize(&options);
  const struct {
    // Source expression evaluated through the production frontend.
    const char* expression;
    // Independently specified narrow result, exactly represented in double.
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
      {"(__bf16)1.00390625", 1.0},
      {"(__bf16)1.01171875", 1.015625},
      {"(__bf16)0x1.0100000000001p0", 1.0078125},
      {"(__bf16)0x1p-134", 0.0},
      {"(__bf16)0x1.8p-133", 0x1p-132},
      {"(__bf16)0x1.fep-127", 0x1p-126},
      {"(__bf16)0x1.fep127", 0x1.fep127},
      {"(__bf16)-0.0", -0.0},
      {"1.00390625bf16", 1.0},
      {"1.01171875BF16", 1.015625},
      {"+((__bf16)1.0 + (__bf16)0x1p-8)", 1.0},
      {"-((__bf16)1.5 * (__bf16)2.0)", -3.0},
      {"(__bf16)1.0 / (__bf16)3.0", 0.333984375},
      {"(float)(__bf16)1.00390625", 1.0},
      {"(__bf16)511u", 512.0},
      {"(__bf16)0xffffffffffffffffULL", 0x1p64},
      {"(_Float16)(__bf16)1.00390625", 1.0},
      {"(__bf16)(_Float16)1.0048828125", 1.0078125},
      {"(_Float16)65520.0", std::numeric_limits<double>::infinity()},
      {"(__bf16)0x1.ffp127", std::numeric_limits<double>::infinity()},
      {"(__float8_e4m3fn)1.0625", 1.0},
      {"(__float8_e4m3fn)1.1875", 1.25},
      {"(__float8_e4m3fn)0x1.1000000000001p0", 1.125},
      {"(__float8_e4m3fn)0x1p-10", 0.0},
      {"(__float8_e4m3fn)0x1.8p-9", 0x1p-8},
      {"(__float8_e4m3fn)0x1.ep-7", 0x1p-6},
      {"(__float8_e4m3fn)1000.0", 448.0},
      {"(__float8_e4m3fn)-0.0", -0.0},
      {"+((__float8_e4m3fn)1.0 + (__float8_e4m3fn)0.0625)", 1.0},
      {"-((__float8_e4m3fn)1.5 * (__float8_e4m3fn)2.0)", -3.0},
      {"(__float8_e4m3fn)1.0 / (__float8_e4m3fn)3.0", 0.34375},
      {"(__float8_e4m3fn)0xffffffffffffffffULL", 448.0},
      {"(__float8_e5m2)1.125", 1.0},
      {"(__float8_e5m2)1.375", 1.5},
      {"(__float8_e5m2)0x1.2000000000001p0", 1.25},
      {"(__float8_e5m2)0x1p-17", 0.0},
      {"(__float8_e5m2)0x1.8p-16", 0x1p-15},
      {"(__float8_e5m2)0x1.cp-15", 0x1p-14},
      {"(__float8_e5m2)61439.0", 57344.0},
      {"(__float8_e5m2)61440.0", std::numeric_limits<double>::infinity()},
      {"(__float8_e5m2)-0.0", -0.0},
      {"(__float8_e5m2)1.0 / (__float8_e5m2)3.0", 0.3125},
      {"(__float8_e5m2)(__float8_e4m3fn)1.125", 1.0},
      {"(__float8_e4m3fn)(__float8_e5m2)512.0", 448.0},
  };
  for (const auto& test : cases) {
    SCOPED_TRACE(test.expression);
    std::string text =
        "auto entry() { return " + std::string(test.expression) + "; }";
    Source source(view(text), IREE_SV("narrow_float.cpp"), options);
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

TEST(FloatingConstantTest, NarrowFormatsRetainInfinityAndNaN) {
  loom_cxx_import_options_t options;
  loom_cxx_import_options_initialize(&options);
  Source source(IREE_SV(""), IREE_SV("special.cpp"), options);
  cxx::ASTInterpreter interpreter(&source.unit());
  const cxx::Type* types[] = {source.unit().control()->getFloat16Type(),
                              source.unit().control()->getBFloat16Type()};
  for (auto* type : types) {
    for (double input : {std::numeric_limits<double>::infinity(),
                         -std::numeric_limits<double>::infinity(),
                         std::numeric_limits<double>::quiet_NaN()}) {
      auto value = interpreter.toArithmeticType(cxx::ConstValue{input}, type);
      ASSERT_TRUE(value.has_value());
      auto result = interpreter.toDouble(*value);
      ASSERT_TRUE(result.has_value());
      if (std::isnan(input)) {
        EXPECT_TRUE(std::isnan(*result));
      } else {
        EXPECT_EQ(*result, input);
      }
    }
  }
}

TEST(FloatingConstantTest, Float8RoundingMatchesEveryFiniteInterval) {
  loom_cxx_import_options_t options;
  loom_cxx_import_options_initialize(&options);
  Source source(IREE_SV(""), IREE_SV("float8.cxx"), options);
  cxx::ASTInterpreter interpreter(&source.unit());
  const struct {
    // Canonical source format consumed by the production interpreter.
    const cxx::Type* type;
    // Number of encoded fraction bits.
    unsigned fraction_bits;
    // Bias of the encoded normal exponent.
    int exponent_bias;
    // Largest positive finite payload, excluding NaNs and infinity.
    unsigned maximum_finite;
    // Expected conversion of positive infinity under this format's policy.
    double infinity;
  } formats[] = {
      {source.unit().control()->getFloat8E4M3FNType(), 3, 7, 0x7e, 448.0},
      {source.unit().control()->getFloat8E5M2Type(), 2, 15, 0x7b,
       std::numeric_limits<double>::infinity()},
  };
  for (const auto& format : formats) {
    SCOPED_TRACE(cxx::to_string(format.type));
    // Decode the finite value set directly from the format, independently of
    // either the frontend's rounding helper or Loom's runtime conversion.
    auto decode = [&](unsigned payload) {
      auto exponent = payload >> format.fraction_bits;
      auto fraction = payload & ((1u << format.fraction_bits) - 1);
      return std::ldexp(
          exponent ? (1u << format.fraction_bits) + fraction : fraction,
          static_cast<int>(exponent ? exponent : 1) - format.exponent_bias -
              static_cast<int>(format.fraction_bits));
    };
    auto expect = [&](double input, double expected) {
      auto value =
          interpreter.toArithmeticType(cxx::ConstValue{input}, format.type);
      ASSERT_TRUE(value.has_value());
      auto actual = interpreter.toDouble(*value);
      ASSERT_TRUE(actual.has_value());
      if (std::isnan(expected)) {
        EXPECT_TRUE(std::isnan(*actual));
      } else {
        EXPECT_EQ(*actual, expected) << "input = " << input;
        EXPECT_EQ(std::signbit(*actual), std::signbit(expected));
      }
    };
    for (double sign : {1.0, -1.0}) {
      for (unsigned payload = 0; payload <= format.maximum_finite; ++payload) {
        SCOPED_TRACE(payload);
        double lower = decode(payload);
        expect(sign * lower, sign * lower);
        if (payload == format.maximum_finite) {
          continue;
        }
        double upper = decode(payload + 1);
        double midpoint = (lower + upper) / 2.0;
        expect(sign * std::nextafter(midpoint, lower), sign * lower);
        expect(sign * midpoint, sign * (payload & 1 ? upper : lower));
        expect(sign * std::nextafter(midpoint, upper), sign * upper);
      }
      expect(sign * std::numeric_limits<double>::infinity(),
             sign * format.infinity);
      expect(sign * std::numeric_limits<double>::quiet_NaN(),
             std::numeric_limits<double>::quiet_NaN());
    }
  }
}

}  // namespace
}  // namespace loom::cxx_import
