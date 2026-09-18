// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_IMPORT_CXX_INCLUDE_CATALOG_H_
#define LOOM_IMPORT_CXX_INCLUDE_CATALOG_H_

#include <optional>
#include <string_view>

namespace loom::cxx_import {

// Virtual system include root for the immutable facade catalog. Empty when the
// binary was built without embedded include contents.
std::string_view builtin_include_root();

// Looks up a catalog-relative path such as hip/hip_runtime.h. Returned bytes
// have process lifetime and may be used concurrently without synchronization.
std::optional<std::string_view> builtin_include(std::string_view path);

}  // namespace loom::cxx_import

#endif  // LOOM_IMPORT_CXX_INCLUDE_CATALOG_H_
