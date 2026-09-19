// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/value/types.h"

#include <cxx/ast.h>
#include <cxx/control.h>
#include <cxx/types.h>

#include "iree/testing/gtest.h"
#include "loom/import/cxx/source/error.h"

namespace loom::cxx_import {
namespace {

TEST(TypesTest, ProjectsTheConfiguredDataModelAndRetainsSignedness) {
  for (auto model : {LOOM_CXX_DATA_MODEL_LP64, LOOM_CXX_DATA_MODEL_LLP64,
                     LOOM_CXX_DATA_MODEL_ILP32}) {
    loom_cxx_import_options_t options;
    loom_cxx_import_options_initialize(&options);
    options.data_model = model;
    Source source(IREE_SV("int entry();"), IREE_SV("types.cpp"), options);
    Types types(source.unit(), source.diagnostics());
    auto* control = source.unit().control();
    auto* owner = source.unit().ast();
    EXPECT_EQ(
        loom_type_element_type(types.get(control->getLongIntType(), owner)),
        model == LOOM_CXX_DATA_MODEL_LP64 ? LOOM_SCALAR_TYPE_I64
                                          : LOOM_SCALAR_TYPE_I32);
    EXPECT_TRUE(
        loom_type_equal(types.get(control->getIntType(), owner),
                        types.get(control->getUnsignedIntType(), owner)));
    EXPECT_FALSE(types.is_unsigned(control->getIntType()));
    EXPECT_TRUE(types.is_unsigned(control->getUnsignedIntType()));
    EXPECT_EQ(loom_type_kind(types.get(
                  control->getPointerType(control->getFloatType()), owner)),
              LOOM_TYPE_BUFFER);
  }
}

TEST(TypesTest, RejectsRepresentationsThatLoseSourceSemantics) {
  loom_cxx_import_options_t options;
  loom_cxx_import_options_initialize(&options);
  Source source(IREE_SV("int entry();"), IREE_SV("types.cpp"), options);
  Types types(source.unit(), source.diagnostics());
  auto* control = source.unit().control();
  auto* owner = source.unit().ast();
  EXPECT_THROW(types.get(control->getQualType(control->getIntType(),
                                              cxx::CvQualifiers::kVolatile),
                         owner),
               SourceRejected);
  EXPECT_THROW(
      types.get(control->getPointerType(control->getBoolType()), owner),
      SourceRejected);
  EXPECT_THROW(
      types.get(control->getLvalueReferenceType(control->getIntType()), owner),
      SourceRejected);
  EXPECT_THROW(types.get(control->getLongDoubleType(), owner), SourceRejected);
}

TEST(TypesTest, VectorProjectionKeepsLaneShapeAndRejectsPackedBoolAndPadding) {
  loom_cxx_import_options_t options;
  loom_cxx_import_options_initialize(&options);
  Source source(IREE_SV("int entry();"), IREE_SV("vectors.cpp"), options);
  Types types(source.unit(), source.diagnostics());
  auto* control = source.unit().control();
  auto* owner = source.unit().ast();
  auto* vector = control->getVectorType(control->getUnsignedIntType(), 16,
                                        cxx::VectorKind::kGnu);
  EXPECT_TRUE(loom_type_equal(
      types.get(vector, owner),
      loom_type_shaped_1d(LOOM_TYPE_VECTOR, LOOM_SCALAR_TYPE_I32, 16, 0)));
  EXPECT_EQ(loom_type_kind(types.get(control->getPointerType(vector), owner)),
            LOOM_TYPE_BUFFER);
  EXPECT_THROW(types.get(control->getVectorType(control->getBoolType(), 16,
                                                cxx::VectorKind::kExt),
                         owner),
               SourceRejected);
  EXPECT_THROW(types.get(control->getVectorType(control->getFloatType(), 3,
                                                cxx::VectorKind::kExt),
                         owner),
               SourceRejected);
}

TEST(TypesTest, MutationUsesTheObjectQualifierInsteadOfItsPointee) {
  loom_cxx_import_options_t options;
  loom_cxx_import_options_initialize(&options);
  Source source(IREE_SV("int entry();"), IREE_SV("types.cpp"), options);
  Types types(source.unit(), source.diagnostics());
  auto* control = source.unit().control();
  auto* owner = source.unit().ast();
  auto* constant =
      control->getQualType(control->getIntType(), cxx::CvQualifiers::kConst);
  auto* pointer = control->getPointerType(control->getIntType());
  EXPECT_THROW(types.require_mutable(constant, owner), SourceRejected);
  EXPECT_THROW(
      types.require_mutable(
          control->getQualType(pointer, cxx::CvQualifiers::kConst), owner),
      SourceRejected);
  EXPECT_NO_THROW(types.require_mutable(control->getIntType(), owner));
  EXPECT_NO_THROW(
      types.require_mutable(control->getPointerType(constant), owner));
}

}  // namespace
}  // namespace loom::cxx_import
