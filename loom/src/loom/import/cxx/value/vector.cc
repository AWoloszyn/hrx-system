// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/value/vector.h"

#include <cxx/control.h>
#include <cxx/memory_layout.h>
#include <cxx/token.h>
#include <cxx/types.h>

#include "loom/import/cxx/source/error.h"
#include "loom/ops/vector/ops.h"

namespace loom::cxx_import {

loom_value_id_t Vectors::convert(loom_value_id_t value,
                                 const cxx::Type* input_type,
                                 const cxx::Type* output_type,
                                 cxx::AST* owner) {
  auto input = types_.get(input_type, owner);
  auto output = types_.get(output_type, owner);
  auto* vector = types_.vector(output_type);
  if (!vector) {
    diagnostics_.reject(unit_, owner,
                        "vector conversion requires a vector destination");
  }
  if (loom_type_equal(input, output)) {
    return value;
  }
  loom_op_t* op;
  if (loom_type_kind(input) == LOOM_TYPE_SCALAR) {
    value = scalars_.convert(value, input_type, vector->elementType(), owner);
    check(loom_vector_splat_build(&builder_, value, output,
                                  locations_.get(owner), &op));
  } else if (loom_type_kind(input) == LOOM_TYPE_VECTOR &&
             unit_.control()->memoryLayout()->sizeOf(input_type) ==
                 unit_.control()->memoryLayout()->sizeOf(output_type)) {
    check(loom_vector_bitcast_build(&builder_, value, input, output,
                                    locations_.get(owner), &op));
  } else {
    diagnostics_.reject(unit_, owner,
                        "vector reinterpretation requires equal object sizes");
  }
  return loom_op_results(op)[0];
}

loom_value_id_t Vectors::binary(cxx::TokenKind token, loom_value_id_t left,
                                loom_value_id_t right,
                                const cxx::Type* input_type,
                                const cxx::Type* output_type, cxx::AST* owner) {
  auto input = types_.get(input_type, owner);
  auto output = types_.get(output_type, owner);
  auto* vector = types_.vector(input_type);
  bool floating = types_.is_float(vector->elementType());
  bool unsigned_input = types_.is_unsigned(vector->elementType());
  auto source = locations_.get(owner);
  loom_op_t* op;
  int predicate = -1;
  switch (token) {
    case cxx::TokenKind::T_EQUAL_EQUAL:
      predicate = 0;
      break;
    case cxx::TokenKind::T_EXCLAIM_EQUAL:
      predicate = 1;
      break;
    case cxx::TokenKind::T_LESS:
      predicate = 2;
      break;
    case cxx::TokenKind::T_LESS_EQUAL:
      predicate = 3;
      break;
    case cxx::TokenKind::T_GREATER:
      predicate = 4;
      break;
    case cxx::TokenKind::T_GREATER_EQUAL:
      predicate = 5;
      break;
    default:
      break;
  }
  if (predicate >= 0) {
    auto mask_type = loom_type_shaped_1d(LOOM_TYPE_VECTOR, LOOM_SCALAR_TYPE_I1,
                                         vector->elementCount(), 0);
    if (floating) {
      const loom_vector_cmpf_predicate_t predicates[] = {
          LOOM_VECTOR_CMPF_PREDICATE_OEQ, LOOM_VECTOR_CMPF_PREDICATE_UNE,
          LOOM_VECTOR_CMPF_PREDICATE_OLT, LOOM_VECTOR_CMPF_PREDICATE_OLE,
          LOOM_VECTOR_CMPF_PREDICATE_OGT, LOOM_VECTOR_CMPF_PREDICATE_OGE};
      check(loom_vector_cmpf_build(&builder_, 0, predicates[predicate], left,
                                   right, input, mask_type, source, &op));
    } else {
      auto selected =
          predicate >= 2 && unsigned_input ? predicate + 4 : predicate;
      check(loom_vector_cmpi_build(&builder_, selected, left, right, input,
                                   mask_type, source, &op));
    }
    // GNU/Clang vectors expose all bits set in each true integer lane. Zero
    // extension would silently change both bitwise selection and stored masks.
    check(loom_vector_extsi_build(&builder_, loom_op_results(op)[0], mask_type,
                                  output, source, &op));
    return loom_op_results(op)[0];
  }
  if (!floating) {
    auto build = token == cxx::TokenKind::T_SLASH
                     ? (unsigned_input ? loom_vector_divui_build
                                       : loom_vector_divsi_build)
                 : token == cxx::TokenKind::T_PERCENT
                     ? (unsigned_input ? loom_vector_remui_build
                                       : loom_vector_remsi_build)
                 : token == cxx::TokenKind::T_AMP   ? loom_vector_andi_build
                 : token == cxx::TokenKind::T_BAR   ? loom_vector_ori_build
                 : token == cxx::TokenKind::T_CARET ? loom_vector_xori_build
                 : token == cxx::TokenKind::T_GREATER_GREATER
                     ? (unsigned_input ? loom_vector_shrui_build
                                       : loom_vector_shrsi_build)
                     : nullptr;
    if (build) {
      check(build(&builder_, left, right, output, source, &op));
      return loom_op_results(op)[0];
    }
  }
  auto build =
      token == cxx::TokenKind::T_PLUS
          ? (floating ? loom_vector_addf_build : loom_vector_addi_build)
      : token == cxx::TokenKind::T_MINUS
          ? (floating ? loom_vector_subf_build : loom_vector_subi_build)
      : token == cxx::TokenKind::T_STAR
          ? (floating ? loom_vector_mulf_build : loom_vector_muli_build)
      : token == cxx::TokenKind::T_SLASH && floating ? loom_vector_divf_build
      : token == cxx::TokenKind::T_LESS_LESS && !floating
          ? loom_vector_shli_build
          : nullptr;
  if (!build) {
    diagnostics_.reject(unit_, owner, "unsupported vector binary operator");
  }
  check(build(&builder_, 0, left, right, output, source, &op));
  return loom_op_results(op)[0];
}

loom_value_id_t Vectors::unary(cxx::TokenKind token, loom_value_id_t value,
                               const cxx::Type* input_type,
                               const cxx::Type* output_type, cxx::AST* owner) {
  auto input = types_.get(input_type, owner);
  auto output = types_.get(output_type, owner);
  bool floating = types_.is_float(types_.vector(input_type)->elementType());
  auto source = locations_.get(owner);
  loom_op_t* op;
  switch (token) {
    case cxx::TokenKind::T_PLUS:
      return value;
    case cxx::TokenKind::T_MINUS:
      if (floating) {
        check(loom_vector_negf_build(&builder_, 0, value, output, source, &op));
      } else {
        check(loom_vector_negi_build(&builder_, value, output, source, &op));
      }
      break;
    case cxx::TokenKind::T_TILDE:
      check(loom_vector_constant_build(&builder_, loom_attr_i64(-1), input,
                                       source, &op));
      check(loom_vector_xori_build(&builder_, value, loom_op_results(op)[0],
                                   output, source, &op));
      break;
    case cxx::TokenKind::T_EXCLAIM:
      check(loom_vector_constant_build(
          &builder_, floating ? loom_attr_f64(0.0) : loom_attr_i64(0), input,
          source, &op));
      return binary(cxx::TokenKind::T_EQUAL_EQUAL, value,
                    loom_op_results(op)[0], input_type, output_type, owner);
    default:
      diagnostics_.reject(unit_, owner, "unsupported vector unary operator");
  }
  return loom_op_results(op)[0];
}

}  // namespace loom::cxx_import
