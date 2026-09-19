// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_IMPORT_CXX_VALUE_VECTOR_H_
#define LOOM_IMPORT_CXX_VALUE_VECTOR_H_

#include "loom/import/cxx/value/scalar.h"

namespace loom::cxx_import {

// Projects explicit source vector operations, without scalar integer promotion
// or automatic vectorization. Operands are evaluated by the caller. Source
// signedness selects lane operations; comparison results retain C++'s
// full-width integer masks rather than exposing Loom's internal i1 predicates.
class Vectors {
 public:
  Vectors(cxx::TranslationUnit& unit, Diagnostics& diagnostics, Types& types,
          Scalars& scalars, Locations& locations, loom_builder_t& builder)
      : unit_(unit),
        diagnostics_(diagnostics),
        types_(types),
        scalars_(scalars),
        locations_(locations),
        builder_(builder) {}

  // Applies a resolved vector splat or equal-width bit reinterpretation.
  // Numeric lane conversion is a different source operation and is not inferred
  // here.
  loom_value_id_t convert(loom_value_id_t value, const cxx::Type* input_type,
                          const cxx::Type* output_type, cxx::AST* owner);
  // Operands already have the source operation's common vector type.
  loom_value_id_t binary(cxx::TokenKind token, loom_value_id_t left,
                         loom_value_id_t right, const cxx::Type* input_type,
                         const cxx::Type* output_type, cxx::AST* owner);
  // Preserves vector lane width for sign, complement and logical negation.
  loom_value_id_t unary(cxx::TokenKind token, loom_value_id_t value,
                        const cxx::Type* input_type,
                        const cxx::Type* output_type, cxx::AST* owner);

 private:
  // Resolved frontend types and object layout.
  cxx::TranslationUnit& unit_;
  // Source admission boundary.
  Diagnostics& diagnostics_;
  // Validated source vector representation.
  Types& types_;
  // Scalar conversion of a splatted lane.
  Scalars& scalars_;
  // Source provenance retained in the output module.
  Locations& locations_;
  // Borrowed insertion point owned by the AST driver.
  loom_builder_t& builder_;
};

}  // namespace loom::cxx_import

#endif  // LOOM_IMPORT_CXX_VALUE_VECTOR_H_
