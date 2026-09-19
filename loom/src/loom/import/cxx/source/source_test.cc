// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/source/source.h"

#include <cxx/ast.h>
#include <cxx/control.h>
#include <cxx/memory_layout.h>

#include <map>
#include <string>

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/import/cxx/source/error.h"

namespace loom::cxx_import {
namespace {

TEST(SourceTest, LayoutAndMutableSemanticStateBelongToEachSource) {
  loom_cxx_import_options_t options;
  loom_cxx_import_options_initialize(&options);
  Source first(IREE_SV("static_assert(sizeof(long) == 8); int value = 1;"),
               IREE_SV("first.cpp"), options);
  options.data_model = LOOM_CXX_DATA_MODEL_LLP64;
  Source second(IREE_SV("static_assert(sizeof(long) == 4); int value = 2;"),
                IREE_SV("second.cpp"), options);
  options.data_model = LOOM_CXX_DATA_MODEL_ILP32;
  Source third(IREE_SV("static_assert(sizeof(void*) == 4); int value = 3;"),
               IREE_SV("third.cpp"), options);
  EXPECT_EQ(first.unit().control()->memoryLayout()->sizeOfLong(), 8);
  EXPECT_EQ(second.unit().control()->memoryLayout()->sizeOfLong(), 4);
  EXPECT_EQ(third.unit().control()->memoryLayout()->sizeOfPointer(), 4);
  EXPECT_NE(first.unit().ast(), second.unit().ast());
  EXPECT_NE(first.unit().globalScope(), second.unit().globalScope());
}

TEST(SourceTest, ProviderBytesAreCopiedBeforeTheNextCallback) {
  struct Store {
    // Mutable provider buffer invalidated by every subsequent lookup.
    std::string scratch;
    // Candidate request counts, including missing paths.
    std::map<std::string, unsigned> requests;
  } store;
  loom_cxx_import_options_t options;
  loom_cxx_import_options_initialize(&options);
  options.source_provider = {
      [](void* user_data, iree_string_view_t path, bool* out_found,
         iree_string_view_t* out_source) {
        auto& store = *static_cast<Store*>(user_data);
        std::string filename(path.data, path.size);
        ++store.requests[filename];
        store.scratch.assign(256, '?');
        if (filename == "/include/first.h") {
          store.scratch =
              "#pragma once\n#include <second.h>\n"
              "constexpr int first = second + 1;\n";
        } else if (filename == "/include/second.h") {
          store.scratch = "#pragma once\nconstexpr int second = 41;\n";
        } else {
          *out_found = false;
          *out_source = iree_string_view_empty();
          return iree_ok_status();
        }
        *out_found = true;
        *out_source = view(store.scratch);
        return iree_ok_status();
      },
      &store};
  iree_string_view_t directory = IREE_SV("/include");
  options.include_paths = &directory;
  options.include_path_count = 1;
  Source source(
      IREE_SV("#include <first.h>\n#include <first.h>\n"
              "#if __has_include(<missing.h>)\n#error missing\n#endif\n"
              "#if __has_include(<missing.h>)\n#error missing\n#endif\n"
              "static_assert(first == 42);\n"),
      IREE_SV("/app/source.cpp"), options);
  store.scratch.clear();
  ASSERT_NE(source.unit().ast(), nullptr);
  EXPECT_EQ(store.requests.at("/include/first.h"), 1u);
  EXPECT_EQ(store.requests.at("/include/second.h"), 1u);
  EXPECT_EQ(store.requests.at("/include/missing.h"), 1u);
}

TEST(SourceTest, RejectedSourceLeavesTheNextInvocationIndependent) {
  loom_cxx_import_options_t options;
  loom_cxx_import_options_initialize(&options);
  EXPECT_THROW(Source(IREE_SV("int invalid = ;"), IREE_SV("bad.cpp"), options),
               SourceRejected);
  Source source(IREE_SV("int valid = 7;"), IREE_SV("good.cpp"), options);
  EXPECT_FALSE(source.diagnostics().has_error());
  ASSERT_NE(source.unit().ast(), nullptr);
}

TEST(SourceTest, DiagnosticSinkFailureCrossesTheParserSafeBoundary) {
  loom_cxx_import_options_t options;
  loom_cxx_import_options_initialize(&options);
  options.diagnostic_sink = {[](void*, const loom_diagnostic_t*) {
                               return iree_make_status(IREE_STATUS_CANCELLED,
                                                       "consumer stopped");
                             },
                             nullptr};
  try {
    Source source(IREE_SV("int invalid = ;"), IREE_SV("bad.cpp"), options);
    FAIL() << "Expected the retained diagnostic sink failure";
  } catch (StatusError& error) {
    IREE_EXPECT_STATUS_IS(IREE_STATUS_CANCELLED, error.release());
  }
}

}  // namespace
}  // namespace loom::cxx_import
