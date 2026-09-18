// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/include_catalog.h"

#if LOOM_CXX_EMBED_INCLUDES
#include "loom/import/cxx/embedded_hip_headers.h"
#include "loom/import/cxx/embedded_loomcxx_headers.h"
#endif

namespace loom::cxx_import {

std::string_view builtin_include_root() {
#if LOOM_CXX_EMBED_INCLUDES
  return "/__loomcxx_builtin__";
#else
  return {};
#endif
}

std::optional<std::string_view> builtin_include(std::string_view path) {
#if LOOM_CXX_EMBED_INCLUDES
  const iree_file_toc_t* entries = nullptr;
  if (path.substr(0, 8) == "loomcxx/") {
    path.remove_prefix(8);
    entries = loom_cxx_embedded_loomcxx_headers_create();
  } else if (path.substr(0, 4) == "hip/") {
    path.remove_prefix(4);
    entries = loom_cxx_embedded_hip_headers_create();
  }
  if (entries) {
    for (auto* entry = entries; entry->name; ++entry) {
      if (path == entry->name) {
        return std::string_view(entry->data, entry->size);
      }
    }
  }
#else
  (void)path;
#endif
  return std::nullopt;
}

}  // namespace loom::cxx_import
