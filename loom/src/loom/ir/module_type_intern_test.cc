// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/base/internal/arena.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/ir/context.h"
#include "loom/ir/module.h"
#include "loom/ops/test/types.h"

namespace loom {
namespace {

struct TypeGrowthBoundary {
  // Public capacity hint used when allocating the module.
  iree_host_size_t type_count_hint;
  // Number of unique types inserted before fault injection.
  iree_host_size_t type_count;
  // Expected row capacity at the boundary, before the next insertion.
  iree_host_size_t row_capacity;
  // Expected hash capacity at the boundary, before the next insertion.
  iree_host_size_t hash_capacity;
};

class TypeInternerFailureTest
    : public ::testing::TestWithParam<TypeGrowthBoundary> {
 protected:
  static iree_status_t Allocate(void* self, iree_allocator_command_t command,
                                const void* parameters, void** pointer) {
    auto* test = static_cast<TypeInternerFailureTest*>(self);
    if (command != IREE_ALLOCATOR_COMMAND_FREE) {
      if (test->allocation_count_++ == test->failure_index_) {
        return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                                "injected allocation failure");
      }
    }
    const auto allocator = iree_allocator_system();
    return allocator.ctl(allocator.self, command, parameters, pointer);
  }

  void SetUp() override {
    loom_context_initialize(iree_allocator_system(), &context_);
    IREE_ASSERT_OK(loom_context_finalize(&context_));
    iree_arena_block_pool_initialize(128, {this, Allocate}, &pool_);
  }

  void TearDown() override {
    loom_module_free(module_);
    iree_arena_block_pool_deinitialize(&pool_);
    loom_context_deinitialize(&context_);
  }

  void PrepareBoundary() {
    failure_index_ = SIZE_MAX;
    loom_module_free(module_);
    module_ = nullptr;
    // Each attempt starts with the same empty pool, not blocks cached by the
    // previous attempt's rollback or successful retry.
    iree_arena_block_pool_trim(&pool_);
    loom_module_size_hints_t hints = {};
    hints.type_count = GetParam().type_count_hint;
    IREE_ASSERT_OK(loom_module_allocate(&context_, IREE_SV("type_growth"),
                                        &pool_, &hints, iree_allocator_system(),
                                        &module_));
    loom_type_id_t element_id = LOOM_TYPE_ID_INVALID;
    IREE_ASSERT_OK(loom_module_intern_type_id(
        module_, loom_type_scalar(LOOM_SCALAR_TYPE_BF16), &element_id));
    ASSERT_EQ(element_id, 0u);
    for (iree_host_size_t alignment = 1; alignment < GetParam().type_count;
         ++alignment) {
      loom_type_t type = {};
      loom_type_id_t type_id = LOOM_TYPE_ID_INVALID;
      IREE_ASSERT_OK(MakeType(alignment, &type, &type_id));
      ASSERT_EQ(type_id, alignment);
    }
    ASSERT_EQ(module_->types.capacity, GetParam().row_capacity);
    ASSERT_EQ(module_->type_intern.capacity, GetParam().hash_capacity);
    allocation_count_ = 0;
  }

  iree_status_t MakeType(iree_host_size_t alignment, loom_type_t* out_type,
                         loom_type_id_t* out_type_id) {
    const loom_attribute_t parameters[] = {
        loom_attr_type(0), loom_attr_i64(alignment), loom_attr_absent()};
    return loom_module_make_parameterized_type(
        module_, &loom_test_array_type_parameterized_descriptor, parameters,
        IREE_ARRAYSIZE(parameters), out_type, out_type_id);
  }

  // Backing allocation ordinal to fail, or SIZE_MAX when failure is disabled.
  iree_host_size_t failure_index_ = SIZE_MAX;
  // Backing allocation requests since the latest preparation completed.
  iree_host_size_t allocation_count_ = 0;
  // Small blocks expose table allocations to the fault-injecting allocator.
  iree_arena_block_pool_t pool_ = {};
  // Minimal context for type construction through the production module API.
  loom_context_t context_ = {};
  // Module owning the canonical rows, payloads, and interner under test.
  loom_module_t* module_ = nullptr;
};

TEST_P(TypeInternerFailureTest,
       GrowthFailurePreservesCanonicalTypesAndRetries) {
  ASSERT_NO_FATAL_FAILURE(PrepareBoundary());
  loom_type_t type = {};
  loom_type_id_t type_id = LOOM_TYPE_ID_INVALID;
  IREE_ASSERT_OK(MakeType(GetParam().type_count, &type, &type_id));
  const iree_host_size_t growth_allocation_count = allocation_count_;
  ASSERT_GT(growth_allocation_count, 0u);

  for (iree_host_size_t failure_index = 0;
       failure_index < growth_allocation_count; ++failure_index) {
    SCOPED_TRACE(failure_index);
    ASSERT_NO_FATAL_FAILURE(PrepareBoundary());
    const loom_type_table_t types = module_->types;
    const loom_intern_table_t interner = module_->type_intern;
    const iree_host_size_t used_bytes = module_->arena.used_allocation_size;
    const iree_host_size_t owned_bytes = module_->arena.total_allocation_size;
    failure_index_ = failure_index;
    IREE_EXPECT_STATUS_IS(IREE_STATUS_RESOURCE_EXHAUSTED,
                          MakeType(GetParam().type_count, &type, &type_id));
    EXPECT_EQ(allocation_count_, failure_index + 1);
    failure_index_ = SIZE_MAX;

    EXPECT_EQ(module_->types.entries, types.entries);
    EXPECT_EQ(module_->types.hashes, types.hashes);
    EXPECT_EQ(module_->types.capacity, types.capacity);
    EXPECT_EQ(module_->types.count, types.count);
    EXPECT_EQ(module_->type_intern.hashes, interner.hashes);
    EXPECT_EQ(module_->type_intern.indices, interner.indices);
    EXPECT_EQ(module_->type_intern.capacity, interner.capacity);
    EXPECT_EQ(module_->type_intern.count, interner.count);
    EXPECT_EQ(module_->arena.used_allocation_size, used_bytes);
    EXPECT_EQ(module_->arena.total_allocation_size, owned_bytes);

    for (iree_host_size_t alignment = 1; alignment < types.count; ++alignment) {
      IREE_ASSERT_OK(MakeType(alignment, &type, &type_id));
      EXPECT_EQ(type_id, alignment);
      EXPECT_EQ(loom_test_array_type_alignment(type), alignment);
      EXPECT_EQ(loom_type_parameterized_parameters(type),
                loom_type_parameterized_parameters(types.entries[type_id]));
      EXPECT_EQ(module_->types.hashes[type_id], loom_type_hash(type));
    }
    IREE_ASSERT_OK(MakeType(GetParam().type_count, &type, &type_id));
    EXPECT_EQ(type_id, GetParam().type_count);
    EXPECT_EQ(module_->types.count, types.count + 1);
    EXPECT_EQ(module_->type_intern.count, interner.count + 1);
  }
}

TEST_P(TypeInternerFailureTest, InvalidParameterRollsBackAndRetries) {
  for (iree_host_size_t parameter_index = 0; parameter_index < 3;
       ++parameter_index) {
    SCOPED_TRACE(parameter_index);
    ASSERT_NO_FATAL_FAILURE(PrepareBoundary());
    const iree_host_size_t used_bytes = module_->arena.used_allocation_size;
    const iree_host_size_t owned_bytes = module_->arena.total_allocation_size;
    const loom_type_table_t types = module_->types;
    const loom_intern_table_t interner = module_->type_intern;
    loom_attribute_t parameters[] = {loom_attr_type(0),
                                     loom_attr_i64(GetParam().type_count),
                                     loom_attr_absent()};
    // Each slot has a non-string descriptor. Later failures follow one or two
    // successful canonicalizations before the shared payload builder returns.
    parameters[parameter_index] = loom_attr_string(LOOM_STRING_ID_INVALID);
    loom_type_t type = {};
    loom_type_id_t type_id = LOOM_TYPE_ID_INVALID;
    IREE_EXPECT_STATUS_IS(
        IREE_STATUS_INVALID_ARGUMENT,
        loom_module_make_parameterized_type(
            module_, &loom_test_array_type_parameterized_descriptor, parameters,
            IREE_ARRAYSIZE(parameters), &type, &type_id));
    EXPECT_EQ(module_->arena.used_allocation_size, used_bytes);
    EXPECT_EQ(module_->arena.total_allocation_size, owned_bytes);
    EXPECT_EQ(module_->types.entries, types.entries);
    EXPECT_EQ(module_->types.count, types.count);
    EXPECT_EQ(module_->type_intern.hashes, interner.hashes);
    EXPECT_EQ(module_->type_intern.count, interner.count);
    IREE_ASSERT_OK(MakeType(GetParam().type_count, &type, &type_id));
    EXPECT_EQ(type_id, GetParam().type_count);
  }
}

TEST_P(TypeInternerFailureTest, GenericInternOwnsCanonicalParameterPayload) {
  ASSERT_NO_FATAL_FAILURE(PrepareBoundary());
  loom_attribute_t parameters[] = {loom_attr_type(0),
                                   loom_attr_i64(GetParam().type_count),
                                   loom_attr_absent()};
  const loom_type_t temporary_type =
      loom_type_parameterized(&loom_test_array_type_parameterized_descriptor,
                              IREE_ARRAYSIZE(parameters), parameters);
  loom_type_id_t type_id = LOOM_TYPE_ID_INVALID;
  IREE_ASSERT_OK(loom_module_intern_type_id(module_, temporary_type, &type_id));
  ASSERT_EQ(type_id, GetParam().type_count);
  const loom_type_t canonical_type = module_->types.entries[type_id];
  EXPECT_NE(loom_type_parameterized_parameters(canonical_type), parameters);
  parameters[1] = loom_attr_i64(0);
  EXPECT_EQ(loom_test_array_type_alignment(canonical_type),
            GetParam().type_count);
  const iree_host_size_t used_bytes = module_->arena.used_allocation_size;
  loom_type_t duplicate_type = {};
  loom_type_id_t duplicate_id = LOOM_TYPE_ID_INVALID;
  IREE_ASSERT_OK(
      MakeType(GetParam().type_count, &duplicate_type, &duplicate_id));
  EXPECT_EQ(duplicate_id, type_id);
  EXPECT_EQ(loom_type_parameterized_parameters(duplicate_type),
            loom_type_parameterized_parameters(canonical_type));
  EXPECT_EQ(module_->arena.used_allocation_size, used_bytes);
}

// Exercise row-only, hash-only, and simultaneous row/hash growth through public
// size hints and distinct type construction, without altering table metadata.
INSTANTIATE_TEST_SUITE_P(TypeGrowth, TypeInternerFailureTest,
                         ::testing::Values(TypeGrowthBoundary{0, 8, 8, 16},
                                           TypeGrowthBoundary{0, 12, 16, 16},
                                           TypeGrowthBoundary{64, 96, 96,
                                                              128}));

}  // namespace
}  // namespace loom
