// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_IMPORT_CXX_SYMBOL_FUNCTIONS_H_
#define LOOM_IMPORT_CXX_SYMBOL_FUNCTIONS_H_

#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "loom/import/cxx/binding/intrinsics.h"
#include "loom/import/cxx/binding/launch.h"
#include "loom/import/cxx/source/locations.h"
#include "loom/import/cxx/value/types.h"

namespace loom::cxx_import {

enum class FunctionKind { Ordinary, Kernel };

// Admitted definition ready for recursive body construction. The source owns
// AST/types and the module owns native IR; both outlive this borrowed contract.
struct FunctionBody {
  // Definition whose source range diagnoses implicit-return admission.
  cxx::FunctionDefinitionAST* source;
  // Source statements to translate, without a second function-body lookup.
  cxx::CompoundStatementAST* body;
  // Native function or kernel definition that owns the body region.
  loom_op_t* operation;
  // Entry region with projected parameter types, ready for source bindings.
  loom_region_t* region;
  // Resolved source return type, including void.
  const cxx::Type* return_type;
  // Determines return terminators and workgroup-storage admission.
  FunctionKind kind;
};

// Owns root selection, native symbol identities and reachable function order.
// Binding admission sees every concrete declaration once during selection;
// concrete template instances are admitted when reached. The source and output
// module, along with their admission services, outlive this object.
class Functions {
 public:
  Functions(cxx::TranslationUnit& unit, Diagnostics& diagnostics,
            loom_module_t* module, Intrinsics& intrinsics,
            LaunchContracts& launches)
      : unit_(unit),
        diagnostics_(diagnostics),
        module_(module),
        intrinsics_(intrinsics),
        launches_(launches) {}

  // Selects explicit qualified roots or externally visible concrete
  // definitions. Called once before translating the pending worklist.
  void select(std::span<const iree_string_view_t> roots);
  // Retains one identity and queues a newly reached source function exactly
  // once.
  loom_symbol_ref_t declare(cxx::FunctionSymbol* function);
  // Discovery order grows as declare reaches helpers. Spans are invalidated by
  // growth; consumers obtain the next indexed entry after each body finishes.
  std::span<cxx::FunctionSymbol* const> pending() const { return pending_; }
  // Builds the selected native definition and launch contract at the caller's
  // module insertion point. The caller then enters and fills the returned body.
  FunctionBody define(cxx::FunctionSymbol* symbol, Types& types,
                      Locations& locations, loom_builder_t* builder);

 private:
  void collect(cxx::List<cxx::DeclarationAST*>* declarations,
               std::vector<cxx::FunctionSymbol*>& definitions);
  const std::string& qualified_name(cxx::FunctionSymbol* symbol);

  // Source declarations, semantic identities and namespace relationships.
  cxx::TranslationUnit& unit_;
  // Root/definition admission diagnostic boundary.
  Diagnostics& diagnostics_;
  // Owns output strings, symbols and definitions.
  loom_module_t* module_;
  // Retains generated operation bindings admitted while visiting declarations.
  Intrinsics& intrinsics_;
  // Retains merged launch contracts for definitions and concrete instances.
  LaunchContracts& launches_;
  // Selected externally visible definitions; other reached helpers stay
  // private.
  std::unordered_set<cxx::FunctionSymbol*> exported_;
  // Each reached source function receives one native identity.
  std::unordered_map<cxx::FunctionSymbol*, loom_symbol_ref_t> callees_;
  // Reachable worklist in deterministic discovery order.
  std::vector<cxx::FunctionSymbol*> pending_;
  // Ordinals disambiguate overloaded or colliding source spellings.
  std::unordered_map<std::string, unsigned> symbol_names_;
  // Source qualification retained once per reached function.
  std::unordered_map<cxx::FunctionSymbol*, std::string> qualified_names_;
};

}  // namespace loom::cxx_import

#endif  // LOOM_IMPORT_CXX_SYMBOL_FUNCTIONS_H_
