// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <array>

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "iree/vm/buffer.h"
#include "iree/vm/bytecode/module.h"
#include "iree/vm/reflection.h"
#include "iree/vm/sync.h"
#include "loom/tooling/target/vm/imports_bytecode.h"

namespace {

constexpr iree_vm_module_signature_type_t kStep[] = {
    {IREE_VM_SCALAR_TYPE_I32, 0}};
constexpr iree_vm_module_signature_type_t kFail[] = {
    {IREE_VM_MODULE_SIGNATURE_TYPE_KIND_REF, 0},
    {IREE_VM_MODULE_SIGNATURE_TYPE_KIND_REF, 0}};
constexpr iree_vm_module_signature_type_t kMixed[] = {
    {IREE_VM_MODULE_SIGNATURE_TYPE_KIND_REF, 0},
    {IREE_VM_SCALAR_TYPE_I64, 0},
    {IREE_VM_MODULE_SIGNATURE_TYPE_KIND_REF, 0}};
constexpr auto kWide = [] {
  std::array<iree_vm_module_signature_type_t, 34> types = {};
  for (int i = 0; i < 17; ++i) {
    types[i] = {IREE_VM_SCALAR_TYPE_I64, 0};
    types[17 + i] = {IREE_VM_MODULE_SIGNATURE_TYPE_KIND_REF, 0};
  }
  return types;
}();

const iree_vm_module_callable_type_declaration_t kCallables[] = {
    {{{kStep, 1, 1, 0, 0}, {kStep, 1, 1, 0, 0}},
     IREE_VM_CALLABLE_TYPE_FLAG_NONE,
     0,
     0},
    {{{kFail, 2, 0, 2, 0}, {nullptr, 0, 0, 0, 0}},
     IREE_VM_CALLABLE_TYPE_FLAG_NONE,
     0,
     0},
    {{{kMixed, 3, 1, 2, 0}, {kMixed, 3, 1, 2, 0}},
     IREE_VM_CALLABLE_TYPE_FLAG_NONE,
     0,
     0},
    {{{kWide.data(), 34, 17, 17, 0}, {kWide.data(), 34, 17, 17, 0}},
     IREE_VM_CALLABLE_TYPE_FLAG_NONE,
     0,
     0},
};
const iree_vm_module_export_declaration_t kExports[] = {
    {IREE_SVL("fail"), 1, 0, 0},
    {IREE_SVL("mixed"), 2, 1, 0},
    {IREE_SVL("step"), 0, 2, 0},
    {IREE_SVL("wide"), 3, 3, 0},
};

iree_status_t Start(iree_vm_module_t*,
                    const iree_vm_module_function_start_params_t* params,
                    iree_vm_execution_outcome_t* outcome) {
  if (params->function_ordinal == 0) {
    return iree_make_status(IREE_STATUS_ABORTED, "native failure");
  }
  const uint16_t value_count = params->function_ordinal == 3 ? 17 : 1;
  const uint16_t ref_count = params->function_ordinal == 3   ? 17
                             : params->function_ordinal == 1 ? 2
                                                             : 0;
  // Result banks are disjoint from argument banks, including overflow slots.
  for (uint16_t i = 0; i < value_count; ++i) {
    iree_vm_call_value_result_store(
        &params->call, i,
        iree_vm_call_value_argument_load(&params->call, i) + i + 1);
  }
  for (uint16_t i = 0; i < ref_count; ++i) {
    iree_vm_ref_t ref = iree_vm_ref_null();
    iree_vm_call_ref_argument_load_move(&params->call, i, &ref);
    iree_vm_call_ref_result_store_move(&params->call, i, &ref);
  }
  *outcome = IREE_VM_EXECUTION_OUTCOME_COMPLETED;
  return iree_ok_status();
}

void QueryExport(const iree_vm_module_t*, iree_host_size_t ordinal,
                 iree_vm_module_export_declaration_t* value) {
  *value = kExports[ordinal];
}
void QueryCallable(const iree_vm_module_t*, iree_host_size_t ordinal,
                   iree_vm_module_callable_type_declaration_t* value) {
  *value = kCallables[ordinal];
}
void QueryImportGroup(const iree_vm_module_t*, iree_host_size_t,
                      iree_vm_module_import_group_t*) {
  IREE_CHECK_UNREACHABLE("native module has no imports");
}
void QueryImport(const iree_vm_module_t*, iree_host_size_t,
                 iree_vm_module_import_declaration_t*) {
  IREE_CHECK_UNREACHABLE("native module has no imports");
}
void DestroyModule(iree_vm_module_t*) {}
const iree_vm_module_vtable_t kVtable = {
    sizeof(kVtable),
    IREE_VM_MODULE_ABI_VERSION_0,
    DestroyModule,
    Start,
    iree_vm_module_function_resume_unreachable,
    nullptr,
    nullptr,
    nullptr,
    QueryImportGroup,
    QueryImport,
    QueryExport,
    QueryCallable,
    iree_vm_module_query_presentation_none,
    iree_vm_module_metadata_by_ordinal_none};

void CountRelease(void* user_data, iree_byte_span_t) {
  ++*static_cast<int*>(user_data);
}

class VMImportsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    iree_vm_environment_t* environment = nullptr;
    IREE_ASSERT_OK(
        iree_vm_environment_allocate(iree_allocator_system(), &environment));
    IREE_ASSERT_OK(iree_vm_ref_types_resolve(
        iree_vm_environment_lookup_ref_type_table(environment, IREE_SV("vm")),
        &types_));
    const iree_file_toc_t* source = loom_vm_imports_bytecode_create();
    IREE_ASSERT_OK(iree_vm_bytecode_module_create(
        environment, IREE_SV("compiled"),
        {iree_make_const_byte_span(source[0].data, source[0].size),
         iree_allocator_null()},
        iree_allocator_system(), &bytecode_));
    iree_vm_environment_free(environment);

    descriptor_ = {IREE_SVL("native"),
                   IREE_VM_MODULE_FLAG_LINKABLE,
                   {&types_.buffer, 1},
                   {4, 4, 0, 0, 4, 0, {38, 40, 0}},
                   0};
    IREE_ASSERT_OK(iree_vm_module_initialize(&kVtable, &descriptor_, &native_));
    iree_vm_module_t* libraries[] = {&native_};
    IREE_ASSERT_OK(iree_vm_program_create(
        {bytecode_, iree_vm_module_span_from_array(libraries)},
        iree_allocator_system(), &program_));
    IREE_ASSERT_OK(iree_vm_invocation_initialize(
        iree_make_byte_span(storage_.data(), storage_.size()), &invocation_));
    IREE_ASSERT_OK(iree_vm_process_create(program_, invocation_,
                                          iree_vm_variant_span_empty(),
                                          iree_allocator_system(), &process_));
  }

  void TearDown() override {
    iree_vm_process_release(process_);
    iree_vm_invocation_deinitialize(invocation_);
    iree_vm_program_release(program_);
    iree_vm_module_release(bytecode_);
    iree_vm_module_release(&native_);
  }

  iree_status_t Invoke(iree_string_view_t name,
                       iree_vm_variant_span_t arguments,
                       iree_vm_variant_span_t results) {
    iree_vm_function_t function = iree_vm_function_null();
    IREE_RETURN_IF_ERROR(iree_vm_process_lookup_function(
        process_, IREE_SV("compiled"), name, &function));
    return iree_vm_invoke(invocation_, function, arguments, results);
  }

  iree_vm_variant_t WrapBuffer(int* release_count) {
    iree_vm_buffer_t* buffer = nullptr;
    IREE_CHECK_OK(iree_vm_buffer_wrap(
        IREE_VM_BUFFER_ACCESS_FLAG_READ,
        iree_make_byte_span(bytes_.data(), bytes_.size()),
        {CountRelease, release_count}, iree_allocator_system(), &buffer));
    iree_vm_buffer_t* alias = nullptr;
    IREE_CHECK_OK(iree_vm_buffer_subspan(buffer, 2, 4,
                                         IREE_VM_BUFFER_ACCESS_FLAG_READ,
                                         iree_allocator_system(), &alias));
    iree_vm_buffer_release(buffer);
    return iree_vm_buffer_variant_from_ptr_move(&types_, &alias);
  }

  void ExpectAliases(iree_vm_variant_t left, iree_vm_variant_t right) {
    void* lhs = nullptr;
    void* rhs = nullptr;
    IREE_ASSERT_OK(
        iree_vm_ptr_from_variant_borrowed(left, types_.buffer, &lhs));
    IREE_ASSERT_OK(
        iree_vm_ptr_from_variant_borrowed(right, types_.buffer, &rhs));
    EXPECT_EQ(lhs, rhs);
    EXPECT_EQ(iree_vm_buffer_length(static_cast<iree_vm_buffer_t*>(lhs)), 4u);
  }

  // Resolved core reference types shared by both modules.
  iree_vm_ref_types_t types_ = {};
  // Native provider storage borrowed throughout the fixture.
  iree_vm_module_t native_ = {};
  // Immutable native provider description.
  iree_vm_module_descriptor_t descriptor_ = {};
  // Bytecode compiled from the authored fixture by loom-compile.
  iree_vm_module_t* bytecode_ = nullptr;
  // Linked native and compiled modules.
  iree_vm_program_t* program_ = nullptr;
  // Independent execution state for the linked program.
  iree_vm_process_t* process_ = nullptr;
  // Host-owned invocation storage.
  alignas(iree_max_align_t) std::array<uint8_t, 16384> storage_ = {};
  // Reusable invocation borrowing storage_.
  iree_vm_invocation_t* invocation_ = nullptr;
  // Host buffer backing kept alive until every returned alias is released.
  std::array<uint8_t, 8> bytes_ = {};
};

TEST_F(VMImportsTest, NativeCallsExecuteInsideRuntimeLoop) {
  iree_vm_variant_t arguments[] = {iree_vm_variant_from_i32(4)};
  iree_vm_variant_t result = {};
  IREE_ASSERT_OK(Invoke(IREE_SV("loop"),
                        iree_vm_variant_span_from_array(arguments),
                        {&result, 1}));
  int32_t value = 0;
  IREE_ASSERT_OK(iree_vm_i32_from_variant(result, &value));
  EXPECT_EQ(value, 10);
}

TEST_F(VMImportsTest, MixedCallPreservesLiveValuesAndAliasedReferences) {
  int release_count = 0;
  iree_vm_variant_t arguments[] = {WrapBuffer(&release_count),
                                   iree_vm_variant_from_i64(42)};
  iree_vm_variant_t results[3] = {};
  IREE_ASSERT_OK(Invoke(IREE_SV("aliases"),
                        iree_vm_variant_span_from_array(arguments),
                        iree_vm_variant_span_from_array(results)));
  int64_t value = 0;
  IREE_ASSERT_OK(iree_vm_i64_from_variant(results[1], &value));
  EXPECT_EQ(value, 85);
  ExpectAliases(results[0], results[2]);
  EXPECT_EQ(release_count, 0);
  iree_vm_variant_span_reset(iree_vm_variant_span_from_array(results));
  EXPECT_EQ(release_count, 1);
}

TEST_F(VMImportsTest, OverflowArgumentsAndResultsUseBothBanks) {
  int release_count = 0;
  iree_vm_variant_t arguments[] = {iree_vm_variant_from_i64(42),
                                   WrapBuffer(&release_count)};
  iree_vm_variant_t results[4] = {};
  IREE_ASSERT_OK(Invoke(IREE_SV("overflow"),
                        iree_vm_variant_span_from_array(arguments),
                        iree_vm_variant_span_from_array(results)));
  int64_t first = 0;
  int64_t last = 0;
  IREE_ASSERT_OK(iree_vm_i64_from_variant(results[0], &first));
  IREE_ASSERT_OK(iree_vm_i64_from_variant(results[1], &last));
  EXPECT_EQ(first, 43);
  EXPECT_EQ(last, 101);
  ExpectAliases(results[2], results[3]);
  EXPECT_EQ(release_count, 0);
  iree_vm_variant_span_reset(iree_vm_variant_span_from_array(results));
  EXPECT_EQ(release_count, 1);
}

TEST_F(VMImportsTest, NativeFailureUnwindsAliasedBufferArguments) {
  int release_count = 0;
  iree_vm_variant_t argument = WrapBuffer(&release_count);
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_ABORTED,
      Invoke(IREE_SV("failure"), {&argument, 1}, iree_vm_variant_span_empty()));
  EXPECT_EQ(release_count, 1);
}

}  // namespace
