// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/binding/assumptions.h"

#include <cxx/ast.h>
#include <cxx/initialization.h>
#include <cxx/token.h>

#include "loom/import/cxx/source/constants.h"

namespace loom::cxx_import {
namespace {

cxx::ExpressionAST* unwrapped(cxx::ExpressionAST* expression) {
  expression = cxx::Initializer::stripImplicitCasts(expression);
  while (auto* nested = cxx::ast_cast<cxx::NestedExpressionAST>(expression)) {
    expression = cxx::Initializer::stripImplicitCasts(nested->expression);
  }
  return expression;
}

void collect_bounds(cxx::TranslationUnit& unit, Diagnostics& diagnostics,
                    cxx::ExpressionAST* expression,
                    std::vector<AssumptionBound>& bounds) {
  auto* condition =
      cxx::ast_cast<cxx::BinaryExpressionAST>(unwrapped(expression));
  if (condition && !condition->symbol &&
      condition->op == cxx::TokenKind::T_AMP_AMP) {
    collect_bounds(unit, diagnostics, condition->leftExpression, bounds);
    collect_bounds(unit, diagnostics, condition->rightExpression, bounds);
    return;
  }
  auto* binding = condition ? cxx::ast_cast<cxx::IdExpressionAST>(
                                  unwrapped(condition->leftExpression))
                            : nullptr;
  auto traits = unit.typeTraits();
  if (!condition || condition->symbol ||
      condition->op != cxx::TokenKind::T_LESS || !binding ||
      !traits.is_integral(binding->type) ||
      !traits.is_unsigned(binding->type) || traits.is_volatile(binding->type) ||
      !traits.is_integral(condition->leftExpression->type)) {
    diagnostics.reject(unit, expression,
                       "assume requires unsigned scalar bindings < constant "
                       "bounds, optionally joined by &&");
  }
  auto bound = integer_constant(unit, condition->rightExpression);
  if (!bound || *bound <= 0 || *bound > INT32_MAX) {
    diagnostics.reject(unit, condition->rightExpression,
                       "assume upper bound requires a pure integer constant "
                       "in [1, INT32_MAX] without calls or mutation");
  }
  bounds.push_back(
      {binding, condition->leftExpression, static_cast<int32_t>(*bound)});
}

}  // namespace

std::vector<AssumptionBound> assumption_bounds(cxx::TranslationUnit& unit,
                                               Diagnostics& diagnostics,
                                               cxx::CallExpressionAST* call) {
  if (!call->expressionList || call->expressionList->next) {
    diagnostics.reject(unit, call, "assume requires exactly one condition");
  }
  std::vector<AssumptionBound> bounds;
  collect_bounds(unit, diagnostics, call->expressionList->value, bounds);
  return bounds;
}

}  // namespace loom::cxx_import
