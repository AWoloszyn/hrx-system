// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/ops/op_defs.h"

#include "iree/base/internal/arena.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/ir/context.h"
#include "loom/ir/module.h"
#include "loom/ops/kernel/ops.h"

namespace loom {
namespace {

class OpEraseTest : public ::testing::Test {
 protected:
  void SetUp() override {
    iree_arena_block_pool_initialize(4096, iree_allocator_system(),
                                     &block_pool_);
    loom_context_initialize(iree_allocator_system(), &context_);
    iree_host_size_t count = 0;
    const loom_op_vtable_t* const* vtables =
        loom_kernel_dialect_vtables(&count);
    IREE_ASSERT_OK(loom_context_register_dialect(&context_, LOOM_DIALECT_KERNEL,
                                                 vtables, (uint16_t)count));
    IREE_ASSERT_OK(loom_context_finalize(&context_));
    IREE_ASSERT_OK(loom_module_allocate(&context_, IREE_SV("erase"),
                                        &block_pool_, nullptr,
                                        iree_allocator_system(), &module_));
  }

  void TearDown() override {
    loom_module_free(module_);
    loom_context_deinitialize(&context_);
    iree_arena_block_pool_deinitialize(&block_pool_);
  }

  // Storage backing the module's invocation-lifetime arena.
  iree_arena_block_pool_t block_pool_ = {};
  // Immutable operation metadata shared by the fixture's builders.
  loom_context_t context_ = {};
  // Owned module whose retained reference state is under test.
  loom_module_t* module_ = nullptr;
};

TEST_F(OpEraseTest, KernelDeclarationDropsBothOwnedSignatures) {
  loom_builder_t builder = {};
  loom_builder_initialize(module_, &module_->arena, loom_module_block(module_),
                          &builder);
  loom_string_id_t name = LOOM_STRING_ID_INVALID;
  IREE_ASSERT_OK(
      loom_module_intern_string(module_, IREE_SV("dispatch"), &name));
  uint16_t symbol = LOOM_SYMBOL_ID_INVALID;
  IREE_ASSERT_OK(loom_module_add_symbol(module_, name, &symbol));
  const loom_symbol_ref_t callee = {0, symbol};
  const loom_type_t argument_types[] = {
      loom_type_scalar(LOOM_SCALAR_TYPE_INDEX),
      loom_type_pool(loom_dim_pack_static(4)),
  };
  loom_op_t* declaration = nullptr;
  IREE_ASSERT_OK(loom_kernel_decl_build(
      &builder, /*build_flags=*/0, /*retain=*/0, loom_symbol_ref_null(),
      LOOM_STRING_ID_INVALID, /*export_linkage=*/0, callee, argument_types,
      IREE_ARRAYSIZE(argument_types), argument_types,
      IREE_ARRAYSIZE(argument_types), /*predicates=*/nullptr,
      /*predicates_count=*/0, LOOM_LOCATION_UNKNOWN, &declaration));
  const loom_value_slice_t signatures[] = {
      loom_kernel_decl_workloads(declaration),
      loom_kernel_decl_args(declaration),
  };
  for (const loom_value_slice_t signature : signatures) {
    IREE_ASSERT_OK(loom_module_set_value_type(
        module_, signature.values[1],
        loom_type_pool(loom_dim_pack_dynamic(signature.values[0]))));
  }
  ASSERT_EQ(module_->type_uses.active_count, 2u);
  const iree_host_size_t arena_bytes = module_->arena.used_allocation_size;
  IREE_ASSERT_OK(loom_op_erase(module_, declaration));
  EXPECT_FALSE(loom_module_has_active_type_uses(module_));
  EXPECT_EQ(module_->arena.used_allocation_size, arena_bytes);
  for (const loom_value_slice_t signature : signatures) {
    EXPECT_EQ(loom_module_value(module_, signature.values[1])->use_count, 0u);
    EXPECT_EQ(
        loom_module_value_first_outgoing_type_use(module_, signature.values[1]),
        LOOM_TYPE_USE_ID_INVALID);
    EXPECT_FALSE(loom_module_value_has_type_uses(module_, signature.values[0]));
  }
  IREE_ASSERT_OK(loom_module_compute_uses(module_));
  EXPECT_FALSE(loom_module_has_active_type_uses(module_));
}

TEST(DialectTableHelpers, ReturnVtableArraysAndCounts) {
  const loom_op_vtable_t vtable = {};
  const loom_op_vtable_t* const vtables[] = {
      &vtable,
  };

  iree_host_size_t count = 0;
  const loom_op_vtable_t* const* result =
      loom_dialect_vtable_array(vtables, IREE_ARRAYSIZE(vtables), &count);
  EXPECT_EQ(result, vtables);
  EXPECT_EQ(count, 1u);

  EXPECT_EQ(loom_dialect_vtable_array(vtables, IREE_ARRAYSIZE(vtables),
                                      /*out_count=*/nullptr),
            vtables);
}

TEST(DialectTableHelpers, ReturnSemanticArraysAndCounts) {
  loom_op_semantics_t semantics[] = {
      loom_op_semantics_empty(),
      loom_op_semantics_empty(),
  };
  semantics[1].phase = LOOM_OP_PHASE_EXECUTABLE;
  semantics[1].contract_families = LOOM_CONTRACT_VECTOR_COORDINATE;

  iree_host_size_t count = 0;
  const loom_op_semantics_t* result = loom_dialect_semantics_array(
      semantics, IREE_ARRAYSIZE(semantics), &count);
  EXPECT_EQ(result, semantics);
  EXPECT_EQ(count, 2u);

  EXPECT_EQ(loom_dialect_semantics_array(semantics, IREE_ARRAYSIZE(semantics),
                                         /*out_count=*/nullptr),
            semantics);
}

TEST(DialectTableHelpers, LookupSemanticsByDialectAndIndex) {
  loom_op_semantics_t semantics[] = {
      loom_op_semantics_empty(),
      loom_op_semantics_empty(),
  };
  semantics[1].phase = LOOM_OP_PHASE_EXECUTABLE;
  semantics[1].contract_families = LOOM_CONTRACT_VECTOR_COORDINATE;

  loom_op_semantics_t found = loom_dialect_semantics_lookup(
      LOOM_OP_KIND(LOOM_DIALECT_TEST, 1), LOOM_DIALECT_TEST, semantics,
      IREE_ARRAYSIZE(semantics));
  EXPECT_EQ(found.phase, LOOM_OP_PHASE_EXECUTABLE);
  EXPECT_EQ(found.contract_families, LOOM_CONTRACT_VECTOR_COORDINATE);

  loom_op_semantics_t wrong_dialect = loom_dialect_semantics_lookup(
      LOOM_OP_KIND(LOOM_DIALECT_VECTOR, 1), LOOM_DIALECT_TEST, semantics,
      IREE_ARRAYSIZE(semantics));
  EXPECT_EQ(wrong_dialect.phase, LOOM_OP_PHASE_UNSPECIFIED);
  EXPECT_EQ(wrong_dialect.contract_families, 0u);

  loom_op_semantics_t out_of_range = loom_dialect_semantics_lookup(
      LOOM_OP_KIND(LOOM_DIALECT_TEST, 2), LOOM_DIALECT_TEST, semantics,
      IREE_ARRAYSIZE(semantics));
  EXPECT_EQ(out_of_range.phase, LOOM_OP_PHASE_UNSPECIFIED);
  EXPECT_EQ(out_of_range.contract_families, 0u);
}

TEST(MemoryAccessHelpers, OperandIndexIsPayload) {
  loom_op_t op = {};
  op.operand_count = 5;

  loom_memory_access_vtable_t memory_access = {};
  memory_access.operation_kind = LOOM_MEMORY_ACCESS_OPERATION_STORE;
  memory_access.value_operand_index = 3;
  memory_access.expected_operand_index = LOOM_OPERAND_INDEX_NONE;
  memory_access.replacement_operand_index = LOOM_OPERAND_INDEX_NONE;

  loom_op_vtable_t op_vtable = {};
  op_vtable.fixed_operand_count = op.operand_count;
  op_vtable.memory_access = &memory_access;

  loom_memory_access_t access = {};
  access.op = &op;
  access.op_vtable = &op_vtable;

  EXPECT_FALSE(loom_memory_access_operand_index_is_payload(access, 0));
  EXPECT_TRUE(loom_memory_access_operand_index_is_payload(access, 3));
  EXPECT_FALSE(loom_memory_access_operand_index_is_payload(access, 5));

  memory_access.operation_kind = LOOM_MEMORY_ACCESS_OPERATION_LOAD;
  EXPECT_FALSE(loom_memory_access_operand_index_is_payload(access, 3));

  memory_access.operation_kind = LOOM_MEMORY_ACCESS_OPERATION_ATOMIC_CMPXCHG;
  memory_access.value_operand_index = LOOM_OPERAND_INDEX_NONE;
  memory_access.expected_operand_index = 1;
  memory_access.replacement_operand_index = 2;
  EXPECT_FALSE(loom_memory_access_operand_index_is_payload(access, 0));
  EXPECT_TRUE(loom_memory_access_operand_index_is_payload(access, 1));
  EXPECT_TRUE(loom_memory_access_operand_index_is_payload(access, 2));
}

}  // namespace
}  // namespace loom
