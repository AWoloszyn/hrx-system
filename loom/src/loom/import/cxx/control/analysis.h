// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_IMPORT_CXX_CONTROL_ANALYSIS_H_
#define LOOM_IMPORT_CXX_CONTROL_ANALYSIS_H_

#include <cxx/ast_visitor.h>
#include <cxx/cxx_fwd.h>
#include <cxx/symbols_fwd.h>

#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace loom::cxx_import {

// Retained proof that a source loop has a stable, nonwrapping unsigned
// interval.
struct CountedLoop {
  // Source binding replaced by the structured loop's induction argument.
  cxx::Symbol* induction;
  // Stable upper bound, evaluated once by the composition driver.
  cxx::ExpressionAST* upper;
  // Positive constant step in the source's unsigned-int width.
  unsigned step;
};

// One traversal owns control facts for an immutable source function body.
// Ordered writes include nested constructs and preserve source symbol identity.
// Counted-loop classification happens after those writes are complete. Queries
// only consume retained facts; they do not traverse source or output IR.
// The source unit and body outlive this object and every returned reference.
class ControlFlow final : private cxx::ASTVisitor {
 public:
  ControlFlow(cxx::TranslationUnit& unit, cxx::StatementAST* body);
  ControlFlow(const ControlFlow&) = delete;
  ControlFlow& operator=(const ControlFlow&) = delete;

  // Unique bindings mutated under a structured owner, in encounter order.
  std::span<cxx::Symbol* const> written(cxx::AST* owner) const;
  // Null retains ordinary while semantics; a result permits scf.for lowering.
  const CountedLoop* counted(cxx::ForStatementAST* loop) const;

 private:
  bool preVisit(cxx::AST* ast) override;
  void postVisit(cxx::AST* ast) override;
  void visit(cxx::AssignmentExpressionAST* ast) override;
  void visit(cxx::CompoundAssignmentExpressionAST* ast) override;
  void visit(cxx::PostIncrExpressionAST* ast) override;
  void visit(cxx::UnaryExpressionAST* ast) override;
  static bool structured(cxx::AST* ast);
  void record(cxx::ExpressionAST* expression);
  std::optional<CountedLoop> classify(cxx::ForStatementAST* loop);

  // Resolved source types and literal interpretation for loop admission.
  cxx::TranslationUnit& unit_;
  // Active structured ancestors during construction only.
  std::vector<cxx::AST*> owners_;
  // Stable encounter order determines region argument/result order.
  std::unordered_map<cxx::AST*, std::vector<cxx::Symbol*>> writes_;
  // Proven intervals retained after each source loop's children are visited.
  std::unordered_map<cxx::ForStatementAST*, CountedLoop> counted_;
};

}  // namespace loom::cxx_import

#endif  // LOOM_IMPORT_CXX_CONTROL_ANALYSIS_H_
