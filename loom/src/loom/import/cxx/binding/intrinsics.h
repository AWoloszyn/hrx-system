// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_IMPORT_CXX_BINDING_INTRINSICS_H_
#define LOOM_IMPORT_CXX_BINDING_INTRINSICS_H_

#include <cxx/attributes.h>
#include <cxx/symbols_fwd.h>

#include <optional>
#include <span>
#include <unordered_map>
#include <variant>

#include "loom/import/cxx/binding/scalar_bindings.h"
#include "loom/import/cxx/binding/shaped.h"
#include "loom/import/cxx/source/source.h"
#include "loom/import/cxx/value/types.h"

namespace loom::cxx_import {

// Resolves annotated source declarations against scalar or shaped operation
// contracts at declaration admission. Calls consume the retained binding
// directly; typed builders consume the resulting trusted signature.
class Intrinsics {
 public:
  Intrinsics(cxx::TranslationUnit& unit, Diagnostics& diagnostics, Types& types)
      : unit_(unit), diagnostics_(diagnostics), types_(types) {}

  // Admits raw attribute arguments before the frontend's string-only semantic
  // attribute map can erase unsupported arguments or duplicate bindings.
  void declaration(cxx::FunctionSymbol* function,
                   cxx::List<cxx::AttributeSpecifierAST*>* attributes,
                   cxx::AST* owner);

  std::optional<loom_value_id_t> call(
      cxx::FunctionSymbol* function, std::span<const loom_value_id_t> arguments,
      uint8_t math_flags, loom_builder_t* builder, loom_location_id_t location);

 private:
  struct ScalarBinding {
    // Immutable generated binding for this source declaration.
    const loom_cxx_scalar_binding_t* scalar;
    // Source-selected floating-point scalar representation.
    loom_type_t type;
    // Explicit source permissions in addition to invocation permissions.
    uint8_t flags = 0;

    bool equivalent(const ScalarBinding& other) const {
      return scalar == other.scalar && flags == other.flags &&
             loom_type_equal(type, other.type);
    }
  };
  using Binding = std::variant<ScalarBinding, ShapedIntrinsic>;

  Binding resolve(cxx::FunctionSymbol* function,
                  const cxx::Attribute& attribute, cxx::AST* owner);
  ScalarBinding resolve_scalar(const loom_cxx_scalar_binding_t* scalar,
                               const cxx::FunctionType* signature,
                               const cxx::Attribute& attribute,
                               cxx::AST* owner);

  // Invocation-owned frontend supplying canonical semantic types.
  cxx::TranslationUnit& unit_;
  // Invocation-owned diagnostic boundary for source rejection.
  Diagnostics& diagnostics_;
  // Invocation-owned source type projection shared with ordinary translation.
  Types& types_;
  // Validated bindings indexed by canonical semantic function symbol.
  std::unordered_map<cxx::FunctionSymbol*, Binding> bindings_;
};

}  // namespace loom::cxx_import

#endif  // LOOM_IMPORT_CXX_BINDING_INTRINSICS_H_
