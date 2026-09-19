// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/binding/shaped.h"

#include "loom/import/cxx/binding/declaration_test.h"
#include "loom/import/cxx/source/error.h"

namespace loom::cxx_import {
namespace {

class ShapedTest : public ValueBuilderTest {
 protected:
  std::optional<ShapedIntrinsic> resolve(Source& source) {
    auto declared = declarations(source, "custom");
    EXPECT_EQ(declared.size(), 1u);
    const auto& declaration = declared[0];
    Types types(source.unit(), source.diagnostics());
    return ShapedIntrinsic::resolve(
        source.unit(), source.diagnostics(), types,
        cxx::type_cast<cxx::FunctionType>(declaration.function->type()),
        (*declaration.function->attributes())[0], declaration.owner);
  }

  loom_value_id_t constant(loom_scalar_type_t element, int64_t lanes) {
    loom_op_t* op;
    check(loom_vector_constant_build(
        &builder_, loom_attr_i64(0),
        loom_type_shaped_1d(LOOM_TYPE_VECTOR, element, lanes, 0),
        LOOM_LOCATION_UNKNOWN, &op));
    return loom_vector_constant_result(op);
  }
};

TEST_F(ShapedTest, LookupRetainsDifferentTableAndIndexShapes) {
  Source source(IREE_SV(R"(
    typedef unsigned Table __attribute__((vector_size(64)));
    typedef unsigned Result __attribute__((vector_size(16)));
    typedef unsigned short Indices __attribute__((vector_size(8)));
    [[loom::op("vector.table.lookup")]] Result custom(Table, Indices);
  )"),
                IREE_SV("lookup.cpp"), options());
  auto binding = resolve(source);
  ASSERT_TRUE(binding);
  loom_value_id_t operands[] = {constant(LOOM_SCALAR_TYPE_I32, 16),
                                constant(LOOM_SCALAR_TYPE_I16, 4)};
  auto value = binding->call(operands, &builder_, LOOM_LOCATION_UNKNOWN);
  auto* op = producer(value);
  EXPECT_TRUE(loom_vector_table_lookup_isa(op));
  EXPECT_EQ(loom_vector_table_lookup_table(op), operands[0]);
  EXPECT_EQ(loom_vector_table_lookup_indices(op), operands[1]);
  EXPECT_TRUE(loom_type_equal(
      loom_module_value_type(module_, value),
      loom_type_shaped_1d(LOOM_TYPE_VECTOR, LOOM_SCALAR_TYPE_I32, 4, 0)));
}

TEST_F(ShapedTest, DotRetainsEverySignednessCombinationAndWidenedResult) {
  for (const auto* kind : {"s8s8", "u8s8", "s8u8", "u8u8"}) {
    SCOPED_TRACE(kind);
    std::string text =
        "typedef signed char S __attribute__((vector_size(16)));"
        "typedef unsigned char U __attribute__((vector_size(16)));"
        "typedef int A __attribute__((vector_size(16)));"
        "[[loom::op(\"vector.dot4i\", \"";
    text += kind;
    text += "\")]] A custom(";
    text += kind[0] == 'u' ? "U," : "S,";
    text += kind[2] == 'u' ? "U,A);" : "S,A);";
    Source source(view(text), IREE_SV("dot.cpp"), options());
    auto binding = resolve(source);
    ASSERT_TRUE(binding);
    loom_value_id_t operands[] = {constant(LOOM_SCALAR_TYPE_I8, 16),
                                  constant(LOOM_SCALAR_TYPE_I8, 16),
                                  constant(LOOM_SCALAR_TYPE_I32, 4)};
    auto value = binding->call(operands, &builder_, LOOM_LOCATION_UNKNOWN);
    auto* op = producer(value);
    EXPECT_TRUE(loom_vector_dot4i_isa(op));
    EXPECT_EQ(loom_vector_dot4i_kind(op),
              (kind[0] == 'u' ? 1 : 0) | (kind[2] == 'u' ? 2 : 0));
    EXPECT_TRUE(loom_type_equal(loom_module_value_type(module_, value),
                                loom_module_value_type(module_, operands[2])));
  }
}

TEST_F(ShapedTest, InvalidSourceContractsRejectBeforeEmission) {
  const std::string aliases =
      "typedef signed char S __attribute__((vector_size(16)));"
      "typedef signed char T __attribute__((vector_size(8)));"
      "typedef unsigned char U __attribute__((vector_size(16)));"
      "typedef int I __attribute__((vector_size(16)));"
      "typedef unsigned R __attribute__((vector_size(16)));"
      "typedef float F __attribute__((vector_size(16)));";
  for (const char* declaration : {
           "[[loom::op(\"vector.table.lookup\")]] int custom(I,I);",
           "[[loom::op(\"vector.table.lookup\")]] I custom(int,I);",
           "[[loom::op(\"vector.table.lookup\")]] I custom(I,F);",
           "[[loom::op(\"vector.table.lookup\")]] I custom(I,S);",
           "[[loom::op(\"vector.table.lookup\")]] I custom(R,I);",
           "[[loom::op(\"vector.table.lookup\")]] I custom(I);",
           "[[loom::op(\"vector.table.lookup\")]] I custom(I,I,...);",
           "[[loom::op(\"vector.table.lookup\",\"afn\")]] I custom(I,I);",
           "[[loom::op(\"vector.dot4i\")]] I custom(S,S,I);",
           "[[loom::op(\"vector.dot4i\",\"unknown\")]] I custom(S,S,I);",
           "[[loom::op(\"vector.dot4i\",\"s8s8\",\"afn\")]] I custom(S,S,I);",
           "[[loom::op(\"vector.dot4i\",\"s8s8\")]] I custom(I,I,I);",
           "[[loom::op(\"vector.dot4i\",\"s8s8\")]] I custom(S,T,I);",
           "[[loom::op(\"vector.dot4i\",\"s8s8\")]] I custom(T,T,I);",
           "[[loom::op(\"vector.dot4i\",\"s8s8\")]] F custom(S,S,F);",
           "[[loom::op(\"vector.dot4i\",\"s8s8\")]] I custom(S,S,R);",
           "[[loom::op(\"vector.dot4i\",\"s8s8\")]] I custom(U,S,I);",
           "[[loom::op(\"vector.dot4i\",\"u8u8\")]] I custom(U,S,I);",
           "[[loom::op(\"vector.dot4i\",\"s8s8\")]] I custom(S,S);",
       }) {
    SCOPED_TRACE(declaration);
    std::string text = aliases + declaration;
    Source source(view(text), IREE_SV("invalid.cpp"), options());
    EXPECT_THROW(resolve(source), SourceRejected);
  }
}

TEST_F(ShapedTest, OtherOperationsRemainUnclaimed) {
  Source source(IREE_SV("[[loom::op(\"scalar.expf\")]] float custom(float);"),
                IREE_SV("scalar.cpp"), options());
  EXPECT_FALSE(resolve(source));
}

}  // namespace
}  // namespace loom::cxx_import
