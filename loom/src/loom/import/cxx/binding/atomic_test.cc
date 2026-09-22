// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/binding/atomic.h"

#include <cxx/ast.h>
#include <cxx/symbols.h>

#include <vector>

#include "iree/testing/gtest.h"
#include "loom/import/cxx/source/source.h"

namespace loom::cxx_import {
namespace {

TEST(AtomicTest, SemanticIdentityUsesAdmittedSpecialization) {
  loom_cxx_import_options_t options;
  loom_cxx_import_options_initialize(&options);
  Source source(IREE_SV(R"(
    template <int Kind, int Ordering, int Scope>
    [[loom::op("view.atomic.rmw")]] int binding(int, volatile int*);
    int first(int* pointer) { return binding<2, 0, 3>(1, pointer); }
    int same(int* pointer) { return binding<2, 0, 3>(2, pointer); }
    int ordered(int* pointer) { return binding<2, 1, 3>(1, pointer); }
    int scoped(int* pointer) { return binding<2, 0, 2>(1, pointer); }
    int different(int* pointer) { return binding<4, 0, 3>(1, pointer); }
  )"),
                IREE_SV("atomic.cpp"), options);
  Types types(source.unit(), source.diagnostics());
  std::vector<AtomicIntrinsic> bindings;
  auto* root = cxx::ast_cast<cxx::TranslationUnitAST>(source.unit().ast());
  for (auto* declaration : cxx::ListView{root->declarationList}) {
    auto* definition = cxx::ast_cast<cxx::FunctionDefinitionAST>(declaration);
    if (!definition) {
      continue;
    }
    auto* body = cxx::ast_cast<cxx::CompoundStatementFunctionBodyAST>(
        definition->functionBody);
    ASSERT_NE(body, nullptr);
    auto* returned = cxx::ast_cast<cxx::ReturnStatementAST>(
        body->statement->statementList->value);
    ASSERT_NE(returned, nullptr);
    auto* call = cxx::ast_cast<cxx::CallExpressionAST>(returned->expression);
    ASSERT_NE(call, nullptr);
    auto* callee = cxx::ast_cast<cxx::IdExpressionAST>(call->baseExpression);
    ASSERT_NE(callee, nullptr);
    auto* function = cxx::symbol_cast<cxx::FunctionSymbol>(callee->symbol);
    ASSERT_NE(function, nullptr);
    auto binding = AtomicIntrinsic::resolve(
        source.unit(), source.diagnostics(), types, function,
        function->attributes()->front(), call);
    ASSERT_TRUE(binding);
    bindings.push_back(*binding);
  }
  ASSERT_EQ(bindings.size(), 5u);
  EXPECT_TRUE(bindings[0].equivalent(bindings[1]));
  EXPECT_FALSE(bindings[0].equivalent(bindings[2]));
  EXPECT_FALSE(bindings[0].equivalent(bindings[3]));
  EXPECT_FALSE(bindings[0].equivalent(bindings[4]));
}

}  // namespace
}  // namespace loom::cxx_import
