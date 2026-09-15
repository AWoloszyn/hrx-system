// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/format/text/printer/name_plan.h"

#include <string>
#include <vector>

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/format/text/printer/printer.h"
#include "loom/ir/context.h"
#include "loom/ir/module.h"
#include "loom/ops/test/ops.h"
#include "loom/ops/test/registry.h"

namespace {

class NamePlanTest : public ::testing::Test {
 protected:
  void SetUp() override {
    iree_allocator_t pool_allocator = {this, PoolAllocate};
    iree_arena_block_pool_initialize(4096, pool_allocator, &pool_);
    loom_context_initialize(iree_allocator_system(), &context_);
    IREE_ASSERT_OK(loom_test_dialect_register(&context_));
    IREE_ASSERT_OK(loom_context_finalize(&context_));
    IREE_ASSERT_OK(loom_module_allocate(&context_, IREE_SV("names"), &pool_,
                                        nullptr, iree_allocator_system(),
                                        &module_));
    loom_builder_initialize(module_, &module_->arena,
                            loom_module_block(module_), &builder_);
  }

  void TearDown() override {
    loom_module_free(module_);
    loom_context_deinitialize(&context_);
    iree_arena_block_pool_deinitialize(&pool_);
  }

  loom_value_id_t Constant(const std::string& name = {}) {
    loom_op_t* op = nullptr;
    IREE_CHECK_OK(loom_test_constant_build(
        &builder_, loom_attr_i64(1), loom_type_scalar(LOOM_SCALAR_TYPE_INDEX),
        LOOM_LOCATION_UNKNOWN, &op));
    loom_value_id_t value_id = loom_test_constant_result(op);
    if (!name.empty()) SetName(value_id, name);
    return value_id;
  }

  void SetName(loom_value_id_t value_id, const std::string& name) {
    loom_string_id_t name_id = LOOM_STRING_ID_INVALID;
    IREE_CHECK_OK(loom_module_intern_string(
        module_, iree_make_string_view(name.data(), name.size()), &name_id));
    IREE_CHECK_OK(loom_module_set_value_name(module_, value_id, name_id));
  }

  loom_type_t DimensionType(loom_value_id_t value_id) {
    return loom_type_shaped_1d(LOOM_TYPE_TENSOR, LOOM_SCALAR_TYPE_F32,
                               loom_dim_pack_dynamic(value_id), 0);
  }

  std::string Reference(loom_print_name_plan_t* plan,
                        loom_value_id_t value_id) {
    iree_string_builder_t output;
    iree_string_builder_initialize(iree_allocator_system(), &output);
    loom_output_stream_t stream;
    loom_output_stream_for_builder(&output, &stream);
    IREE_EXPECT_OK(
        loom_print_name_plan_write_value_ref(plan, &stream, module_, value_id));
    std::string result(iree_string_builder_buffer(&output),
                       iree_string_builder_size(&output));
    iree_string_builder_deinitialize(&output);
    return result;
  }

  std::vector<std::string> Names() {
    loom_print_name_plan_t plan;
    IREE_CHECK_OK(loom_print_name_plan_initialize(module_, &plan));
    std::vector<std::string> names;
    for (iree_host_size_t i = 0; i < module_->values.count; ++i) {
      names.push_back(Reference(&plan, (loom_value_id_t)i));
    }
    loom_print_name_plan_deinitialize(&plan);
    return names;
  }

  loom_op_t* Function(const char* name, uint16_t argument_count) {
    loom_string_id_t name_id;
    IREE_CHECK_OK(loom_module_intern_string(
        module_, iree_make_cstring_view(name), &name_id));
    uint16_t symbol_id;
    IREE_CHECK_OK(loom_module_add_symbol(module_, name_id, &symbol_id));
    loom_symbol_ref_t symbol = {0, symbol_id};
    loom_type_t type = loom_type_scalar(LOOM_SCALAR_TYPE_INDEX);
    std::vector<loom_type_t> types(argument_count, type);
    loom_op_t* op = nullptr;
    IREE_CHECK_OK(loom_test_func_build(&builder_, 0, 0, 0, symbol, types.data(),
                                       argument_count, nullptr, 0, nullptr, 0,
                                       nullptr, 0, LOOM_LOCATION_UNKNOWN, &op));
    return op;
  }

  static iree_status_t PoolAllocate(void* self,
                                    iree_allocator_command_t command,
                                    const void* parameters, void** pointer) {
    auto* fixture = static_cast<NamePlanTest*>(self);
    if (command != IREE_ALLOCATOR_COMMAND_FREE) {
      if (fixture->allocations_until_failure_ == 0) {
        return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                                "injected block allocation failure");
      }
      if (fixture->allocations_until_failure_ > 0) {
        --fixture->allocations_until_failure_;
      }
      ++fixture->allocation_count_;
    }
    iree_allocator_t allocator = iree_allocator_system();
    return allocator.ctl(allocator.self, command, parameters, pointer);
  }

  // Remaining successful pool allocations before an injected failure; -1
  // leaves the production system allocator unrestricted.
  int allocations_until_failure_ = -1;
  // Number of pool allocations, excluding free operations.
  size_t allocation_count_ = 0;
  // Module block pool used by both IR and print-scoped allocations.
  iree_arena_block_pool_t pool_;
  // Test dialect registry used by constant builders and printing.
  loom_context_t context_;
  // Module owning the values and explicit display names.
  loom_module_t* module_ = nullptr;
  // Insertion point for small API fixtures.
  loom_builder_t builder_;
};

TEST_F(NamePlanTest, UnnamedValuesRequireNoNamePlanAllocation) {
  Constant();
  Constant();
  loom_print_name_plan_t plan;
  IREE_ASSERT_OK(loom_print_name_plan_initialize(module_, &plan));
  EXPECT_EQ(plan.arena.block_pool, module_->arena.block_pool);
  EXPECT_EQ(plan.arena.total_allocation_size, 0u);
  EXPECT_EQ(Reference(&plan, 0), "%0");
  EXPECT_EQ(Reference(&plan, 1), "%1");
  loom_print_name_plan_deinitialize(&plan);
}

TEST_F(NamePlanTest, NamedValuesUseModuleBlockPool) {
  Constant("extent");
  loom_print_name_plan_t plan;
  IREE_ASSERT_OK(loom_print_name_plan_initialize(module_, &plan));
  EXPECT_EQ(plan.arena.block_pool, module_->arena.block_pool);
  EXPECT_NE(plan.resolutions, nullptr);
  EXPECT_GT(plan.arena.used_allocation_size, 0u);
  EXPECT_EQ(plan.arena.total_allocation_size, pool_.total_block_size);
  EXPECT_EQ(Reference(&plan, 0), "%extent");
  loom_print_name_plan_deinitialize(&plan);
}

TEST_F(NamePlanTest, DuplicateExplicitNames) {
  Constant("extent");
  Constant("extent");
  EXPECT_EQ(Names(), (std::vector<std::string>{"%extent$0", "%extent$1"}));
}

TEST_F(NamePlanTest, ExplicitSuffixAvoidance) {
  Constant("extent");
  Constant("extent");
  Constant("extent$0");
  EXPECT_EQ(Names(), (std::vector<std::string>{"%extent$1$0", "%extent$1",
                                               "%extent$0"}));
}

TEST_F(NamePlanTest, GeneratedCandidateFamiliesAreDisjoint) {
  Constant("extent");
  Constant("extent$0");
  Constant("extent$0");
  Constant("extent");
  EXPECT_EQ(Names(), (std::vector<std::string>{"%extent$1$0", "%extent$0$1",
                                               "%extent$0$2", "%extent$3"}));
}

TEST_F(NamePlanTest, NumericNameAvoidance) {
  Constant("1");
  Constant();
  EXPECT_EQ(Names(), (std::vector<std::string>{"%1", "%$1"}));
}

TEST_F(NamePlanTest, DuplicateBlockArgumentNames) {
  loom_op_t* function = Function("duplicate_arguments", 2);
  loom_block_t* block = loom_region_entry_block(loom_test_func_body(function));
  SetName(loom_block_arg_id(block, 0), "extent");
  SetName(loom_block_arg_id(block, 1), "extent");
  EXPECT_EQ(Names(), (std::vector<std::string>{"%extent$0", "%extent$1"}));
}

TEST_F(NamePlanTest, SiblingScopesPreserveExplicitNames) {
  loom_op_t* first = Function("first", 1);
  loom_op_t* second = Function("second", 1);
  SetName(
      loom_block_arg_id(loom_region_entry_block(loom_test_func_body(first)), 0),
      "extent");
  SetName(loom_block_arg_id(
              loom_region_entry_block(loom_test_func_body(second)), 0),
          "extent");
  EXPECT_EQ(Names(), (std::vector<std::string>{"%extent", "%extent"}));
}

TEST_F(NamePlanTest, GeneratedNamesAvoidExplicitNamesAcrossScopes) {
  Constant("extent");
  Constant("extent");
  loom_op_t* function = Function("nested", 1);
  SetName(loom_block_arg_id(
              loom_region_entry_block(loom_test_func_body(function)), 0),
          "extent$0");
  EXPECT_EQ(Names(), (std::vector<std::string>{"%extent$1$0", "%extent$1",
                                               "%extent$0"}));
}

TEST_F(NamePlanTest, LongExplicitCollisionChain) {
  Constant("extent");
  Constant("extent");
  Constant("extent$0");
  for (int counter = 1; counter <= 300; ++counter) {
    Constant("extent$" + std::to_string(counter) + "$0");
  }
  auto names = Names();
  EXPECT_EQ(names[0], "%extent$301$0");
  EXPECT_EQ(names[1], "%extent$1");
  EXPECT_EQ(names.back(), "%extent$300$0");
}

TEST_F(NamePlanTest, LongAnonymousCollisionChain) {
  Constant();
  Constant("0");
  Constant("$0");
  for (int counter = 1; counter <= 300; ++counter) {
    Constant("$" + std::to_string(counter) + "$0");
  }
  auto names = Names();
  EXPECT_EQ(names[0], "%$301$0");
  EXPECT_EQ(names[1], "%0");
  EXPECT_EQ(names.back(), "%$300$0");
}

TEST_F(NamePlanTest, NonNameStringsDoNotReserveCandidates) {
  Constant("extent");
  Constant("extent");
  loom_string_id_t string_id;
  IREE_ASSERT_OK(
      loom_module_intern_string(module_, IREE_SV("extent$0"), &string_id));
  EXPECT_EQ(Names(), (std::vector<std::string>{"%extent$0", "%extent$1"}));
}

TEST_F(NamePlanTest, NamePlanningDoesNotChangeIR) {
  auto first = Constant("extent");
  auto second = Constant("extent");
  loom_string_id_t name_id = loom_module_value(module_, first)->name_id;
  iree_host_size_t string_count = module_->strings.count;
  auto names = Names();
  EXPECT_EQ(Names(), names);
  EXPECT_EQ(module_->strings.count, string_count);
  EXPECT_EQ(loom_module_value(module_, first)->name_id, name_id);
  EXPECT_EQ(loom_module_value(module_, second)->name_id, name_id);
}

TEST_F(NamePlanTest, LazyPlanIsSharedByReferences) {
  Constant("extent");
  Constant("extent");
  loom_print_name_plan_t plan = {};
  EXPECT_EQ(Reference(&plan, 0), "%extent$0");
  auto* resolutions = plan.resolutions;
  auto allocated_size = plan.arena.total_allocation_size;
  EXPECT_NE(resolutions, nullptr);
  EXPECT_EQ(Reference(&plan, 1), "%extent$1");
  EXPECT_EQ(plan.resolutions, resolutions);
  EXPECT_EQ(plan.arena.total_allocation_size, allocated_size);
  loom_print_name_plan_deinitialize(&plan);
}

TEST_F(NamePlanTest, InvalidReferenceDoesNotPreparePlan) {
  loom_print_name_plan_t plan = {};
  EXPECT_EQ(Reference(&plan, LOOM_VALUE_ID_INVALID), "%?");
  EXPECT_EQ(plan.arena.block_pool, nullptr);
  loom_print_name_plan_deinitialize(&plan);
}

TEST_F(NamePlanTest, AllocationFailuresLeaveAnEmptyPlan) {
  Constant("extent");
  Constant("extent");
  // Force the temporary index into a separate oversized arena allocation.
  Constant(std::string(4096, 'a'));
  for (int successful_allocations = 0; successful_allocations < 2;
       ++successful_allocations) {
    iree_arena_block_pool_trim(&pool_);
    allocations_until_failure_ = successful_allocations;
    loom_print_name_plan_t plan = {};
    IREE_EXPECT_STATUS_IS(IREE_STATUS_RESOURCE_EXHAUSTED,
                          loom_print_name_plan_initialize(module_, &plan));
    EXPECT_EQ(plan.resolutions, nullptr);
    EXPECT_EQ(plan.arena.block_pool, nullptr);
    loom_print_name_plan_deinitialize(&plan);
  }
}

TEST_F(NamePlanTest, StaticAtomsDoNotAllocateNamePlans) {
  Constant("extent");
  Constant("extent");
  iree_arena_block_pool_trim(&pool_);
  allocations_until_failure_ = 0;
  size_t allocation_count = allocation_count_;
  loom_output_stream_t stream;
  loom_output_stream_null(&stream);
  loom_type_t scalar = loom_type_scalar(LOOM_SCALAR_TYPE_F32);
  IREE_ASSERT_OK(loom_text_print_type(scalar, module_, &stream));
  IREE_ASSERT_OK(
      loom_text_print_type_with_options(scalar, module_, &stream, nullptr));
  loom_type_t tensor = loom_type_shaped_1d(
      LOOM_TYPE_TENSOR, LOOM_SCALAR_TYPE_F32, loom_dim_pack_static(4), 0);
  IREE_ASSERT_OK(loom_text_print_type(tensor, module_, &stream));
  loom_attribute_t attribute = loom_attr_i64(42);
  IREE_ASSERT_OK(loom_text_print_attribute(&attribute, module_, &stream));
  EXPECT_EQ(allocation_count_, allocation_count);
}

TEST_F(NamePlanTest, StandaloneTypePropagatesNamePlanAllocationFailure) {
  auto dimension = Constant("extent");
  Constant("extent");
  iree_arena_block_pool_trim(&pool_);
  allocations_until_failure_ = 0;
  loom_output_stream_t stream;
  loom_output_stream_null(&stream);
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_RESOURCE_EXHAUSTED,
      loom_text_print_type(DimensionType(dimension), module_, &stream));
}

TEST_F(NamePlanTest, StandaloneRegisterAttributeKeepsDiagnosticSpelling) {
  auto dimension = Constant("extent");
  Constant("extent");
  loom_type_t register_type;
  IREE_ASSERT_OK(loom_module_intern_register_type(
      module_, /*carrier_payload0=*/0x2a,
      /*carrier_payload1=*/7 | ((uint64_t)1 << 16), DimensionType(dimension),
      &register_type));
  loom_type_id_t type_id;
  IREE_ASSERT_OK(loom_module_intern_type_id(module_, register_type, &type_id));
  loom_attribute_t attribute = loom_attr_type(type_id);
  iree_string_builder_t output;
  iree_string_builder_initialize(iree_allocator_system(), &output);
  loom_output_stream_t stream;
  loom_output_stream_for_builder(&output, &stream);
  IREE_EXPECT_OK(loom_text_print_attribute(&attribute, module_, &stream));
  EXPECT_EQ(std::string(iree_string_builder_buffer(&output)),
            "reg<0x2a:7 : tensor<[%extent$0]xf32>>");
  IREE_EXPECT_STATUS_IS(IREE_STATUS_FAILED_PRECONDITION,
                        loom_text_print_type_with_options(
                            register_type, module_, &stream, nullptr));
  iree_string_builder_deinitialize(&output);
}

TEST_F(NamePlanTest, StandaloneCompositeTypesAndAttributesShareCanonicalNames) {
  auto first = Constant("extent");
  auto second = Constant("extent");
  Constant("extent$0");
  loom_type_t dimensions[] = {DimensionType(first), DimensionType(second)};
  loom_type_t function_type;
  IREE_ASSERT_OK(loom_type_function_build(
      dimensions, IREE_ARRAYSIZE(dimensions), nullptr, 0,
      iree_arena_allocator(&module_->arena), &function_type));
  loom_type_id_t type_id;
  IREE_ASSERT_OK(loom_module_intern_type_id(module_, function_type, &type_id));
  loom_attribute_t attribute = loom_attr_type(type_id);
  const std::string expected =
      "(tensor<[%extent$1$0]xf32>, tensor<[%extent$1]xf32>) -> ()";
  iree_string_builder_t output;
  iree_string_builder_initialize(iree_allocator_system(), &output);
  loom_output_stream_t stream;
  loom_output_stream_for_builder(&output, &stream);
  IREE_EXPECT_OK(loom_text_print_type(function_type, module_, &stream));
  EXPECT_EQ(std::string(iree_string_builder_buffer(&output)), expected);
  iree_string_builder_reset(&output);
  IREE_EXPECT_OK(loom_text_print_type_with_options(function_type, module_,
                                                   &stream, nullptr));
  EXPECT_EQ(std::string(iree_string_builder_buffer(&output)), expected);
  iree_string_builder_reset(&output);
  IREE_EXPECT_OK(loom_text_print_attribute(&attribute, module_, &stream));
  EXPECT_EQ(std::string(iree_string_builder_buffer(&output)), expected);
  iree_string_builder_deinitialize(&output);
}

}  // namespace
