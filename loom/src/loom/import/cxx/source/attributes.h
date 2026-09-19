// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_IMPORT_CXX_SOURCE_ATTRIBUTES_H_
#define LOOM_IMPORT_CXX_SOURCE_ATTRIBUTES_H_

#include <cxx/ast.h>
#include <cxx/names.h>
#include <cxx/translation_unit.h>

namespace loom::cxx_import {

// Visits raw C++ Loom attributes, including duplicates and numeric arguments
// that the frontend's semantic string-attribute map cannot preserve. Admission
// owners retain their checked interpretation for subsequent source lowering.
template <typename Visitor>
void visit_loom_attributes(cxx::TranslationUnit& unit,
                           cxx::List<cxx::AttributeSpecifierAST*>* attributes,
                           Visitor visitor) {
  for (auto* specifier : cxx::ListView{attributes}) {
    auto* list = cxx::ast_cast<cxx::CxxAttributeAST>(specifier);
    if (!list) {
      continue;
    }
    auto* prefix = list->attributeUsingPrefix;
    for (auto* attribute : cxx::ListView{list->attributeList}) {
      const cxx::Identifier* space =
          prefix ? unit.identifier(prefix->attributeNamespaceLoc) : nullptr;
      const cxx::Identifier* name = nullptr;
      if (auto* scoped = cxx::ast_cast<cxx::ScopedAttributeTokenAST>(
              attribute->attributeToken)) {
        space = scoped->attributeNamespace;
        name = scoped->identifier;
      } else if (auto* simple = cxx::ast_cast<cxx::SimpleAttributeTokenAST>(
                     attribute->attributeToken)) {
        name = simple->identifier;
      }
      if (space && name && space->name() == "loom") {
        visitor(name->name(), attribute);
      }
    }
  }
}

}  // namespace loom::cxx_import

#endif  // LOOM_IMPORT_CXX_SOURCE_ATTRIBUTES_H_
