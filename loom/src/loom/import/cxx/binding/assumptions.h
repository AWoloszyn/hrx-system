// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_IMPORT_CXX_BINDING_ASSUMPTIONS_H_
#define LOOM_IMPORT_CXX_BINDING_ASSUMPTIONS_H_

#include <cstdint>
#include <vector>

#include "loom/import/cxx/source/source.h"

namespace loom::cxx_import {

// One admitted unsigned bound. AST references borrow the owning Source.
struct AssumptionBound {
  // Source identifier whose current SSA value receives the refinement.
  cxx::IdExpressionAST* binding;
  // Comparison operand retaining the frontend's integral promotions. The
  // nonnegative range applies in this type before conversion back to binding.
  cxx::ExpressionAST* value;
  // Positive exclusive upper bound representable in signed i32.
  int32_t upper_bound;
};

// Admits an assume-annotated call as a conjunction of unsigned binding < bound
// predicates, in source order. Bounds use pure integer constant expressions;
// no condition or bound expression is evaluated at runtime. Reports source
// diagnostics and throws SourceRejected before returning any records if a
// predicate is unsupported. Consumers publish refinements only after admission.
std::vector<AssumptionBound> assumption_bounds(cxx::TranslationUnit& unit,
                                               Diagnostics& diagnostics,
                                               cxx::CallExpressionAST* call);

}  // namespace loom::cxx_import

#endif  // LOOM_IMPORT_CXX_BINDING_ASSUMPTIONS_H_
