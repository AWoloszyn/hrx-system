// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/loop_schedule.h"

#include <cxx/ast.h>
#include <cxx/ast_interpreter.h>
#include <cxx/literals.h>

#include "loom/import/cxx/attributes.h"
#include "loom/import/cxx/failure.h"
#include "loom/ops/index/ops.h"

namespace loom::cxx_import {
namespace {

cxx::ExpressionAST* single_argument(cxx::TranslationUnit& unit,
                                    Diagnostics& diagnostics,
                                    cxx::AttributeAST* attribute) {
  auto* clause = attribute->attributeArgumentClause;
  if (!clause || !clause->expressionList || clause->expressionList->next) {
    diagnostics.reject(unit, attribute,
                       "loop scheduling attribute requires one argument");
  }
  return clause->expressionList->value;
}

int32_t positive_constant(cxx::TranslationUnit& unit, Diagnostics& diagnostics,
                          cxx::AttributeAST* attribute) {
  auto* expression = single_argument(unit, diagnostics, attribute);
  cxx::ASTInterpreter interpreter(&unit);
  auto evaluated = interpreter.evaluate(expression);
  auto* value = evaluated ? std::get_if<std::intmax_t>(&*evaluated) : nullptr;
  if (!value || *value <= 0 || *value > INT32_MAX) {
    diagnostics.reject(unit, attribute,
                       "loop scheduling value requires a positive i32 integer "
                       "constant expression");
  }
  return static_cast<int32_t>(*value);
}

loom_value_id_t index_constant(loom_builder_t* builder, int32_t value,
                               loom_location_id_t location) {
  loom_op_t* op;
  check(loom_index_constant_build(builder, loom_attr_i64(value),
                                  loom_type_scalar(LOOM_SCALAR_TYPE_INDEX),
                                  location, &op));
  return loom_op_results(op)[0];
}

}  // namespace

LoopSchedule::LoopSchedule(cxx::TranslationUnit& unit, Diagnostics& diagnostics,
                           cxx::List<cxx::AttributeSpecifierAST*>* attributes) {
  cxx::AttributeAST* ordering = nullptr;
  visit_loom_attributes(
      unit, attributes,
      [&](std::string_view name, cxx::AttributeAST* attribute) {
        if (name == "unroll") {
          if (iree_any_bit_set(flags_,
                               LOOM_SCF_FOR_BUILD_FLAG_HAS_UNROLL_FACTOR |
                                   LOOM_SCF_FOR_BUILD_FLAG_HAS_UNROLL_POLICY)) {
            diagnostics.reject(unit, attribute, "duplicate loop unroll policy");
          }
          if (attribute->attributeArgumentClause) {
            unroll_factor_ = positive_constant(unit, diagnostics, attribute);
            flags_ |= LOOM_SCF_FOR_BUILD_FLAG_HAS_UNROLL_FACTOR;
          } else {
            unroll_policy_ = LOOM_SCF_FOR_UNROLL_POLICY_UNROLL;
            flags_ |= LOOM_SCF_FOR_BUILD_FLAG_HAS_UNROLL_POLICY;
          }
        } else if (name == "pipeline") {
          if (iree_any_bit_set(flags_,
                               LOOM_SCF_FOR_BUILD_FLAG_HAS_PIPELINE_DEPTH)) {
            diagnostics.reject(unit, attribute,
                               "duplicate loop pipeline depth");
          }
          pipeline_depth_ = positive_constant(unit, diagnostics, attribute);
          flags_ |= LOOM_SCF_FOR_BUILD_FLAG_HAS_PIPELINE_DEPTH;
        } else if (name == "schedule") {
          if (ordering) {
            diagnostics.reject(unit, attribute,
                               "duplicate loop unroll schedule");
          }
          ordering = attribute;
          auto* literal = cxx::ast_cast<cxx::StringLiteralExpressionAST>(
              single_argument(unit, diagnostics, attribute));
          auto spelling =
              literal ? literal->literal->stringValue() : std::string_view{};
          if (spelling == "linear") {
            unroll_schedule_ = LOOM_SCF_FOR_UNROLL_SCHEDULE_LINEAR;
          } else if (spelling == "interleaved") {
            unroll_schedule_ = LOOM_SCF_FOR_UNROLL_SCHEDULE_INTERLEAVED;
          } else if (spelling == "recurrence") {
            unroll_schedule_ = LOOM_SCF_FOR_UNROLL_SCHEDULE_RECURRENCE;
          } else {
            diagnostics.reject(unit, attribute,
                               "loop schedule requires linear, interleaved, or "
                               "recurrence as a string literal");
          }
          flags_ |= LOOM_SCF_FOR_BUILD_FLAG_HAS_UNROLL_SCHEDULE;
        } else {
          diagnostics.reject(unit, attribute,
                             "unsupported Loom loop attribute");
        }
      });
  if (ordering && !iree_any_bit_set(
                      flags_, LOOM_SCF_FOR_BUILD_FLAG_HAS_UNROLL_FACTOR |
                                  LOOM_SCF_FOR_BUILD_FLAG_HAS_UNROLL_POLICY)) {
    diagnostics.reject(unit, ordering,
                       "loop schedule requires an unroll policy");
  }
}

loom_op_t* LoopSchedule::build(loom_builder_t* builder, loom_value_id_t lower,
                               loom_value_id_t upper, loom_value_id_t step,
                               std::span<const loom_value_id_t> initial,
                               loom_location_id_t location) const {
  auto depth =
      iree_any_bit_set(flags_, LOOM_SCF_FOR_BUILD_FLAG_HAS_PIPELINE_DEPTH)
          ? index_constant(builder, pipeline_depth_, location)
          : 0;
  auto factor =
      iree_any_bit_set(flags_, LOOM_SCF_FOR_BUILD_FLAG_HAS_UNROLL_FACTOR)
          ? index_constant(builder, unroll_factor_, location)
          : 0;
  loom_op_t* op;
  check(loom_scf_for_build(builder, flags_, lower, upper, step, initial.data(),
                           initial.size(), nullptr, 0, depth, factor,
                           unroll_policy_, unroll_schedule_, location, &op));
  return op;
}

}  // namespace loom::cxx_import
