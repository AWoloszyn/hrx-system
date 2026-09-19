// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_IMPORT_CXX_BINDING_DECLARATION_TEST_H_
#define LOOM_IMPORT_CXX_BINDING_DECLARATION_TEST_H_

#include <cxx/ast.h>
#include <cxx/names.h>
#include <cxx/symbols.h>

#include <string_view>
#include <vector>

#include "loom/import/cxx/value/builder_test.h"

namespace loom::cxx_import {

struct TestDeclaration {
  // Canonicalizable function identity owned by the source.
  cxx::FunctionSymbol* function;
  // Raw leading attributes consumed by binding admission.
  cxx::List<cxx::AttributeSpecifierAST*>* attributes;
  // Source owner for admission diagnostics.
  cxx::AST* owner;
};

inline std::vector<TestDeclaration> declarations(Source& source,
                                                 std::string_view name) {
  std::vector<TestDeclaration> result;
  auto* root = cxx::ast_cast<cxx::TranslationUnitAST>(source.unit().ast());
  for (auto* declaration : cxx::ListView{root->declarationList}) {
    auto* simple = cxx::ast_cast<cxx::SimpleDeclarationAST>(declaration);
    if (!simple) {
      continue;
    }
    for (auto* declarator : cxx::ListView{simple->initDeclaratorList}) {
      auto* function =
          cxx::symbol_cast<cxx::FunctionSymbol>(declarator->symbol);
      if (function && cxx::to_string(function->name()) == name) {
        result.push_back({function, simple->attributeList, declarator});
      }
    }
  }
  return result;
}

}  // namespace loom::cxx_import

#endif  // LOOM_IMPORT_CXX_BINDING_DECLARATION_TEST_H_
