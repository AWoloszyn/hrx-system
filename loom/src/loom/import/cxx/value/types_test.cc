// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/value/types.h"

#include <cxx/ast.h>
#include <cxx/control.h>
#include <cxx/symbols.h>
#include <cxx/types.h>
#include <cxx/views/symbol_chain.h>

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

TEST(TypesTest, RecordPartitionsRetainNominalMembersAndStaticTransport) {
  loom_cxx_import_options_t options;
  loom_cxx_import_options_initialize(&options);
  Source source(IREE_SV("struct Empty {}; struct Pair { unsigned a, b; }; "
                        "struct Other { unsigned a, b; }; "
                        "struct Packet { Empty empty; Pair pair; "
                        "const unsigned* pointer; const bool valid; };"),
                IREE_SV("records.cpp"), options);
  Types types(source.unit(), source.diagnostics());
  auto* owner = source.unit().ast();
  auto source_type = [&](const char* name) {
    return (*source.unit().globalScope()->find(name).begin())->type();
  };
  const auto* packet = types.record(source_type("Packet"), owner);
  ASSERT_NE(packet, nullptr);
  ASSERT_EQ(packet->members.size(), 4u);
  EXPECT_EQ(packet->component_count, 5u);
  EXPECT_EQ(packet->members[0].partition->component_count, 0u);
  EXPECT_EQ(packet->members[1].component_offset, 0u);
  EXPECT_EQ(packet->members[2].component_offset, 2u);
  EXPECT_EQ(packet->members[3].component_offset, 4u);
  EXPECT_EQ(types.member(packet->members[2].field, owner).component_offset, 2u);
  EXPECT_EQ(packet->component_names,
            (std::vector<std::string>{"pair_a", "pair_b", "pointer",
                                      "pointer_byte_offset", "valid"}));
  EXPECT_NE(packet->members[1].partition,
            &types.partition(source_type("Other"), owner));
  EXPECT_EQ(types.partition(source_type("Pair"), owner).kind,
            ValueKind::Record);
  EXPECT_EQ(packet->members[2].partition->kind, ValueKind::Pointer);
  EXPECT_TRUE(
      source.unit().typeTraits().is_const(packet->members[3].field->type()));
  EXPECT_EQ(types.record(source_type("Empty"), owner)->source->sizeInBytes(),
            1);
  std::vector<loom_type_t> signature;
  types.append(source_type("Packet"), owner, signature);
  ASSERT_EQ(signature.size(), 5u);
  EXPECT_EQ(loom_type_element_type(signature[0]), LOOM_SCALAR_TYPE_I32);
  EXPECT_EQ(loom_type_kind(signature[2]), LOOM_TYPE_BUFFER);
  EXPECT_EQ(loom_type_element_type(signature[3]), LOOM_SCALAR_TYPE_OFFSET);
  EXPECT_EQ(loom_type_element_type(signature[4]), LOOM_SCALAR_TYPE_I1);
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

TEST(TypesTest, EnumProjectionPreservesSourceWidthSignednessAndIdentity) {
  struct Case {
    // Complete source definition used to establish the enum representation.
    const char* source;
    // Scalar storage after projection into Loom.
    loom_scalar_type_t element;
    // Source interpretation of the projected integer bits.
    bool is_unsigned;
  };
  for (auto test : {
           Case{"enum class Value : unsigned char { high = 255 };",
                LOOM_SCALAR_TYPE_I8, true},
           Case{"enum class Value : signed char { low = -128 };",
                LOOM_SCALAR_TYPE_I8, false},
           Case{"enum class Value : unsigned short { high = 65535 };",
                LOOM_SCALAR_TYPE_I16, true},
           Case{"enum Value { negative = -1, positive = 2 };",
                LOOM_SCALAR_TYPE_I32, false},
           Case{"enum Value { high = 0x80000000u };", LOOM_SCALAR_TYPE_I32,
                true},
           Case{"enum Value { high = 1ULL << 40 };", LOOM_SCALAR_TYPE_I64,
                false},
           Case{"enum Value { high = 0xffffffffffffffffULL };",
                LOOM_SCALAR_TYPE_I64, true},
       }) {
    SCOPED_TRACE(test.source);
    loom_cxx_import_options_t options;
    loom_cxx_import_options_initialize(&options);
    Source source(iree_make_cstring_view(test.source), IREE_SV("enum.cpp"),
                  options);
    auto symbols = source.unit().globalScope()->find("Value");
    ASSERT_FALSE(symbols.begin() == symbols.end());
    auto* type = (*symbols.begin())->type();
    ASSERT_TRUE(source.unit().typeTraits().is_enum(type));
    Types types(source.unit(), source.diagnostics());
    auto* owner = source.unit().ast();
    EXPECT_EQ(loom_type_element_type(types.get(type, owner)), test.element);
    EXPECT_EQ(types.is_unsigned(type), test.is_unsigned);
    EXPECT_EQ(types.unqualified(type), type);
    auto* control = source.unit().control();
    auto* constant = control->getQualType(type, cxx::CvQualifiers::kConst);
    EXPECT_EQ(types.is_unsigned(constant), test.is_unsigned);
    EXPECT_TRUE(
        loom_type_equal(types.get(constant, owner), types.get(type, owner)));
    EXPECT_THROW(types.require_mutable(constant, owner), SourceRejected);
    EXPECT_EQ(loom_type_kind(types.get(control->getPointerType(type), owner)),
              LOOM_TYPE_BUFFER);
  }
}

TEST(TypesTest, BooleanEnumsKeepTheBooleanStorageContract) {
  loom_cxx_import_options_t options;
  loom_cxx_import_options_initialize(&options);
  Source source(IREE_SV("enum class Flag : bool { no, yes };"),
                IREE_SV("enum.cpp"), options);
  auto symbols = source.unit().globalScope()->find("Flag");
  ASSERT_FALSE(symbols.begin() == symbols.end());
  auto* type = (*symbols.begin())->type();
  Types types(source.unit(), source.diagnostics());
  auto* owner = source.unit().ast();
  EXPECT_EQ(loom_type_element_type(types.get(type, owner)),
            LOOM_SCALAR_TYPE_I1);
  EXPECT_THROW(types.get(source.unit().control()->getPointerType(type), owner),
               SourceRejected);
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
