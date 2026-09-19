// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_IMPORT_CXX_SOURCE_ERROR_H_
#define LOOM_IMPORT_CXX_SOURCE_ERROR_H_

#include <utility>

#include "iree/base/api.h"

namespace loom::cxx_import {

// Unwinds AST translation after its source diagnostic has been delivered.
struct SourceRejected {};

// Owns a fallible Loom builder, diagnostic sink, or source provider result
// while C++ unwinds. The C entry point transfers it back to the caller.
class StatusError {
 public:
  explicit StatusError(iree_status_t status) : status_(status) {}
  StatusError(const StatusError&) = delete;
  StatusError(StatusError&& other) noexcept : status_(other.release()) {}
  ~StatusError() { iree_status_free(status_); }

  iree_status_t release() { return std::exchange(status_, iree_ok_status()); }

 private:
  // Owned until released at the C API boundary.
  iree_status_t status_;
};

inline void check(iree_status_t status) {
  if (!iree_status_is_ok(status)) {
    throw StatusError(status);
  }
}

}  // namespace loom::cxx_import

#endif  // LOOM_IMPORT_CXX_SOURCE_ERROR_H_
