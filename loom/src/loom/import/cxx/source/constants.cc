// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/source/constants.h"

#include <cxx/ast.h>
#include <cxx/ast_interpreter.h>
#include <cxx/ast_visitor.h>
#include <cxx/symbols.h>
#include <cxx/translation_unit.h>

namespace loom::cxx_import {
namespace {

class IntegerConstant final : private cxx::ASTVisitor {
 public:
  explicit IntegerConstant(cxx::TranslationUnit& unit) : unit_(unit) {}

  std::optional<std::intmax_t> evaluate(cxx::ExpressionAST* expression) {
    accept(expression);
    if (!pure_) {
      return std::nullopt;
    }
    cxx::ASTInterpreter interpreter(&unit_);
    auto evaluated = interpreter.evaluate(expression);
    auto value = evaluated ? interpreter.toInt(*evaluated) : std::nullopt;
    if (value && *value < 0) {
      auto representation =
          unit_.typeTraits().integral_representation(expression->type);
      if (!representation->isSigned) {
        return std::nullopt;
      }
    }
    return value;
  }

 private:
  bool preVisit(cxx::AST* ast) override {
    auto* expression = cxx::ast_cast<cxx::ExpressionAST>(ast);
    if (!pure_ || !expression) {
      return false;
    }
    auto traits = unit_.typeTraits();
    if (!traits.is_integral_or_enum(expression->type) ||
        traits.is_volatile(expression->type)) {
      pure_ = false;
      return false;
    }
    switch (ast->kind()) {
      case cxx::ASTKind::IntLiteralExpression:
        pure_ = !cxx::ast_cast<cxx::IntLiteralExpressionAST>(ast)
                     ->literalOperatorCall;
        return false;
      case cxx::ASTKind::CharLiteralExpression:
        pure_ = !cxx::ast_cast<cxx::CharLiteralExpressionAST>(ast)
                     ->literalOperatorCall;
        return false;
      case cxx::ASTKind::IdExpression: {
        auto* symbol = cxx::ast_cast<cxx::IdExpressionAST>(ast)->symbol;
        auto* variable = cxx::symbol_cast<cxx::VariableSymbol>(symbol);
        pure_ =
            cxx::symbol_cast<cxx::EnumeratorSymbol>(symbol) ||
            (variable && variable->constValue() &&
             (variable->isConstexpr() || traits.is_const(variable->type())));
        return false;
      }
      case cxx::ASTKind::BoolLiteralExpression:
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
    pure_ = false;
    return false;
  }

  // Resolved source types and retained frontend constants for this invocation.
  cxx::TranslationUnit& unit_;
  // Admission result; unsupported syntax prevents any interpretation.
  bool pure_ = true;
};

}  // namespace

std::optional<std::intmax_t> integer_constant(cxx::TranslationUnit& unit,
                                              cxx::ExpressionAST* expression) {
  return IntegerConstant(unit).evaluate(expression);
}

}  // namespace loom::cxx_import
