// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/binding/assumptions.h"

#include <cxx/ast.h>
#include <cxx/ast_interpreter.h>
#include <cxx/ast_visitor.h>
#include <cxx/initialization.h>
#include <cxx/token.h>

namespace loom::cxx_import {
namespace {

cxx::ExpressionAST* unwrapped(cxx::ExpressionAST* expression) {
  expression = cxx::Initializer::stripImplicitCasts(expression);
  while (auto* nested = cxx::ast_cast<cxx::NestedExpressionAST>(expression)) {
    expression = cxx::Initializer::stripImplicitCasts(nested->expression);
  }
  return expression;
}

// Owns admission of the pure bound grammar; cxx owns constant evaluation and
// source conversions. Unevaluated operands and retained constants are leaves.
class ConstantBound final : public cxx::ASTVisitor {
 public:
  ConstantBound(cxx::TranslationUnit& unit, Diagnostics& diagnostics)
      : unit_(unit), diagnostics_(diagnostics) {}

  int32_t evaluate(cxx::ExpressionAST* expression) {
    accept(expression);
    cxx::ASTInterpreter interpreter(&unit_);
    auto evaluated = interpreter.evaluate(expression);
    auto bound = evaluated ? interpreter.toInt(*evaluated) : std::nullopt;
    if (!bound || *bound <= 0 || *bound > INT32_MAX) {
      diagnostics_.reject(unit_, expression,
                          "assume upper bound must be an integer constant "
                          "in [1, INT32_MAX]");
    }
    return static_cast<int32_t>(*bound);
  }

 private:
  bool preVisit(cxx::AST* ast) override {
    auto* expression = cxx::ast_cast<cxx::ExpressionAST>(ast);
    if (!expression) {
      return false;
    }
    auto traits = unit_.typeTraits();
    if (!traits.is_integral_or_enum(expression->type) ||
        traits.is_volatile(expression->type)) {
      diagnostics_.reject(unit_, ast,
                          "assume upper bound requires pure integer operands");
    }
    switch (ast->kind()) {
      case cxx::ASTKind::IntLiteralExpression:
      case cxx::ASTKind::CharLiteralExpression:
      case cxx::ASTKind::BoolLiteralExpression:
      case cxx::ASTKind::IdExpression:
      case cxx::ASTKind::ConstExpression:
      case cxx::ASTKind::SizeofExpression:
      case cxx::ASTKind::SizeofTypeExpression:
      case cxx::ASTKind::AlignofTypeExpression:
        return false;
      case cxx::ASTKind::NestedExpression:
      case cxx::ASTKind::CastExpression:
      case cxx::ASTKind::CppCastExpression:
      case cxx::ASTKind::ConditionalExpression:
        return true;
      case cxx::ASTKind::ImplicitCastExpression:
        if (!cxx::ast_cast<cxx::ImplicitCastExpressionAST>(ast)
                 ->conversionFunction) {
          return true;
        }
        break;
      case cxx::ASTKind::TypeConstruction:
        if (!cxx::ast_cast<cxx::TypeConstructionAST>(ast)->constructorSymbol) {
          return true;
        }
        break;
      case cxx::ASTKind::UnaryExpression: {
        auto* unary = cxx::ast_cast<cxx::UnaryExpressionAST>(ast);
        if (!unary->symbol && (unary->op == cxx::TokenKind::T_PLUS ||
                               unary->op == cxx::TokenKind::T_MINUS ||
                               unary->op == cxx::TokenKind::T_TILDE ||
                               unary->op == cxx::TokenKind::T_EXCLAIM)) {
          return true;
        }
        break;
      }
      case cxx::ASTKind::BinaryExpression: {
        auto* binary = cxx::ast_cast<cxx::BinaryExpressionAST>(ast);
        if (!binary->symbol && binary->op != cxx::TokenKind::T_COMMA &&
            binary->op != cxx::TokenKind::T_DOT_STAR &&
            binary->op != cxx::TokenKind::T_MINUS_GREATER_STAR) {
          return true;
        }
        break;
      }
      default:
        break;
    }
    diagnostics_.reject(unit_, ast,
                        "assume upper bound requires a pure integer constant "
                        "expression without calls or mutation");
  }

  // Resolved types and constant-evaluation state for this import invocation.
  cxx::TranslationUnit& unit_;
  // Source rejection boundary, outliving this admission visitor.
  Diagnostics& diagnostics_;
};

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
  auto bound =
      ConstantBound(unit, diagnostics).evaluate(condition->rightExpression);
  bounds.push_back({binding, condition->leftExpression, bound});
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
