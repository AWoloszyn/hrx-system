// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loomc/import/cxx.h"

#include <string>

#include "iree/testing/gtest.h"
#include "loomc/target/amdgpu.h"
#include "test/util.h"

namespace {
using loomc::testing::HandlePtr;
using ContextPtr = HandlePtr<loomc_context_t, loomc_context_release>;
using WorkspacePtr = HandlePtr<loomc_workspace_t, loomc_workspace_release>;
using SourcePtr = HandlePtr<loomc_source_t, loomc_source_release>;
using ModulePtr = HandlePtr<loomc_module_t, loomc_module_release>;
using ResultPtr = HandlePtr<loomc_result_t, loomc_result_release>;
using EnvironmentPtr =
    HandlePtr<loomc_target_environment_t, loomc_target_environment_release>;

ContextPtr CreateContext() {
  loomc_target_environment_t* environment = nullptr;
  LOOMC_EXPECT_OK(loomc_target_environment_create_amdgpu(
      loomc_allocator_system(), &environment));
  EnvironmentPtr owner(environment);
  loomc_context_target_options_t target_options = {};
  target_options.type = LOOMC_STRUCTURE_TYPE_CONTEXT_TARGET_OPTIONS;
  target_options.structure_size = sizeof(target_options);
  target_options.target_environment = environment;
  loomc_context_options_t options = {};
  options.next = &target_options;
  loomc_context_t* context = nullptr;
  LOOMC_EXPECT_OK(
      loomc_context_create(&options, loomc_allocator_system(), &context));
  return ContextPtr(context);
}

std::string Text(const loomc_module_t* module) {
  loomc_source_t* source = nullptr;
  LOOMC_EXPECT_OK(loomc_module_serialize_text_to_source(
      module, nullptr, loomc_allocator_system(), &source));
  SourcePtr owner(source);
  auto contents = loomc_source_contents(source);
  return std::string(reinterpret_cast<const char*>(contents.data),
                     contents.data_length);
}

TEST(CxxAssemblyTest, SourceAndEnvironmentOwnershipSurvivesBothFormats) {
  for (auto format : {LOOMC_SOURCE_FORMAT_TEXT, LOOMC_SOURCE_FORMAT_BYTECODE}) {
    SCOPED_TRACE(format);
    auto context = CreateContext();
    loomc_workspace_t* workspace = nullptr;
    LOOMC_ASSERT_OK(
        loomc_workspace_create(nullptr, loomc_allocator_system(), &workspace));
    WorkspacePtr workspace_owner(workspace);
    const char text[] = R"cpp(
#include <loomcxx/low.h>
      struct [[loom::representation("amdgpu.gfx11.generic.core")]] Contract {};
      using Word = float __attribute__((ext_vector_type(1)));
      Word entry(Word input) {
        Word result = loom::low::assembly<Contract, Word>(R"(
          (%input: reg<amdgpu.vgpr>) -> (reg<amdgpu.vgpr>) { return %input }
        )",
                                                          input);
        return result;
      }
    )cpp";
    loomc_source_options_t source_options = {};
    source_options.identifier = loomc_make_cstring_view("assembly.cxx");
    source_options.contents = loomc_make_byte_span(text, sizeof(text) - 1);
    source_options.storage = LOOMC_SOURCE_STORAGE_COPY;
    loomc_source_t* source = nullptr;
    LOOMC_ASSERT_OK(loomc_source_create(&source_options,
                                        loomc_allocator_system(), &source));
    SourcePtr source_owner(source);
    loomc_module_t* module = nullptr;
    loomc_result_t* result = nullptr;
    LOOMC_ASSERT_OK(loomc_module_import_cxx(context.get(), workspace, source,
                                            nullptr, loomc_allocator_system(),
                                            &module, &result));
    ModulePtr module_owner(module);
    ResultPtr result_owner(result);
    ASSERT_TRUE(loomc_result_succeeded(result));
    source_owner.reset();
    result_owner.reset();
    workspace_owner.reset();
    context.reset();

    std::string original = Text(module);
    EXPECT_NE(original.find("low.invoke inline"), std::string::npos);
    EXPECT_NE(original.find("reg<amdgpu.vgpr>"), std::string::npos);
    EXPECT_NE(original.find("vector<1xf32>"), std::string::npos);
    loomc_module_serialize_options_t serialize_options = {};
    serialize_options.format = format;
    loomc_source_t* serialized = nullptr;
    LOOMC_ASSERT_OK(loomc_module_serialize_to_source(
        module, &serialize_options, loomc_allocator_system(), &serialized));
    SourcePtr serialized_owner(serialized);
    module_owner.reset();

    // A new context resolves durable representation keys after all importer
    // and original module state has been released.
    context = CreateContext();
    LOOMC_ASSERT_OK(
        loomc_workspace_create(nullptr, loomc_allocator_system(), &workspace));
    workspace_owner.reset(workspace);
    module = nullptr;
    result = nullptr;
    LOOMC_ASSERT_OK(loomc_module_deserialize_from_source(
        context.get(), workspace, serialized, nullptr, loomc_allocator_system(),
        &module, &result));
    module_owner.reset(module);
    result_owner.reset(result);
    ASSERT_TRUE(loomc_result_succeeded(result));
    serialized_owner.reset();
    result_owner.reset();
    context.reset();
    workspace_owner.reset();
    EXPECT_EQ(Text(module), original);
  }
}

}  // namespace
