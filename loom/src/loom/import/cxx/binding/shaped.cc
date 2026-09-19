// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/binding/shaped.h"

#include <cxx/names.h>

#include <array>

#include "loom/import/cxx/source/error.h"

namespace loom::cxx_import {

std::optional<ShapedIntrinsic> ShapedIntrinsic::resolve(
    cxx::TranslationUnit& unit, Diagnostics& diagnostics, Types& types,
    const cxx::FunctionType* signature, const cxx::Attribute& attribute,
    cxx::AST* owner) {
  auto name = attribute.arguments[0]->name();
  Operation operation;
  if (name == "vector.table.lookup") {
    operation = Operation::TableLookup;
  } else if (name == "vector.dot4i") {
    operation = Operation::Dot4i;
  } else {
    return std::nullopt;
  }

  size_t operand_count = operation == Operation::TableLookup ? 2 : 3;
  if (signature->isVariadic() ||
      signature->parameterTypes().size() != operand_count) {
    diagnostics.reject(unit, owner,
                       "intrinsic declaration has the wrong operand count");
  }
  const auto* result = types.vector(signature->returnType());
  if (!result) {
    diagnostics.reject(unit, owner, "shaped intrinsic result must be a vector");
  }
  auto result_type = types.get(result, owner);
  std::array<const cxx::VectorType*, 3> operands;
  std::array<loom_type_t, 3> operand_types;
  for (size_t index = 0; index < operand_count; ++index) {
    operands[index] = types.vector(signature->parameterTypes()[index]);
    if (!operands[index]) {
      diagnostics.reject(unit, owner,
                         "shaped intrinsic operands must be vectors");
    }
    operand_types[index] = types.get(operands[index], owner);
  }

  if (operation == Operation::TableLookup) {
    if (attribute.arguments.size() != 1) {
      diagnostics.reject(unit, owner,
                         "register table lookup has no semantic arguments");
    }
    if (types.unqualified(operands[0]->elementType()) !=
            types.unqualified(result->elementType()) ||
        operands[1]->elementCount() != result->elementCount() ||
        !unit.typeTraits().is_integral(operands[1]->elementType())) {
      diagnostics.reject(unit, owner,
                         "register table lookup requires the table's element "
                         "type and the integer index vector's lane count");
    }
    return ShapedIntrinsic(operation, result_type, LOOM_VECTOR_DOT4I_KIND_S8S8);
  }

  if (attribute.arguments.size() != 2) {
    diagnostics.reject(unit, owner,
                       "dot4i requires one signedness argument: s8s8, u8s8, "
                       "s8u8, or u8u8");
  }
  auto kind_name = attribute.arguments[1]->name();
  loom_vector_dot4i_kind_t kind;
  if (kind_name == "s8s8") {
    kind = LOOM_VECTOR_DOT4I_KIND_S8S8;
  } else if (kind_name == "u8s8") {
    kind = LOOM_VECTOR_DOT4I_KIND_U8S8;
  } else if (kind_name == "s8u8") {
    kind = LOOM_VECTOR_DOT4I_KIND_S8U8;
  } else if (kind_name == "u8u8") {
    kind = LOOM_VECTOR_DOT4I_KIND_U8U8;
  } else {
    diagnostics.reject(unit, owner, "unsupported dot4i signedness argument");
  }
  if (loom_type_element_type(operand_types[0]) != LOOM_SCALAR_TYPE_I8 ||
      loom_type_element_type(operand_types[1]) != LOOM_SCALAR_TYPE_I8 ||
      loom_type_element_type(result_type) != LOOM_SCALAR_TYPE_I32 ||
      operands[0]->elementCount() != operands[1]->elementCount() ||
      operands[0]->elementCount() != result->elementCount() * 4 ||
      operands[2] != result) {
    diagnostics.reject(unit, owner,
                       "dot4i requires equal i8 input shapes with four lanes "
                       "per i32 accumulator/result lane");
  }
  if (types.is_unsigned(operands[0]->elementType()) != (kind_name[0] == 'u') ||
      types.is_unsigned(operands[1]->elementType()) != (kind_name[2] == 'u')) {
    diagnostics.reject(unit, owner,
                       "dot4i signedness must match the source byte types");
  }
  return ShapedIntrinsic(operation, result_type, kind);
}

loom_value_id_t ShapedIntrinsic::call(
    std::span<const loom_value_id_t> arguments, loom_builder_t* builder,
    loom_location_id_t location) const {
  loom_op_t* op;
  switch (operation_) {
    case Operation::TableLookup:
      check(loom_vector_table_lookup_build(builder, arguments[0], arguments[1],
                                           result_type_, location, &op));
      break;
    case Operation::Dot4i:
      check(loom_vector_dot4i_build(builder, dot_kind_, arguments[0],
                                    arguments[1], arguments[2], result_type_,
                                    location, &op));
      break;
  }
  return loom_op_results(op)[0];
}

bool ShapedIntrinsic::equivalent(const ShapedIntrinsic& other) const {
  return operation_ == other.operation_ && dot_kind_ == other.dot_kind_ &&
         loom_type_equal(result_type_, other.result_type_);
}

}  // namespace loom::cxx_import
