// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/ops/function_contract_verify.h"

#include <utility>

#include "iree/base/internal/arena.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/error/error_catalog.h"
#include "loom/ir/context.h"
#include "loom/ir/module.h"
#include "loom/ops/func/ops.h"
#include "loom/testing/diagnostic_matchers.h"

namespace loom {
namespace {

using ::loom::testing::DiagnosticEmissionCapture;

class FunctionContractVerifyTest : public ::testing::Test {
 protected:
  void SetUp() override {
    iree_arena_block_pool_initialize(4096, iree_allocator_system(),
                                     &block_pool_);
    loom_context_initialize(iree_allocator_system(), &context_);
    iree_host_size_t vtable_count = 0;
    const loom_op_vtable_t* const* vtables =
        loom_func_dialect_vtables(&vtable_count);
    IREE_ASSERT_OK(loom_context_register_dialect(
        &context_, LOOM_DIALECT_FUNC, vtables, (uint16_t)vtable_count));
    IREE_ASSERT_OK(loom_context_finalize(&context_));
    IREE_ASSERT_OK(loom_module_allocate(&context_, IREE_SV("test"),
                                        &block_pool_, nullptr,
                                        iree_allocator_system(), &module_));
    loom_builder_initialize(module_, &module_->arena,
                            loom_module_block(module_), &builder_);
  }

  void TearDown() override {
    loom_module_free(module_);
    loom_context_deinitialize(&context_);
    iree_arena_block_pool_deinitialize(&block_pool_);
  }

  void AddSymbol(iree_string_view_t name, loom_symbol_ref_t* out_symbol) {
    loom_string_id_t name_id = LOOM_STRING_ID_INVALID;
    IREE_ASSERT_OK(loom_module_intern_string(module_, name, &name_id));
    loom_symbol_id_t symbol_id = LOOM_SYMBOL_ID_INVALID;
    IREE_ASSERT_OK(loom_module_add_symbol(module_, name_id, &symbol_id));
    *out_symbol = loom_symbol_ref_t{/*.module_id=*/0, /*.symbol_id=*/symbol_id};
  }

  void AddIndexDeclaration(iree_string_view_t name, loom_op_t** out_op) {
    loom_symbol_ref_t symbol = loom_symbol_ref_null();
    AddSymbol(name, &symbol);
    const loom_type_t index = loom_type_scalar(LOOM_SCALAR_TYPE_INDEX);
    const loom_type_t arguments[] = {index, index, index, index, index};
    const loom_type_t results[] = {index, index, index, index};
    IREE_ASSERT_OK(loom_func_decl_build(
        &builder_, /*build_flags=*/0, /*visibility=*/0, /*retain=*/0,
        LOOM_STRING_ID_INVALID, LOOM_STRING_ID_INVALID, /*cc=*/0,
        /*purity=*/0, /*temperature=*/0, /*inline_policy=*/0,
        loom_symbol_ref_null(), /*abi=*/0, loom_named_attr_slice_empty(),
        LOOM_STRING_ID_INVALID, loom_named_attr_slice_empty(), symbol,
        arguments, IREE_ARRAYSIZE(arguments), results, IREE_ARRAYSIZE(results),
        /*tied_results=*/nullptr, /*tied_result_count=*/0,
        /*predicates=*/nullptr, /*predicates_count=*/0, LOOM_LOCATION_UNKNOWN,
        out_op));
  }

  iree_arena_block_pool_t block_pool_;
  loom_context_t context_;
  loom_module_t* module_ = nullptr;
  loom_builder_t builder_ = {};
};

TEST_F(FunctionContractVerifyTest,
       IndexedDeclarationSliceKeepsDefinitionDomains) {
  loom_op_t* declaration = nullptr;
  loom_op_t* other_declaration = nullptr;
  AddIndexDeclaration(IREE_SV("source"), &declaration);
  AddIndexDeclaration(IREE_SV("other"), &other_declaration);
  ASSERT_NE(declaration, nullptr);
  ASSERT_NE(other_declaration, nullptr);
  const auto arguments = loom_func_decl_args(declaration);
  const auto results = loom_func_decl_results(declaration);
  const auto other_arguments = loom_func_decl_args(other_declaration);
  loom_value_id_t targets[6] = {};
  for (loom_value_id_t& target : targets) {
    IREE_ASSERT_OK(loom_module_define_value(
        module_, loom_type_scalar(LOOM_SCALAR_TYPE_INDEX), &target));
    IREE_ASSERT_OK(
        loom_block_add_arg(module_, loom_module_block(module_), target));
  }
  const loom_type_value_remap_t result_remap = {
      /*.source_values=*/results.values,
      /*.target_values=*/&targets[3],
      /*.count=*/3,
      /*.flags=*/LOOM_TYPE_VALUE_REMAP_FLAG_SOURCE_DEFINITION_SLICE,
  };
  const loom_type_value_remap_t argument_remap = {
      /*.source_values=*/&arguments.values[1],
      /*.target_values=*/targets,
      /*.count=*/3,
      /*.flags=*/LOOM_TYPE_VALUE_REMAP_FLAG_SOURCE_DEFINITION_SLICE,
      /*.next=*/&result_remap,
  };

  // The slice begins at operand one. Results, other declarations and block
  // arguments do not become members merely by sharing an in-slice index.
  const std::pair<loom_value_id_t, loom_value_id_t> mappings[] = {
      {arguments.values[0], arguments.values[0]},
      {arguments.values[1], targets[0]},
      {arguments.values[2], targets[1]},
      {arguments.values[3], targets[2]},
      {arguments.values[4], arguments.values[4]},
      {results.values[0], targets[3]},
      {results.values[1], targets[4]},
      {results.values[2], targets[5]},
      {results.values[3], results.values[3]},
      {other_arguments.values[1], other_arguments.values[1]},
      {targets[1], targets[1]},
  };
  const iree_host_size_t retained_bytes = module_->arena.used_allocation_size;
  for (const auto& [source_value, expected_value] : mappings) {
    SCOPED_TRACE(source_value);
    const loom_type_t source =
        loom_type_shaped_1d(LOOM_TYPE_VECTOR, LOOM_SCALAR_TYPE_F32,
                            loom_dim_pack_dynamic(source_value), 0);
    const loom_type_t expected =
        loom_type_shaped_1d(LOOM_TYPE_VECTOR, LOOM_SCALAR_TYPE_F32,
                            loom_dim_pack_dynamic(expected_value), 0);
    EXPECT_TRUE(loom_type_equal_after_value_remap(module_, source, expected,
                                                  &argument_remap));
    EXPECT_EQ(
        loom_type_hash_after_value_remap(module_, source, &argument_remap),
        loom_type_hash_after_value_remap(module_, expected, nullptr));
  }
  EXPECT_EQ(module_->arena.used_allocation_size, retained_bytes);
}

TEST_F(FunctionContractVerifyTest,
       SharedCanonicalTypeGraphIsComparedOncePerNode) {
  loom_op_t* declaration = nullptr;
  AddIndexDeclaration(IREE_SV("shared_graph"), &declaration);
  ASSERT_NE(declaration, nullptr);
  const loom_value_slice_t declaration_arguments =
      loom_func_decl_args(declaration);
  const loom_value_slice_t declaration_results =
      loom_func_decl_results(declaration);

  loom_value_id_t call_operands[5] = {};
  for (loom_value_id_t& operand : call_operands) {
    IREE_ASSERT_OK(loom_module_define_value(
        module_, loom_type_scalar(LOOM_SCALAR_TYPE_INDEX), &operand));
    IREE_ASSERT_OK(
        loom_block_add_arg(module_, loom_module_block(module_), operand));
  }
  const loom_type_t call_result_types[] = {
      loom_type_scalar(LOOM_SCALAR_TYPE_INDEX),
      loom_type_scalar(LOOM_SCALAR_TYPE_INDEX),
      loom_type_scalar(LOOM_SCALAR_TYPE_INDEX),
      loom_type_scalar(LOOM_SCALAR_TYPE_INDEX),
  };
  loom_op_t* call = nullptr;
  IREE_ASSERT_OK(loom_func_call_build(
      &builder_, /*build_flags=*/0, /*purity=*/0, /*temperature=*/0,
      /*inline_policy=*/0, loom_func_decl_callee(declaration), call_operands,
      IREE_ARRAYSIZE(call_operands), call_result_types,
      IREE_ARRAYSIZE(call_result_types), /*tied_results=*/nullptr,
      /*tied_result_count=*/0, LOOM_LOCATION_UNKNOWN, &call));
  ASSERT_NE(call, nullptr);
  const loom_value_slice_t call_results = loom_func_call_results(call);

  loom_type_t source_type = loom_type_shaped_1d(
      LOOM_TYPE_VECTOR, LOOM_SCALAR_TYPE_F32,
      loom_dim_pack_dynamic(declaration_arguments.values[0]), 0);
  loom_type_t target_type =
      loom_type_shaped_1d(LOOM_TYPE_VECTOR, LOOM_SCALAR_TYPE_F32,
                          loom_dim_pack_dynamic(call_operands[0]), 0);
  for (int i = 0; i < 64; ++i) {
    const loom_type_t source_children[] = {source_type, source_type};
    const loom_type_t target_children[] = {target_type, target_type};
    IREE_ASSERT_OK(loom_module_intern_function_type(
        module_, source_children, IREE_ARRAYSIZE(source_children), nullptr, 0,
        &source_type));
    IREE_ASSERT_OK(loom_module_intern_function_type(
        module_, target_children, IREE_ARRAYSIZE(target_children), nullptr, 0,
        &target_type));
  }
  for (uint16_t i = 0; i < declaration_arguments.count; ++i) {
    IREE_ASSERT_OK(loom_module_set_value_type(
        module_, declaration_arguments.values[i], source_type));
    IREE_ASSERT_OK(
        loom_module_set_value_type(module_, call_operands[i], target_type));
  }
  for (uint16_t i = 0; i < declaration_results.count; ++i) {
    IREE_ASSERT_OK(loom_module_set_value_type(
        module_, declaration_results.values[i], source_type));
    IREE_ASSERT_OK(loom_module_set_value_type(module_, call_results.values[i],
                                              target_type));
  }

  const iree_host_size_t type_count = module_->types.count;
  const iree_host_size_t retained_bytes = module_->arena.used_allocation_size;
  DiagnosticEmissionCapture capture;
  IREE_EXPECT_OK(loom_function_call_contract_verify(
      module_, call, loom_func_call_callee(call), loom_func_call_operands(call),
      call_results, capture.emitter()));
  EXPECT_TRUE(capture.emissions.empty());
  EXPECT_EQ(module_->types.count, type_count);
  EXPECT_EQ(module_->arena.used_allocation_size, retained_bytes);
}

TEST_F(FunctionContractVerifyTest, RejectsPredicateValueOutsideSignature) {
  const loom_type_t i32 = loom_type_scalar(LOOM_SCALAR_TYPE_I32);
  loom_value_id_t foreign_value = LOOM_VALUE_ID_INVALID;
  IREE_ASSERT_OK(loom_module_define_value(module_, i32, &foreign_value));

  loom_predicate_t predicate = {};
  predicate.kind = LOOM_PREDICATE_EQ;
  predicate.arg_count = 2;
  predicate.arg_tags[0] = LOOM_PRED_ARG_VALUE;
  predicate.args[0] = foreign_value;
  predicate.arg_tags[1] = LOOM_PRED_ARG_CONST;
  predicate.args[1] = 4;

  loom_symbol_ref_t function_symbol = loom_symbol_ref_null();
  AddSymbol(IREE_SV("invalid"), &function_symbol);
  loom_op_t* function_op = nullptr;
  IREE_ASSERT_OK(loom_func_def_build(
      &builder_, LOOM_FUNC_DEF_BUILD_FLAG_HAS_PREDICATES,
      /*visibility=*/0, /*retain=*/0, /*cc=*/0, /*purity=*/0,
      /*temperature=*/0, /*inline_policy=*/0, loom_symbol_ref_null(),
      /*abi=*/0, loom_named_attr_slice_empty(), LOOM_STRING_ID_INVALID,
      loom_named_attr_slice_empty(), function_symbol, &i32, 1,
      /*result_types=*/nullptr, /*result_count=*/0, /*tied_results=*/nullptr,
      /*tied_result_count=*/0, &predicate, 1, LOOM_LOCATION_UNKNOWN,
      &function_op));

  DiagnosticEmissionCapture capture;
  IREE_EXPECT_OK(
      loom_function_contract_verify(module_, function_op, capture.emitter()));
  ASSERT_EQ(capture.emissions.size(), 1u);
  const auto& emission = capture.emissions.front();
  EXPECT_EQ(emission.error, LOOM_ERR_STRUCTURE_032);
  ASSERT_EQ(emission.string_params.size(), 3u);
  EXPECT_EQ(emission.string_params[0], "func.def");
  EXPECT_EQ(emission.string_params[1], "predicates[0].arg[0]");
  EXPECT_EQ(emission.string_params[2], "a function argument or result");
}

}  // namespace
}  // namespace loom
