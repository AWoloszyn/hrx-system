// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_IMPORT_CXX_BINDING_SHAPED_H_
#define LOOM_IMPORT_CXX_BINDING_SHAPED_H_

#include <cxx/attributes.h>
#include <cxx/types.h>

#include <optional>
#include <span>

#include "loom/import/cxx/value/types.h"
#include "loom/ops/vector/ops.h"

namespace loom::cxx_import {

// An admitted register-vector intrinsic. Source signatures preserve the
// operation's heterogeneous shapes and element interpretations. Calls consume
// the retained result type and semantic kind without reexamining source types.
class ShapedIntrinsic {
 public:
  // Resolves a string-only loom::op attribute after raw attribute admission.
  // Unknown names return nullopt; recognized names with invalid declarations
  // diagnose at owner and throw SourceRejected. All source objects are borrowed
  // only for admission; the returned binding owns no frontend storage.
  static std::optional<ShapedIntrinsic> resolve(
      cxx::TranslationUnit& unit, Diagnostics& diagnostics, Types& types,
      const cxx::FunctionType* signature, const cxx::Attribute& attribute,
      cxx::AST* owner);

  // Emits the admitted operation using already-converted source arguments.
  loom_value_id_t call(std::span<const loom_value_id_t> arguments,
                       loom_builder_t* builder,
                       loom_location_id_t location) const;

  // Compares operation semantics for redeclarations of one canonical symbol.
  bool equivalent(const ShapedIntrinsic& other) const;

 private:
  enum class Operation { TableLookup, Dot4i };

  ShapedIntrinsic(Operation operation, loom_type_t result_type,
                  loom_vector_dot4i_kind_t dot_kind)
      : operation_(operation), result_type_(result_type), dot_kind_(dot_kind) {}

  // Operation family admitted from the source declaration.
  Operation operation_;
  // Exact source result shape and lane width.
  loom_type_t result_type_;
  // Explicit byte interpretation for dot products; unused for table lookup.
  loom_vector_dot4i_kind_t dot_kind_;
};

}  // namespace loom::cxx_import

#endif  // LOOM_IMPORT_CXX_BINDING_SHAPED_H_
