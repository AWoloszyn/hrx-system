// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/import/cxx/tooling/input.h"

#include <string>

#include "iree/io/file_contents.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "iree/testing/temp_file.h"
#include "loom/import/cxx/import.h"
#include "loom/import/cxx/source/catalog.h"
#include "loom/ops/op_registry.h"
#include "loom/tools/loom-check/file.h"
#include "loom/tools/loom-check/test_util.h"

namespace {

iree_string_view_t View(const std::string& value) {
  return iree_make_string_view(value.data(), value.size());
}

std::string String(iree_string_view_t value) {
  return std::string(value.data ? value.data : "", value.size);
}

iree_status_t RegisterContext(void*, loom_context_t* context) {
  return loom_op_registry_register_all_dialects(context);
}

iree_status_t EmptyLowRegistry(
    void*, loom_target_low_descriptor_registry_t* registry) {
  *registry = {};
  return iree_ok_status();
}

const loom_input_provider_t* const kInputs[] = {&loom_cxx_input_provider};

class InputTest : public ::testing::Test {
 protected:
  void SetUp() override {
    iree_arena_block_pool_initialize(64 * 1024, iree_allocator_system(),
                                     &pool_);
    loom_context_initialize(iree_allocator_system(), &context_);
    IREE_ASSERT_OK(RegisterContext(nullptr, &context_));
    IREE_ASSERT_OK(loom_context_finalize(&context_));
    environment_.input_providers = {kInputs, IREE_ARRAYSIZE(kInputs)};
    environment_.register_context.fn = RegisterContext;
    environment_.initialize_low_descriptor_registry.fn = EmptyLowRegistry;
  }

  void TearDown() override {
    loom_input_module_deinitialize(&input_);
    loom_context_deinitialize(&context_);
    iree_arena_block_pool_deinitialize(&pool_);
  }

  iree_status_t Load(const loom_input_request_t& request) {
    loom_input_module_deinitialize(&input_);
    return loom_input_module_load(&loom_cxx_input_provider, &request, &context_,
                                  &pool_, iree_allocator_system(), &input_);
  }

  iree_status_t Write(const std::string& path, const std::string& contents) {
    return iree_io_file_contents_write(
        View(path), iree_make_const_byte_span(contents.data(), contents.size()),
        iree_allocator_system());
  }

  std::string Read(const std::string& path) {
    iree_io_file_contents_t* contents = nullptr;
    IREE_EXPECT_OK(iree_io_file_contents_read(
        View(path), iree_allocator_system(), &contents));
    if (!contents) {
      return {};
    }
    std::string result(
        reinterpret_cast<const char*>(contents->const_buffer.data),
        contents->const_buffer.data_length);
    iree_io_file_contents_free(contents);
    return result;
  }

  // Module and snapshot allocation pool, released last.
  iree_arena_block_pool_t pool_ = {};
  // Finalized production dialect context.
  loom_context_t context_ = {};
  // Loaded module and retained sources under test.
  loom_input_module_t input_ = {};
  // Generic check runner with the C++ input contribution.
  loom_check_environment_t environment_ = {};
};

TEST_F(InputTest, HeaderSnapshotsSurviveFrontendAndFilesystemChanges) {
  iree::testing::TempFilePath header("loom_input_header", ".h");
  iree::testing::TempFilePath main("loom_input_source", ".cxx-test");
  const std::string header_source =
      "static int helper(int value) { return value * 2; }\n";
  IREE_ASSERT_OK(Write(header.path(), header_source));
  const std::string::size_type separator = header.path().find_last_of("/\\");
  const std::string directory = header.path().substr(0, separator);
  std::string source = "#include \"" + header.path().substr(separator + 1) +
                       "\"\nint entry(int value) { return helper(value); }\n";
  const std::string original_source = source;
  const std::string map = directory + "=/logical";
  const iree_string_view_t maps[] = {View(map)};
  loom_input_request_t request = {};
  request.source = View(source);
  request.path = main.path_view();
  request.options = IREE_SV("root=entry");
  request.source_path_options.prefix_maps = {IREE_ARRAYSIZE(maps), maps};
  IREE_ASSERT_OK(Load(request));
  ASSERT_NE(input_.module, nullptr);
  source.assign(source.size(), '?');
  IREE_ASSERT_OK(Write(header.path(), "#error source changed after import\n"));

  auto resolver = loom_input_module_source_resolver(&input_);
  bool saw_main = false;
  bool saw_header = false;
  for (iree_host_size_t i = 0; i < input_.module->locations.count; ++i) {
    loom_source_range_t range = {};
    if (!loom_source_resolve(resolver, input_.module, (loom_location_id_t)i,
                             &range)) {
      continue;
    }
    EXPECT_EQ(range.provenance, LOOM_SOURCE_PROVENANCE_EXACT_SOURCE);
    EXPECT_EQ(String(range.filename).find("/logical/"), 0u);
    const std::string text = String(range.source);
    if (text == header_source) {
      saw_header = true;
      EXPECT_EQ(range.start_line, 1u);
      EXPECT_LT(range.start, range.end);
      EXPECT_LE(range.end, text.size());
    } else if (text == original_source) {
      saw_main = true;
      EXPECT_EQ(range.start_line, 2u);
    } else {
      ADD_FAILURE() << "resolver lost the admitted source snapshot";
    }
  }
  EXPECT_TRUE(saw_main);
  EXPECT_TRUE(saw_header);
}

TEST_F(InputTest, RemappingCannotMergeSourceIdentities) {
  iree::testing::TempFilePath header("loom_input_collision", ".h");
  IREE_ASSERT_OK(Write(header.path(), "int helper() { return 1; }\n"));
  const std::string source = "#include \"" + header.path() + "\"\n";
  const std::string header_map = header.path() + "=same";
  const iree_string_view_t maps[] = {IREE_SV("main.cxx-test=same"),
                                     View(header_map)};
  loom_input_request_t request = {};
  request.source = View(source);
  request.path = IREE_SV("main.cxx-test");
  request.source_path_options.prefix_maps = {IREE_ARRAYSIZE(maps), maps};
  IREE_EXPECT_STATUS_IS(IREE_STATUS_INVALID_ARGUMENT, Load(request));
  EXPECT_EQ(input_.module, nullptr);
}

TEST_F(InputTest, OptionsAndFailuresUseNativeAdmission) {
  loom_input_request_t request = {};
  request.path = IREE_SV("source.cxx-test");
  request.source = IREE_SV(
      "static_assert(sizeof(long) == 4); int entry() { return VALUE; }\n"
      "int unused() { return 9; }\n");
  request.options =
      IREE_SV("std=c++23 data-model=llp64 root=entry D='VALUE=7'");
  IREE_ASSERT_OK(Load(request));
  ASSERT_NE(input_.module, nullptr);
  for (auto options : {IREE_SV("std=unsupported"), IREE_SV("root='entry"),
                       IREE_SV("unknown=true"), IREE_SV("data-model=unknown"),
                       IREE_SV("builtin-includes=maybe")}) {
    request.options = options;
    IREE_EXPECT_STATUS_IS(IREE_STATUS_INVALID_ARGUMENT, Load(request));
    EXPECT_EQ(input_.module, nullptr);
  }
}

TEST_F(InputTest, NativeSourceObserverCoversBuiltinsAndPropagatesFailure) {
  loom_cxx_import_options_t options;
  loom_cxx_import_options_initialize(&options);
  struct Capture {
    // Whether the main translation unit was observed.
    bool main = false;
    // Whether an embedded facade header was observed.
    bool header = false;
  } capture;
  options.source_observer = {
      [](void* user_data, iree_string_view_t filename,
         iree_string_view_t source) {
        auto* capture = static_cast<Capture*>(user_data);
        if (iree_string_view_equal(filename, IREE_SV("source.cxx-test"))) {
          capture->main = true;
        } else if (iree_string_view_ends_with(filename,
                                              IREE_SV("loomcxx/scalar.h"))) {
          capture->header = !iree_string_view_is_empty(source);
        }
        return iree_ok_status();
      },
      &capture};
  const bool has_builtins = !loom::cxx_import::builtin_include_root().empty();
  IREE_ASSERT_OK(loom_cxx_import(
      has_builtins
          ? IREE_SV("#include <loomcxx/scalar.h>\nint entry() { return 1; }\n")
          : IREE_SV("int entry() { return 1; }\n"),
      IREE_SV("source.cxx-test"), &context_, &pool_, &options,
      iree_allocator_system(), &input_.module));
  ASSERT_NE(input_.module, nullptr);
  EXPECT_TRUE(capture.main);
  EXPECT_EQ(capture.header, has_builtins);
  loom_module_free(input_.module);
  input_.module = nullptr;
  options.source_observer.fn = [](void*, iree_string_view_t,
                                  iree_string_view_t) {
    return iree_status_from_code(IREE_STATUS_RESOURCE_EXHAUSTED);
  };
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_RESOURCE_EXHAUSTED,
      loom_cxx_import(IREE_SV("int entry() { return 1; }"),
                      IREE_SV("source.cxx-test"), &context_, &pool_, &options,
                      iree_allocator_system(), &input_.module));
  EXPECT_EQ(input_.module, nullptr);
}

TEST_F(InputTest, HeaderErrorsCannotSatisfyMainFileAnnotations) {
  iree::testing::TempFilePath header("loom_input_error", ".h");
  IREE_ASSERT_OK(Write(header.path(),
                       "long distance(int* a, int* b) { return a - b; }\n"));
  const std::string source =
      "// RUN: verify\n// ERROR: LOWERING/059\n"
      "#include \"" +
      header.path() + "\"\n";
  loom::testing::LoomCheckHarness harness;
  IREE_ASSERT_OK(harness.Initialize(&environment_));
  loom_check_result_t result;
  IREE_ASSERT_OK(
      harness.ExecuteFirst(View(source), IREE_SV("main.cxx-test"), &result));
  EXPECT_EQ(result.raw_outcome, LOOM_CHECK_FAIL);
  EXPECT_NE(harness.DiagnosticJsonString(result).find(header.path()),
            std::string::npos);
  const std::string edits =
      String(loom_json_value_list_body(&result.annotation_edits));
  EXPECT_EQ(edits.find("insert_diagnostic_annotations"), std::string::npos);
  loom_check_result_deinitialize(&result);
}

TEST_F(InputTest, UpdatePreservesInputAndIsIdempotent) {
  iree::testing::TempFilePath path("loom_input_update", ".cxx-test");
  const std::string source =
      "// INPUT: cxx root=entry\n"
      "constexpr int count = sizeof(R\"(a\n// kept in the string\nb)\");\n"
      "int entry() { return count; }\n";
  IREE_ASSERT_OK(Write(path.path(), source));
  loom_check_process_options_t options = {};
  options.update = true;
  iree_host_size_t passed = 0, failed = 0, skipped = 0;
  IREE_ASSERT_OK(loom_check_read_and_process(
      path.path_view(), &options, &environment_, &context_, &pool_,
      iree_allocator_system(), &passed, &failed, &skipped));
  const std::string updated = Read(path.path());
  EXPECT_EQ(updated.substr(0, source.size()), source);
  EXPECT_NE(updated.find("// ----\nfunc.def public @entry"), std::string::npos);
  passed = failed = skipped = 0;
  IREE_ASSERT_OK(loom_check_read_and_process(
      path.path_view(), &options, &environment_, &context_, &pool_,
      iree_allocator_system(), &passed, &failed, &skipped));
  EXPECT_EQ(passed, 1u);
  EXPECT_EQ(failed, 0u);
  EXPECT_EQ(Read(path.path()), updated);
}

}  // namespace
