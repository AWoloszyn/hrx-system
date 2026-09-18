// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_IMPORT_CXX_MUTATIONS_H_
#define LOOM_IMPORT_CXX_MUTATIONS_H_

#include <cxx/ast.h>
#include <cxx/ast_visitor.h>

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace loom::cxx_import {

// One source traversal records the scalar bindings each structured construct
// may change. Lowering consumes these lists to construct joins and loop state.
class Mutations final : public cxx::ASTVisitor {
 public:
  bool preVisit(cxx::AST* ast) override {
    if (structured(ast)) {
      owners_.push_back(ast);
    }
    return true;
  }
  void postVisit(cxx::AST* ast) override {
    if (structured(ast)) {
      owners_.pop_back();
    }
  }
  void visit(cxx::AssignmentExpressionAST* ast) override {
    record(ast->leftExpression);
    cxx::ASTVisitor::visit(ast);
  }
  void visit(cxx::CompoundAssignmentExpressionAST* ast) override {
    record(ast->targetExpression);
    cxx::ASTVisitor::visit(ast);
  }
  void visit(cxx::PostIncrExpressionAST* ast) override {
    record(ast->baseExpression);
    cxx::ASTVisitor::visit(ast);
  }
  void visit(cxx::UnaryExpressionAST* ast) override {
    if (ast->op == cxx::TokenKind::T_PLUS_PLUS ||
        ast->op == cxx::TokenKind::T_MINUS_MINUS) {
      record(ast->expression);
    }
    cxx::ASTVisitor::visit(ast);
  }
  const std::vector<cxx::Symbol*>& written(cxx::AST* ast) const {
    auto found = writes_.find(ast);
    return found == writes_.end() ? empty_ : found->second;
  }

 private:
  static bool structured(cxx::AST* ast) {
    return cxx::ast_cast<cxx::IfStatementAST>(ast) ||
           cxx::ast_cast<cxx::ForStatementAST>(ast) ||
           cxx::ast_cast<cxx::WhileStatementAST>(ast) ||
           cxx::ast_cast<cxx::DoStatementAST>(ast) ||
           cxx::ast_cast<cxx::CompoundStatementAST>(ast);
  }
  void record(cxx::ExpressionAST* expression) {
    auto* id = cxx::ast_cast<cxx::IdExpressionAST>(expression);
    if (!id) {
      return;
    }
    for (auto* owner : owners_) {
      auto& writes = writes_[owner];
      if (std::ranges::find(writes, id->symbol) == writes.end()) {
        writes.push_back(id->symbol);
      }
    }
  }
  // Active structured ancestors receive each directly observed mutation.
  std::vector<cxx::AST*> owners_;
  // Stable encounter order defines the output region argument/result order.
  std::unordered_map<cxx::AST*, std::vector<cxx::Symbol*>> writes_;
  // Shared empty result for constructs with no mutations.
  const std::vector<cxx::Symbol*> empty_;
};

}  // namespace loom::cxx_import

#endif  // LOOM_IMPORT_CXX_MUTATIONS_H_
