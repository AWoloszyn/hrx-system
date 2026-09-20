// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <string>

#include "iree/testing/gtest.h"
#include "loom/import/cxx/source/error.h"
#include "loom/import/cxx/source/source.h"

namespace loom::cxx_import {
namespace {

class EnumTest : public ::testing::TestWithParam<loom_cxx_data_model_t> {
 protected:
  loom_cxx_import_options_t options() {
    loom_cxx_import_options_t value;
    loom_cxx_import_options_initialize(&value);
    value.data_model = GetParam();
    value.diagnostic_sink = {
        [](void* user_data, const loom_diagnostic_t* diagnostic) {
          if (diagnostic->severity == LOOM_DIAGNOSTIC_ERROR) {
            auto& messages = *static_cast<std::string*>(user_data);
            messages += string(diagnostic->params[0].string) + "\n";
          }
          return iree_ok_status();
        },
        &diagnostics_};
    return value;
  }

  // Source errors retained so assertions report the frontend's diagnosis.
  std::string diagnostics_;
};

TEST_P(EnumTest, InferredStorageAndPromotionsRepresentTheWholeRange) {
  EXPECT_NO_THROW(Source(IREE_SV(R"cpp(
                           enum Empty {};
                           enum Small { zero, one };
                           enum Signed { negative = -1, positive = 2 };
                           enum Unsigned { high = 0x80000000u };
                           enum Wide { wide = 1ULL << 40 };
                           enum Mixed { minimum = -1, maximum = 0xffffffffu };
                           enum Full { full = 0xffffffffffffffffULL };
                           static_assert(__is_same(__underlying_type(Empty), int));
                           static_assert(__is_same(__underlying_type(Small), int));
                           static_assert(__is_same(decltype(+one), int));
                           static_assert(sizeof(Signed) == 4 && negative < 0);
                           static_assert(sizeof(Unsigned) == 4 && high > 0);
                           static_assert(sizeof(Wide) == 8 && wide == (1ULL << 40));
                           static_assert(sizeof(Mixed) == 8 && minimum < 0);
                           static_assert(maximum > 0 && maximum == 0xffffffffULL);
                           static_assert(sizeof(Full) == 8 && full > 0);
                           static_assert(full == 0xffffffffffffffffULL);
                           static_assert(__is_same(decltype(+wide), __underlying_type(Wide)));
                           static_assert(__is_same(decltype(+full), __underlying_type(Full)));
                         )cpp"),
                         IREE_SV("enum.cpp"), options()))
      << diagnostics_;
}

TEST_P(EnumTest, PreliminaryTypesAndImplicitIncrementsPreserveValues) {
  EXPECT_NO_THROW(
      Source(IREE_SV(R"cpp(
               enum UnsignedStep {
                 last = 0xffffffffu,
                 next,
                 check = next == 0x100000000ULL
               };
               enum SignedStep {
                 signed_last = 9223372036854775807LL,
                 signed_next,
                 signed_check = signed_next == 0x8000000000000000ULL
               };
               enum Negative { first = -2, second, third, fourth };
               enum NegativeWide { lowest = -9223372036854775807LL - 1, following_lowest };
               enum Temporary {
                 initial = 0u,
                 unsigned_type = __is_same(decltype(initial), unsigned),
                 following = initial - 1u
               };
               enum WideTemporary {
                 large = 1ULL << 40,
                 following_large = large + 1,
                 large_type = __is_same(decltype(large), unsigned long long)
               };
               static_assert(check == 1 && signed_check == 1);
               static_assert(second == -1 && third == 0 && fourth == 1);
               static_assert(following_lowest == -9223372036854775807LL);
               static_assert(unsigned_type == 1 && following == 0xffffffffu);
               static_assert(large_type == 1);
               static_assert(following_large == (1ULL << 40) + 1);
             )cpp"),
             IREE_SV("enum.cpp"), options()))
      << diagnostics_;
}

TEST_P(EnumTest, FixedStorageChecksValuesBeforeConversion) {
  EXPECT_NO_THROW(Source(
      IREE_SV(R"cpp(
        enum class Byte : unsigned char { first = 254, last };
        enum class SignedByte : signed char { low = -128, high = 127 };
        enum class Short : unsigned short { high = 65535 };
        enum class Word : unsigned { high = 0xffffffffu };
        enum class SignedWord : int { low = -2147483647 - 1 };
        enum class Long : unsigned long long { high = 0xffffffffffffffffULL };
        enum class SignedLong : long long { low = -9223372036854775807LL - 1 };
        enum class Flag : bool { no, yes };
        enum PlainByte : unsigned char { byte = 255 };
        enum Copy : unsigned { copied = byte };
        static_assert(sizeof(Byte) == 1 && int(Byte::last) == 255);
        static_assert(sizeof(SignedByte) == 1 && int(SignedByte::low) == -128);
        static_assert(sizeof(Short) == 2 && unsigned(Short::high) == 65535);
        static_assert(sizeof(Word) == 4 && unsigned(Word::high) == 0xffffffffu);
        static_assert(int(SignedWord::low) == -2147483647 - 1);
        static_assert(sizeof(Long) == 8 &&
                      (unsigned long long)Long::high == 0xffffffffffffffffULL);
        static_assert((long long)SignedLong::low == -9223372036854775807LL - 1);
        static_assert(!bool(Flag::no) && bool(Flag::yes));
        static_assert(__is_same(decltype(+byte), int));
        static_assert(copied == 255);
      )cpp"),
      IREE_SV("enum.cpp"), options()))
      << diagnostics_;
}

TEST_P(EnumTest, DependentValuesAreCheckedAfterSubstitution) {
  EXPECT_NO_THROW(Source(IREE_SV(R"cpp(
                           template <unsigned long long Value>
                           constexpr auto value() {
                             enum Kind { first = Value, next, last = next + 1 };
                             return last;
                           }
                           template <class T, T Value>
                           constexpr T fixed() {
                             enum class Kind : T { first = Value, next };
                             return T(Kind::next);
                           }
                           static_assert(value<1>() == 3);
                           static_assert(value<(1ULL << 40)>() == (1ULL << 40) + 2);
                           static_assert(value<0xffffffffu>() == 0x100000001ULL);
                           static_assert(fixed<unsigned char, 254>() == 255);
                           static_assert(fixed<long long, -2>() == -1);
                         )cpp"),
                         IREE_SV("enum.cpp"), options()))
      << diagnostics_;
}

TEST_P(EnumTest, InvalidDefinitionsProduceSourceDiagnostics) {
  for (auto contents : {
           "enum E { invalid = 2.5 };",
           "int value = 2; enum E { invalid = value };",
           "enum class E : unsigned char { invalid = 256 };",
           "enum class E : signed char { invalid = 128 };",
           "enum class E : signed char { invalid = -129 };",
           "enum class E : unsigned { invalid = -1 };",
           "enum class E : int { invalid = 0x80000000u };",
           "enum class E : long long { invalid = 0x8000000000000000ULL };",
           "enum class E : bool { invalid = 2 };",
           "enum class E : unsigned char { last = 255, invalid };",
           "enum class E : signed char { last = 127, invalid };",
           "enum class E : bool { no, yes, invalid };",
           "enum class E { last = 2147483647, invalid };",
           "enum E { last = 0xffffffffffffffffULL, invalid };",
           "enum E { negative = -1, large = 0xffffffffffffffffULL };",
           "enum class E { first }; enum F { invalid = E::first };",
           "template <unsigned V> constexpr unsigned value() {"
           "  enum class E : unsigned char { first = V, next };"
           "  return unsigned(E::next);"
           "} static_assert(value<255>() == 256);",
       }) {
    SCOPED_TRACE(contents);
    diagnostics_.clear();
    EXPECT_THROW(Source(iree_make_cstring_view(contents), IREE_SV("enum.cpp"),
                        options()),
                 SourceRejected);
    EXPECT_NE(diagnostics_.find("enumerator"), std::string::npos)
        << diagnostics_;
  }
}

TEST_P(EnumTest, CEnumerationsShareCheckedValuesAndLayout) {
  auto source_options = options();
  source_options.standard = IREE_SV("c23");
  EXPECT_NO_THROW(Source(IREE_SV(R"c(
                  enum Kind { first, second, copy = second, next };
                  enum Wide { large = 1ULL << 40, following };
                  enum Byte : unsigned char { byte = 255 };
                  static_assert(first == 0 && second == 1 && next == 2);
                  static_assert(sizeof(enum Wide) == 8);
                  static_assert(following == (1ULL << 40) + 1);
                  static_assert(sizeof(enum Byte) == 1 && byte == 255);
                )c"),
                         IREE_SV("enum.c"), source_options))
      << diagnostics_;
}

INSTANTIATE_TEST_SUITE_P(DataModels, EnumTest,
                         ::testing::Values(LOOM_CXX_DATA_MODEL_LP64,
                                           LOOM_CXX_DATA_MODEL_LLP64,
                                           LOOM_CXX_DATA_MODEL_ILP32));

}  // namespace
}  // namespace loom::cxx_import
