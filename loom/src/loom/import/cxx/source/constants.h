// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_IMPORT_CXX_SOURCE_CONSTANTS_H_
#define LOOM_IMPORT_CXX_SOURCE_CONSTANTS_H_

#include <cxx/ast_fwd.h>
#include <cxx/cxx_fwd.h>

#include <cstdint>
#include <optional>

namespace loom::cxx_import {

// Evaluates the pure integer grammar used by source bounds. Source conversions
// and arithmetic widths are preserved by cxx. Calls, overloaded operations,
// mutation, and runtime or volatile reads are excluded even in unselected arms.
// Retained constants and unevaluated sizeof/alignof operands are leaves.
// Returns no value for unsupported expressions, invalid constant arithmetic,
// or a final value outside the signed intmax_t range.
// The owning source analysis evaluates each bound once and retains the result;
// downstream consumers do not repeat admission or evaluation.
std::optional<std::intmax_t> integer_constant(cxx::TranslationUnit& unit,
                                              cxx::ExpressionAST* expression);

}  // namespace loom::cxx_import

#endif  // LOOM_IMPORT_CXX_SOURCE_CONSTANTS_H_
