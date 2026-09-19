// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/value/scalar.h"

#include <cxx/ast_interpreter.h>
#include <cxx/control.h>
#include <cxx/memory_layout.h>
#include <cxx/token.h>

#include <bit>

#include "loom/import/cxx/source/error.h"
#include "loom/ops/index/ops.h"
#include "loom/ops/scalar/ops.h"

namespace loom::cxx_import {

loom_value_id_t Scalars::convert(loom_value_id_t value,
                                 const cxx::Type* input_type,
                                 const cxx::Type* output_type,
                                 cxx::AST* owner) {
  auto input = types_.get(input_type, owner);
  auto output = types_.get(output_type, owner);
  if (loom_type_equal(input, output)) {
    return value;
  }
  if (loom_type_kind(input) != LOOM_TYPE_SCALAR ||
      loom_type_kind(output) != LOOM_TYPE_SCALAR) {
    diagnostics_.reject(
        unit_, owner, "conversion must preserve pointer/array representation");
  }
  loom_op_t* op;
  if (loom_type_element_type(output) == LOOM_SCALAR_TYPE_I1) {
    loom_op_t* zero;
    check(loom_scalar_constant_build(
        &builder_,
        types_.is_float(input_type) ? loom_attr_f64(0.0) : loom_attr_i64(0),
        input, locations_.get(owner), &zero));
    if (types_.is_float(input_type)) {
      check(loom_scalar_cmpf_build(&builder_, 0, LOOM_SCALAR_CMPF_PREDICATE_UNE,
                                   value, loom_op_results(zero)[0],
                                   locations_.get(owner), &op));
    } else {
      check(loom_scalar_cmpi_build(&builder_, LOOM_SCALAR_CMPI_PREDICATE_NE,
                                   value, loom_op_results(zero)[0],
                                   locations_.get(owner), &op));
    }
    return loom_op_results(op)[0];
  }
  bool unsigned_input = types_.is_unsigned(input_type) ||
                        loom_type_element_type(input) == LOOM_SCALAR_TYPE_I1;
  auto* layout = unit_.control()->memoryLayout();
  bool narrows = layout->sizeOf(input_type) > layout->sizeOf(output_type);
  auto build =
      types_.is_float(output_type)
          ? (types_.is_float(input_type)
                 ? (narrows ? loom_scalar_fptrunc_build
                            : loom_scalar_extf_build)
                 : (unsigned_input ? loom_scalar_uitofp_build
                                   : loom_scalar_sitofp_build))
          : (types_.is_float(input_type)
                 ? (types_.is_unsigned(output_type) ? loom_scalar_fptoui_build
                                                    : loom_scalar_fptosi_build)
                 : (narrows ? loom_scalar_trunci_build
                            : (unsigned_input ? loom_scalar_extui_build
                                              : loom_scalar_extsi_build)));
  check(build(&builder_, value, input, output, locations_.get(owner), &op));
  return loom_op_results(op)[0];
}

loom_value_id_t Scalars::binary(cxx::TokenKind token, loom_value_id_t left,
                                loom_value_id_t right,
                                const cxx::Type* input_type,
                                const cxx::Type* output_type, cxx::AST* ast) {
  auto source = locations_.get(ast);
  bool floating = types_.is_float(input_type);
  bool unsigned_input = types_.is_unsigned(input_type);
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
    if (floating) {
      const loom_scalar_cmpf_predicate_t predicates[] = {
          LOOM_SCALAR_CMPF_PREDICATE_OEQ, LOOM_SCALAR_CMPF_PREDICATE_UNE,
          LOOM_SCALAR_CMPF_PREDICATE_OLT, LOOM_SCALAR_CMPF_PREDICATE_OLE,
          LOOM_SCALAR_CMPF_PREDICATE_OGT, LOOM_SCALAR_CMPF_PREDICATE_OGE};
      check(loom_scalar_cmpf_build(&builder_, 0, predicates[predicate], left,
                                   right, source, &op));
    } else {
      auto selected = static_cast<loom_scalar_cmpi_predicate_t>(
          predicate >= 2 && unsigned_input ? predicate + 4 : predicate);
      check(loom_scalar_cmpi_build(&builder_, selected, left, right, source,
                                   &op));
    }
    return loom_op_results(op)[0];
  }
  if (!floating) {
    auto build = token == cxx::TokenKind::T_SLASH
                     ? (unsigned_input ? loom_scalar_divui_build
                                       : loom_scalar_divsi_build)
                 : token == cxx::TokenKind::T_PERCENT
                     ? (unsigned_input ? loom_scalar_remui_build
                                       : loom_scalar_remsi_build)
                 : token == cxx::TokenKind::T_AMP   ? loom_scalar_andi_build
                 : token == cxx::TokenKind::T_BAR   ? loom_scalar_ori_build
                 : token == cxx::TokenKind::T_CARET ? loom_scalar_xori_build
                 : token == cxx::TokenKind::T_GREATER_GREATER
                     ? (unsigned_input ? loom_scalar_shrui_build
                                       : loom_scalar_shrsi_build)
                     : nullptr;
    if (build) {
      check(build(&builder_, left, right, types_.get(output_type, ast), source,
                  &op));
      return loom_op_results(op)[0];
    }
  }
  auto build =
      token == cxx::TokenKind::T_PLUS
          ? (floating ? loom_scalar_addf_build : loom_scalar_addi_build)
      : token == cxx::TokenKind::T_MINUS
          ? (floating ? loom_scalar_subf_build : loom_scalar_subi_build)
      : token == cxx::TokenKind::T_STAR
          ? (floating ? loom_scalar_mulf_build : loom_scalar_muli_build)
      : token == cxx::TokenKind::T_SLASH && floating ? loom_scalar_divf_build
      : token == cxx::TokenKind::T_LESS_LESS && !floating
          ? loom_scalar_shli_build
          : nullptr;
  if (!build) {
    diagnostics_.reject(unit_, ast, "unsupported binary operator");
  }
  check(build(&builder_, 0, left, right, types_.get(output_type, ast), source,
              &op));
  return loom_op_results(op)[0];
}

loom_value_id_t Scalars::integer(int64_t value, loom_scalar_type_t scalar,
                                 loom_location_id_t source) {
  loom_op_t* op;
  auto* build =
      scalar == LOOM_SCALAR_TYPE_INDEX || scalar == LOOM_SCALAR_TYPE_OFFSET
          ? loom_index_constant_build
          : loom_scalar_constant_build;
  check(build(&builder_, loom_attr_i64(value), loom_type_scalar(scalar), source,
              &op));
  return loom_op_results(op)[0];
}

loom_value_id_t Scalars::constant(const cxx::ConstValue& value,
                                  const cxx::Type* source_type, cxx::AST* ast) {
  cxx::ASTInterpreter interpreter(&unit_);
  auto source = locations_.get(ast);
  auto target = types_.get(source_type, ast);
  loom_attribute_t attribute;
  if (types_.is_float(source_type)) {
    auto number = interpreter.toDouble(value);
    if (!number) {
      diagnostics_.reject(unit_, ast, "invalid floating literal");
    }
    attribute = loom_attr_f64(*number);
  } else {
    auto number = interpreter.toInt(value);
    if (!number) {
      diagnostics_.reject(unit_, ast, "invalid integer literal");
    }
    // Literals are represented in the signed storage width of their IR
    // scalar type; signedness remains a source fact at each operation.
    auto bytes = *unit_.control()->memoryLayout()->sizeOf(source_type);
    int64_t stored =
        bytes == 1   ? std::bit_cast<int8_t>(static_cast<uint8_t>(*number))
        : bytes == 2 ? std::bit_cast<int16_t>(static_cast<uint16_t>(*number))
        : bytes == 4 ? std::bit_cast<int32_t>(static_cast<uint32_t>(*number))
                     : *number;
    attribute = loom_attr_i64(stored);
  }
  loom_op_t* op;
  check(loom_scalar_constant_build(&builder_, attribute, target, source, &op));
  return loom_op_results(op)[0];
}

}  // namespace loom::cxx_import
