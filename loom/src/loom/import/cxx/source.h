// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_IMPORT_CXX_SOURCE_H_
#define LOOM_IMPORT_CXX_SOURCE_H_

#include <cxx/diagnostics_client.h>
#include <cxx/toolchain.h>
#include <cxx/translation_unit.h>

#include <string>
#include <string_view>

#include "loom/import/cxx/import.h"

namespace loom::cxx_import {

inline iree_string_view_t view(std::string_view value) {
  return iree_make_string_view(value.data(), value.size());
}

inline std::string string(iree_string_view_t value) {
  return value.size ? std::string(value.data, value.size) : std::string();
}

// Forwards frontend diagnostics without invoking its process-exiting renderer.
// Source bytes in each callback belong to the invocation's preprocessor.
class Diagnostics final : public cxx::DiagnosticsClient {
 public:
  explicit Diagnostics(loom_diagnostic_sink_t sink) : sink_(sink) {}
  ~Diagnostics() override { iree_status_free(status_); }
  void bind(cxx::Preprocessor* preprocessor) { preprocessor_ = preprocessor; }
  void report(const cxx::Diagnostic& diagnostic) override;
  [[noreturn]] void reject(cxx::TranslationUnit& unit, cxx::AST* ast,
                           std::string_view message);
  bool has_error() const { return has_error_; }
  // Propagates sink failure and source rejection at a parser-safe boundary.
  // cxx forwards diagnostics from noexcept destructors, so report never throws.
  void finish();

 private:
  void emit(const cxx::Token& token, loom_diagnostic_severity_t severity,
            const loom_error_def_t* error, std::string_view message);

  // Borrowed callback and state supplied by the current import request.
  loom_diagnostic_sink_t sink_;
  // Borrowed source storage used to materialize byte ranges.
  cxx::Preprocessor* preprocessor_ = nullptr;
  // Forwarded diagnostics can bypass cxx's counter; this owns admission.
  bool has_error_ = false;
  // First sink failure, retained until a safe point can unwind the frontend.
  iree_status_t status_ = iree_ok_status();
};

// Preprocesses and type-checks one source configuration. The unit owns all
// mutable AST state; the returned toolchain owns its borrowed memory layout.
std::unique_ptr<cxx::Toolchain> parse_source(
    cxx::TranslationUnit& unit, Diagnostics& diagnostics,
    iree_string_view_t source, iree_string_view_t filename,
    const loom_cxx_import_options_t& options);

}  // namespace loom::cxx_import

#endif  // LOOM_IMPORT_CXX_SOURCE_H_
