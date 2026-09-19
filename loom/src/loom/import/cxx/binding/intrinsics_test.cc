// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/binding/intrinsics.h"

#include "loom/import/cxx/binding/declaration_test.h"
#include "loom/import/cxx/source/error.h"
#include "loom/ops/scalar/ops.h"

namespace loom::cxx_import {
namespace {
using IntrinsicsTest = ValueBuilderTest;

TEST_F(IntrinsicsTest, CallsUseAnAdmittedBindingAndItsExplicitMathFlags) {
  Source source(
      IREE_SV("[[loom::op(\"scalar.expf\", \"afn\")]] float custom(float);"),
      IREE_SV("intrinsics.cpp"), options());
  auto declared = declarations(source, "custom");
  ASSERT_EQ(declared.size(), 1u);
  const auto& declaration = declared[0];
  Intrinsics intrinsics(source.unit(), source.diagnostics());
  intrinsics.declaration(declaration.function, declaration.attributes,
                         declaration.owner);
  loom_op_t* constant;
  IREE_ASSERT_OK(loom_scalar_constant_build(
      &builder_, loom_attr_f64(1.0), loom_type_scalar(LOOM_SCALAR_TYPE_F32),
      LOOM_LOCATION_UNKNOWN, &constant));
  auto operand = loom_op_results(constant)[0];
  auto value = intrinsics.call(declaration.function, {&operand, 1}, 0,
                               &builder_, LOOM_LOCATION_UNKNOWN);
  ASSERT_TRUE(value.has_value());
  EXPECT_TRUE(loom_scalar_expf_isa(producer(*value)));
  EXPECT_EQ(loom_scalar_expf_fastmath(producer(*value)),
            LOOM_SCALAR_FASTMATHFLAGS_AFN);
}

TEST_F(IntrinsicsTest, RejectsADeclarationWhoseTypesLoseTheOperationContract) {
  Source source(IREE_SV("[[loom::op(\"scalar.expf\")]] float custom(double);"),
                IREE_SV("intrinsics.cpp"), options());
  auto declared = declarations(source, "custom");
  ASSERT_EQ(declared.size(), 1u);
  const auto& declaration = declared[0];
  Intrinsics intrinsics(source.unit(), source.diagnostics());
  EXPECT_THROW(
      intrinsics.declaration(declaration.function, declaration.attributes,
                             declaration.owner),
      SourceRejected);
}

}  // namespace
}  // namespace loom::cxx_import
