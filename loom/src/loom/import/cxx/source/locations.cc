// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/source/locations.h"

#include <cxx/ast.h>

#include "loom/import/cxx/source/error.h"

namespace loom::cxx_import {

loom_location_id_t Locations::get(cxx::AST* ast) {
  if (auto found = locations_.find(ast); found != locations_.end()) {
    return found->second;
  }
  auto first = unit_.tokenStartPosition(ast->firstSourceLocation());
  auto last = unit_.tokenEndPosition(ast->lastSourceLocation().previous());
  // Instantiation can synthesize literals without source tokens. Keep those
  // locations unknown instead of inventing an empty filename at line zero.
  if (first.fileName.empty() || !first.line || !first.column) {
    locations_[ast] = LOOM_LOCATION_UNKNOWN;
    return LOOM_LOCATION_UNKNOWN;
  }
  if (first.line > UINT16_MAX || last.line > UINT16_MAX ||
      first.column > UINT16_MAX || last.column > UINT16_MAX) {
    diagnostics_.reject(unit_, ast,
                        "source range exceeds Loom's location representation");
  }
  loom_source_id_t source;
  check(loom_module_register_source(
      module_,
      iree_make_string_view(first.fileName.data(), first.fileName.size()),
      &source));
  loom_location_id_t result;
  check(loom_module_add_location(
      module_,
      loom_location_file_range(source, first.line, first.column, last.line,
                               last.column),
      &result));
  locations_[ast] = result;
  return result;
}

}  // namespace loom::cxx_import
