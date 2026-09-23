// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_IMPORT_CXX_VALUE_BITCAST_H_
#define LOOM_IMPORT_CXX_VALUE_BITCAST_H_

#include "loom/import/cxx/value/types.h"

namespace loom::cxx_import {

// An admitted bit reinterpretation between equal-width scalar/vector values.
// Resolve before evaluating the operand: pointers and aggregates may have the
// same object size but do not carry their object representation in one SSA
// value. Vector lane zero supplies the least-significant bits, matching the
// source object layout used by existing vector-to-vector bit casts.
class BitCast {
 public:
  BitCast(cxx::TranslationUnit& unit, Diagnostics& diagnostics, Types& types,
          const cxx::Type* input_type, const cxx::Type* output_type,
          cxx::AST* owner);

  // Reinterprets an already-evaluated operand using ordinary scalar/vector ops.
  // Mixed shapes use a one-lane vector at the scalar endpoint, without memory
  // or numeric conversion. The result belongs to the builder's module.
  loom_value_id_t build(loom_builder_t& builder, loom_value_id_t value,
                        loom_location_id_t location) const;

 private:
  // Admitted source SSA representation.
  loom_type_t input_;
  // Admitted destination SSA representation with the same total bit count.
  loom_type_t output_;
};

}  // namespace loom::cxx_import

#endif  // LOOM_IMPORT_CXX_VALUE_BITCAST_H_
