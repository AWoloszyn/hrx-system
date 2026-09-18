// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/import.h"

#include <map>
#include <string>

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/format/text/printer.h"
#include "loom/ops/op_registry.h"

namespace {

class ImportTest : public ::testing::Test {
 protected:
  void SetUp() override {
    iree_arena_block_pool_initialize(64 * 1024, iree_allocator_system(),
                                     &pool_);
    loom_context_initialize(iree_allocator_system(), &context_);
    IREE_ASSERT_OK(loom_op_registry_register_all_dialects(&context_));
    IREE_ASSERT_OK(loom_context_finalize(&context_));
    loom_cxx_import_options_initialize(&options_);
    options_.diagnostic_sink = {CaptureDiagnostic, this};
  }

  void TearDown() override {
    loom_module_free(module_);
    loom_context_deinitialize(&context_);
    iree_arena_block_pool_deinitialize(&pool_);
  }

  iree_status_t Import(iree_string_view_t source) {
    loom_module_free(module_);
    module_ = nullptr;
    return loom_cxx_import(source, IREE_SV("/app/source.cpp"), &context_,
                           &pool_, &options_, iree_allocator_system(),
                           &module_);
  }

  std::string Print() {
    iree_string_builder_t builder;
    iree_string_builder_initialize(iree_allocator_system(), &builder);
    IREE_EXPECT_OK(loom_text_print_module_to_builder(module_, &builder, 0));
    std::string result(iree_string_builder_buffer(&builder),
                       iree_string_builder_size(&builder));
    iree_string_builder_deinitialize(&builder);
    return result;
  }

  static iree_status_t CaptureDiagnostic(void* user_data,
                                         const loom_diagnostic_t* diagnostic) {
    auto& self = *static_cast<ImportTest*>(user_data);
    ++self.diagnostic_count_;
    auto filename = diagnostic->source_location.filename;
    self.diagnostic_filename_.assign(filename.data ? filename.data : "",
                                     filename.size);
    auto source = diagnostic->source_location.source;
    self.diagnostic_source_.assign(source.data ? source.data : "", source.size);
    return iree_ok_status();
  }

  static iree_status_t ProvideSource(void* user_data, iree_string_view_t path,
                                     bool* out_found,
                                     iree_string_view_t* out_source) {
    auto& self = *static_cast<ImportTest*>(user_data);
    auto key = std::string(path.data, path.size);
    ++self.source_requests_[key];
    auto found = self.headers_.find(key);
    *out_found = found != self.headers_.end();
    if (*out_found) {
      *out_source =
          iree_make_string_view(found->second.data(), found->second.size());
    }
    return iree_ok_status();
  }

  // Shared native dialect context, finalized before import.
  loom_context_t context_ = {};
  // Arena storage outliving every imported module.
  iree_arena_block_pool_t pool_ = {};
  // Owned import output released before its context and pool.
  loom_module_t* module_ = nullptr;
  // Per-invocation source configuration.
  loom_cxx_import_options_t options_ = {};
  // Number of callbacks delivered by the source diagnostic boundary.
  int diagnostic_count_ = 0;
  // Copied source identity retained after the frontend is destroyed.
  std::string diagnostic_filename_;
  // Copied source bytes retained after the frontend is destroyed.
  std::string diagnostic_source_;
  // Provider-owned header text, independent of frontend storage.
  std::map<std::string, std::string> headers_;
  // Lookup counts witness per-invocation source reuse.
  std::map<std::string, int> source_requests_;
};

TEST_F(ImportTest, FunctionsAndRootsOutliveSource) {
  std::string source =
      "static int helper(int x) { return x + 1; }\n"
      "int first(int x) { return helper(x); }\n"
      "int second(int x) { return x * 2; }\n";
  iree_string_view_t root = IREE_SV("first");
  options_.roots = &root;
  options_.root_count = 1;
  IREE_ASSERT_OK(Import(iree_make_string_view(source.data(), source.size())));
  ASSERT_NE(module_, nullptr);
  source.assign(source.size(), '?');
  auto text = Print();
  EXPECT_NE(text.find("func.def public @first"), std::string::npos);
  EXPECT_NE(text.find("func.def @helper"), std::string::npos);
  EXPECT_EQ(text.find("@second"), std::string::npos);
  EXPECT_EQ(diagnostic_count_, 0);
}

TEST_F(ImportTest, MultipleKernelsAndSymbolicLaunch) {
  IREE_ASSERT_OK(
      Import(IREE_SV("[[loom::kernel, loom::workgroup_size(64, 1, 1), "
                     "loom::workgroup_count(2, 1, 1)]] void first() {}\n"
                     "[[loom::kernel]] void second() {}\n")));
  ASSERT_NE(module_, nullptr);
  auto text = Print();
  EXPECT_NE(text.find("kernel.def @first"), std::string::npos);
  EXPECT_NE(text.find("kernel.def @second"), std::string::npos);
  EXPECT_NE(text.find("config.decl @second.workgroup_size.x"),
            std::string::npos);
  EXPECT_EQ(text.find("config.decl @first"), std::string::npos);
}

TEST_F(ImportTest, HeaderProviderUsesNormalIncludeSearch) {
  const iree_string_view_t paths[] = {IREE_SV("/overrides"),
                                      IREE_SV("/facade")};
  options_.include_paths = paths;
  options_.include_path_count = IREE_ARRAYSIZE(paths);
  options_.source_provider = {ProvideSource, this};
  headers_["/overrides/value.h"] = "#pragma once\n#include_next <value.h>\n";
  headers_["/facade/value.h"] =
      "#pragma once\n#include \"factor.h\"\n"
      "template<int N> int scale(int x) { return x * N; }\n";
  headers_["/facade/factor.h"] = "#define FACTOR 3\n";
  IREE_ASSERT_OK(Import(
      IREE_SV("#include <value.h>\n#include <value.h>\n"
              "#if !__has_include(<value.h>) || __has_include(<absent.h>)\n"
              "#error include lookup failed\n#endif\n"
              "int entry(int x) { return scale<FACTOR>(x); }\n")));
  ASSERT_NE(module_, nullptr);
  headers_.clear();
  EXPECT_NE(Print().find("@scale_3"), std::string::npos);
  EXPECT_EQ(source_requests_["/overrides/value.h"], 1);
  EXPECT_EQ(source_requests_["/facade/value.h"], 1);
  EXPECT_EQ(source_requests_["/facade/factor.h"], 1);
}

TEST_F(ImportTest, RejectedSourceDoesNotPoisonNextImport) {
  IREE_ASSERT_OK(Import(IREE_SV("int broken( {")));
  EXPECT_EQ(module_, nullptr);
  EXPECT_GT(diagnostic_count_, 0);
  EXPECT_EQ(diagnostic_filename_, "/app/source.cpp");
  EXPECT_EQ(diagnostic_source_, "int broken( {");
  IREE_ASSERT_OK(Import(IREE_SV("int repaired() { return 7; }")));
  ASSERT_NE(module_, nullptr);
  EXPECT_NE(Print().find("@repaired"), std::string::npos);
}

TEST_F(ImportTest, UnsupportedSourceHasDiagnosticInsteadOfInvalidModule) {
  IREE_ASSERT_OK(
      Import(IREE_SV("int entry(int x) { while (x) --x; return x; }")));
  EXPECT_EQ(module_, nullptr);
  EXPECT_EQ(diagnostic_count_, 1);
  EXPECT_EQ(diagnostic_filename_, "/app/source.cpp");
}

TEST_F(ImportTest, SinkFailurePropagates) {
  options_.diagnostic_sink = {[](void*, const loom_diagnostic_t*) {
                                return iree_make_status(
                                    IREE_STATUS_CANCELLED,
                                    "diagnostic consumer stopped");
                              },
                              nullptr};
  IREE_EXPECT_STATUS_IS(IREE_STATUS_CANCELLED,
                        Import(IREE_SV("int broken( {")));
  EXPECT_EQ(module_, nullptr);
}

TEST_F(ImportTest, ProviderFailurePropagates) {
  options_.source_provider = {
      [](void*, iree_string_view_t, bool*, iree_string_view_t*) {
        return iree_make_status(IREE_STATUS_UNAVAILABLE,
                                "source store unavailable");
      },
      nullptr};
  IREE_EXPECT_STATUS_IS(IREE_STATUS_UNAVAILABLE,
                        Import(IREE_SV("#include \"value.h\"\n")));
  EXPECT_EQ(module_, nullptr);
}

TEST_F(ImportTest, StandardSelectionUsesFrontendMacros) {
  options_.standard = IREE_SV("c23");
  IREE_ASSERT_OK(Import(
      IREE_SV("#if __STDC_VERSION__ != 202311L\n#error wrong standard\n#endif\n"
              "int entry(void) { return 1; }\n")));
  ASSERT_NE(module_, nullptr);
  options_.standard = IREE_SV("c++17");
  IREE_ASSERT_OK(Import(
      IREE_SV("#if __cplusplus != 201703L\n#error wrong standard\n#endif\n"
              "int entry() { return 1; }\n")));
  ASSERT_NE(module_, nullptr);
  options_.standard = IREE_SV("unknown");
  IREE_EXPECT_STATUS_IS(IREE_STATUS_INVALID_ARGUMENT, Import(IREE_SV("")));
}

TEST_F(ImportTest, PromotionsAndAssignmentConversionsProduceVerifiedIR) {
  IREE_ASSERT_OK(Import(IREE_SV(
      "unsigned long long shift(unsigned long long x, unsigned count) { return "
      "x << count; }\n"
      "unsigned char narrow(unsigned char x) { x += 300; ++x; return x; }\n"
      "unsigned long long increment(unsigned long long x) { ++x; return x; }\n"
      "bool truth(double x) { return bool(x); }\n"
      "double widen(bool x) { return double(x); }\n")));
  ASSERT_NE(module_, nullptr);
  EXPECT_EQ(diagnostic_count_, 0);
}

TEST_F(ImportTest, SourceDataModelControlsLongWidth) {
  options_.data_model = LOOM_CXX_DATA_MODEL_LP64;
  IREE_ASSERT_OK(Import(IREE_SV("long identity(long x) { return x; }")));
  ASSERT_NE(module_, nullptr);
  EXPECT_NE(Print().find("%x: i64"), std::string::npos);
  options_.data_model = LOOM_CXX_DATA_MODEL_LLP64;
  IREE_ASSERT_OK(Import(IREE_SV("long identity(long x) { return x; }")));
  ASSERT_NE(module_, nullptr);
  EXPECT_NE(Print().find("%x: i32"), std::string::npos);
}

TEST_F(ImportTest, VoidHelpersAndVisibility) {
  IREE_ASSERT_OK(
      Import(IREE_SV("[[gnu::visibility(\"hidden\")]] void helper(float* p) { "
                     "p[0u] = 1.0f; }\n"
                     "void entry(float* p) { helper(p); }\n")));
  ASSERT_NE(module_, nullptr);
  auto text = Print();
  EXPECT_NE(text.find("func.def public @entry"), std::string::npos);
  EXPECT_NE(text.find("func.def @helper"), std::string::npos);
}

TEST_F(ImportTest, GlobalConstantsAndUnsupportedStorage) {
  IREE_ASSERT_OK(Import(IREE_SV(
      "constexpr int factor = 3; int entry(int x) { return x * factor; }\n"
      "constexpr unsigned mask = 0xffffffffu; unsigned bits() { return mask; "
      "}\n"
      "constexpr unsigned char byte = 255; unsigned char small() { return "
      "byte; }\n")));
  ASSERT_NE(module_, nullptr);
  IREE_ASSERT_OK(Import(IREE_SV("int global = 3; int entry() { return 1; }")));
  EXPECT_EQ(module_, nullptr);
  EXPECT_EQ(diagnostic_count_, 1);
}

TEST_F(ImportTest, ExternalStorageCannotBecomeAnAutomaticBinding) {
  IREE_ASSERT_OK(
      Import(IREE_SV("extern int counter; void entry() { counter = 3; }")));
  EXPECT_EQ(module_, nullptr);
  EXPECT_EQ(diagnostic_count_, 1);
}

TEST_F(ImportTest, WideLoopComparisonRetainsInductionWraparound) {
  IREE_ASSERT_OK(Import(IREE_SV(
      "unsigned entry(unsigned long long upper) { unsigned sum = 0; "
      "for (unsigned i = 0; i < upper; ++i) { sum += i; } return sum; }")));
  ASSERT_NE(module_, nullptr);
  auto text = Print();
  EXPECT_NE(text.find("scf.while"), std::string::npos);
  EXPECT_EQ(text.find("scf.for"), std::string::npos);
}

}  // namespace
