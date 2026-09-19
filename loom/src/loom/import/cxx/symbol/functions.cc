// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/symbol/functions.h"

#include <cxx/ast.h>
#include <cxx/attributes.h>
#include <cxx/names.h>
#include <cxx/symbols.h>
#include <cxx/types.h>

#include <cctype>

#include "loom/import/cxx/source/attributes.h"
#include "loom/import/cxx/source/error.h"
#include "loom/ir/module.h"
#include "loom/ops/func/ops.h"
#include "loom/ops/kernel/ops.h"

namespace loom::cxx_import {

void Functions::select(std::span<const iree_string_view_t> roots) {
  auto* root = cxx::ast_cast<cxx::TranslationUnitAST>(unit_.ast());
  if (!root) {
    diagnostics_.reject(unit_, unit_.ast(),
                        "expected an ordinary translation unit");
  }
  std::vector<cxx::FunctionSymbol*> definitions;
  collect(root->declarationList, definitions);
  if (!roots.empty()) {
    std::unordered_map<std::string, std::vector<cxx::FunctionSymbol*>>
        candidates;
    for (auto* symbol : definitions) {
      candidates[qualified_name(symbol)].push_back(symbol);
    }
    for (size_t i = 0; i < roots.size(); ++i) {
      auto spelling = cxx_import::string(roots[i]);
      auto found = candidates.find(spelling);
      if (found == candidates.end()) {
        diagnostics_.reject(unit_, root,
                            "root has no concrete definition: " + spelling);
      }
      if (found->second.size() != 1) {
        diagnostics_.reject(unit_, root, "ambiguous root: " + spelling);
      }
      auto* selected = found->second.front();
      exported_.insert(selected);
      declare(selected);
    }
  } else {
    for (auto* symbol : definitions) {
      bool visible = !symbol->isStatic();
      auto* visibility =
          cxx::attributeArgument(symbol->attributes(), "visibility");
      if (visibility && visibility->name() == "hidden") {
        visible = false;
      }
      for (auto* owner : symbol->enclosingSymbols()) {
        if (auto* space = cxx::symbol_cast<cxx::NamespaceSymbol>(owner)) {
          if (space->parent() && !space->name()) {
            visible = false;
          }
        }
      }
      if (visible) {
        exported_.insert(symbol);
        declare(symbol);
      }
    }
  }
}

void Functions::collect(cxx::List<cxx::DeclarationAST*>* declarations,
                        std::vector<cxx::FunctionSymbol*>& definitions) {
  for (auto* declaration : cxx::ListView{declarations}) {
    if (auto* function =
            cxx::ast_cast<cxx::FunctionDefinitionAST>(declaration)) {
      if (!function->symbol->isTemplatePattern()) {
        intrinsics_.declaration(function->symbol, function->attributeList,
                                function);
        launches_.declaration(function->symbol, function->attributeList);
        definitions.push_back(function->symbol);
      }
    } else if (auto* space =
                   cxx::ast_cast<cxx::NamespaceDefinitionAST>(declaration)) {
      collect(space->declarationList, definitions);
    } else if (auto* linkage =
                   cxx::ast_cast<cxx::LinkageSpecificationAST>(declaration)) {
      collect(linkage->declarationList, definitions);
    } else if (auto* simple =
                   cxx::ast_cast<cxx::SimpleDeclarationAST>(declaration)) {
      for (auto* declarator : cxx::ListView{simple->initDeclaratorList}) {
        if (auto* function =
                cxx::symbol_cast<cxx::FunctionSymbol>(declarator->symbol)) {
          intrinsics_.declaration(function, simple->attributeList, declarator);
          launches_.declaration(function, simple->attributeList);
        }
        auto* variable =
            cxx::symbol_cast<cxx::VariableSymbol>(declarator->symbol);
        if (variable && !variable->isExtern() &&
            !(variable->isConstexpr() ||
              (unit_.typeTraits().is_const(variable->type()) &&
               variable->constValue()))) {
          diagnostics_.reject(
              unit_, declarator,
              "global storage definitions require a global-storage "
              "projection");
        }
      }
    }
  }
}

const std::string& Functions::qualified_name(cxx::FunctionSymbol* symbol) {
  auto found = qualified_names_.find(symbol);
  if (found != qualified_names_.end()) {
    return found->second;
  }
  std::string spelling = cxx::to_string(symbol->name());
  for (auto* owner : symbol->enclosingSymbols()) {
    if (auto* space = cxx::symbol_cast<cxx::NamespaceSymbol>(owner)) {
      if (space->name()) {
        spelling = cxx::to_string(space->name()) + "::" + spelling;
      }
    }
  }
  return qualified_names_.emplace(symbol, std::move(spelling)).first->second;
}

loom_symbol_ref_t Functions::declare(cxx::FunctionSymbol* function) {
  if (auto found = callees_.find(function); found != callees_.end()) {
    return found->second;
  }
  if (!function->templateArguments().empty() && function->declaration()) {
    launches_.declaration(function, function->declaration()->attributeList);
  }
  std::string spelling = qualified_name(function);
  for (const auto& argument : function->templateArguments()) {
    spelling += "_" + cxx::to_string(argument);
  }
  for (char& ch : spelling) {
    if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_') {
      ch = '_';
    }
  }
  auto ordinal = symbol_names_[spelling]++;
  if (ordinal) {
    spelling += "_" + std::to_string(ordinal);
  }
  loom_string_id_t name;
  check(loom_module_intern_string(module_, view(spelling), &name));
  loom_symbol_id_t id;
  check(loom_module_add_symbol(module_, name, &id));
  loom_symbol_ref_t callee = {0, id};
  callees_[function] = callee;
  pending_.push_back(function);
  return callee;
}

FunctionBody Functions::define(cxx::FunctionSymbol* symbol, Types& types,
                               Locations& locations, loom_builder_t* builder) {
  auto* definition = symbol->declaration();
  if (!definition) {
    diagnostics_.reject(unit_, unit_.ast(),
                        "reachable function has no definition");
  }
  auto* body = cxx::ast_cast<cxx::CompoundStatementFunctionBodyAST>(
      definition->functionBody);
  if (!body) {
    diagnostics_.reject(unit_, definition, "unsupported function body");
  }
  auto parameters = symbol->parameters();
  bool kernel = annotated(symbol, "kernel");
  std::vector<loom_type_t> arguments;
  for (auto* parameter : parameters) {
    if (kernel) {
      arguments.push_back(types.get(parameter->type(), definition));
    } else {
      types.append(parameter->type(), definition, arguments);
    }
  }
  auto* signature = cxx::type_cast<cxx::FunctionType>(symbol->type());
  if (!signature || signature->isVariadic()) {
    diagnostics_.reject(unit_, definition,
                        "variadic functions are not admitted");
  }
  bool returns_void = signature->returnType()->kind() == cxx::TypeKind::kVoid;
  loom_op_t* op;
  if (kernel) {
    if (!returns_void) {
      diagnostics_.reject(unit_, definition, "kernel must return void");
    }
    check(loom_kernel_def_build(builder, 0, 0, {}, 0, 0, callees_.at(symbol),
                                nullptr, 0, arguments.data(), arguments.size(),
                                nullptr, 0, locations.get(definition), &op));
    auto saved =
        loom_builder_enter_region(builder, op, loom_kernel_def_config(op));
    auto name_id =
        module_->symbols.entries[callees_.at(symbol).symbol_id].name_id;
    auto spelling = module_->strings.entries[name_id];
    launches_.build(symbol, {spelling.data, spelling.size}, builder,
                    locations.get(definition));
    loom_builder_restore(builder, saved);
  } else {
    launches_.reject_ordinary_function(symbol);
    std::vector<loom_type_t> results;
    if (!returns_void) {
      types.append(signature->returnType(), definition, results);
    }
    check(loom_func_def_build(
        builder,
        (annotated(symbol, "device") ? LOOM_FUNC_DEF_BUILD_FLAG_HAS_CC : 0) |
            (exported_.contains(symbol)
                 ? LOOM_FUNC_DEF_BUILD_FLAG_HAS_VISIBILITY
                 : 0) |
            (annotated(symbol, "force_inline")
                 ? LOOM_FUNC_DEF_BUILD_FLAG_HAS_INLINE_POLICY
                 : 0),
        exported_.contains(symbol) ? LOOM_FUNC_VISIBILITY_PUBLIC : 0, 0,
        annotated(symbol, "device") ? LOOM_FUNC_CC_DEVICE : 0, 0, 0,
        annotated(symbol, "force_inline") ? LOOM_INLINE_POLICY_INLINE : 0, {},
        0, {}, 0, {}, callees_.at(symbol), arguments.data(), arguments.size(),
        results.data(), results.size(), nullptr, 0, nullptr, 0,
        locations.get(definition), &op));
  }
  auto* region = kernel ? loom_kernel_def_body(op) : loom_func_def_body(op);
  return {definition,
          body->statement,
          op,
          region,
          signature->returnType(),
          kernel ? FunctionKind::Kernel : FunctionKind::Ordinary};
}

}  // namespace loom::cxx_import
