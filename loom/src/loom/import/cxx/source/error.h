// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_IMPORT_CXX_SOURCE_ERROR_H_
#define LOOM_IMPORT_CXX_SOURCE_ERROR_H_

#include <memory>
#include <utility>

#include "iree/base/status_cc.h"

namespace loom::cxx_import {

// Unwinds AST translation after its source diagnostic has been delivered.
struct SourceRejected {};

// Owns a fallible Loom builder, diagnostic sink, or source provider result
// while C++ unwinds. Exception copies share the original status, including its
// payloads, until the C entry point transfers it back to the caller.
class StatusError {
 public:
  explicit StatusError(iree_status_t status)
      : status_(
            std::make_shared<iree::Status>(iree::Status(std::move(status)))) {}

  iree_status_t release() { return status_->release(); }

 private:
  // Shared only on the exception path; copying never clones or drops payloads.
  // The temporary Status above also owns cleanup if allocation fails.
  std::shared_ptr<iree::Status> status_;
};

inline void check(iree_status_t status) {
  if (!iree_status_is_ok(status)) {
    throw StatusError(status);
  }
}

}  // namespace loom::cxx_import

#endif  // LOOM_IMPORT_CXX_SOURCE_ERROR_H_
