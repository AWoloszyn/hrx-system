// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/codegen/low/memory_access_ir.h"

#include "iree/base/internal/arena.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/codegen/low/function.h"
#include "loom/codegen/low/memory_access_builder.h"
#include "loom/codegen/low/text_asm.h"
#include "loom/error/diagnostic.h"
#include "loom/format/text/parser.h"
#include "loom/ir/context.h"
#include "loom/ir/module.h"
#include "loom/ops/low/ops.h"
#include "loom/ops/test/ops.h"
#include "loom/target/test/descriptors.h"
#include "loom/testing/module_ptr.h"

namespace loom {
namespace {

using ModulePtr = ::loom::testing::ModulePtr;

static const loom_low_descriptor_set_provider_t kDescriptorSetProviders[] = {
    loom_test_low_core_descriptor_set,
};

class LowMemoryAccessIrTest : public ::testing::Test {
 protected:
  void SetUp() override {
    iree_arena_block_pool_initialize(4096, iree_allocator_system(),
                                     &block_pool_);
    loom_context_initialize(iree_allocator_system(), &context_);
    RegisterDialect(LOOM_DIALECT_TEST, loom_test_dialect_vtables);
    RegisterDialect(LOOM_DIALECT_LOW, loom_low_dialect_vtables);
    IREE_ASSERT_OK(loom_context_finalize(&context_));
    descriptor_registry_.descriptor_set_providers = kDescriptorSetProviders;
    descriptor_registry_.descriptor_set_provider_count =
        IREE_ARRAYSIZE(kDescriptorSetProviders);
  }

  void TearDown() override {
    loom_context_deinitialize(&context_);
    iree_arena_block_pool_deinitialize(&block_pool_);
  }

  using DialectVtablesFn =
      const loom_op_vtable_t* const* (*)(iree_host_size_t*);

  void RegisterDialect(uint8_t dialect_id,
                       DialectVtablesFn dialect_vtables_fn) {
    iree_host_size_t count = 0;
    const loom_op_vtable_t* const* vtables = dialect_vtables_fn(&count);
    IREE_ASSERT_OK(loom_context_register_dialect(&context_, dialect_id, vtables,
                                                 (uint16_t)count));
  }

  ModulePtr ParseModule(const char* source) {
    loom_module_t* module = nullptr;
    loom_text_parse_options_t options = {};
    options.diagnostic_sink.fn = loom_diagnostic_stderr_sink;
    loom_low_descriptor_text_asm_environment_initialize(
        &descriptor_registry_, &options.low_asm_environment);
    IREE_CHECK_OK(loom_text_parse(iree_make_cstring_view(source),
                                  IREE_SV("memory_access_ir_test.loom"),
                                  &context_, &block_pool_, &options, &module));
    return ModulePtr(module);
  }

  loom_op_t* FindLowFunction(loom_module_t* module) {
    loom_op_t* op = nullptr;
    loom_block_for_each_op(loom_region_entry_block(module->body), op) {
      if (loom_low_func_def_isa(op)) {
        return op;
      }
    }
    ADD_FAILURE() << "low function not found";
    return nullptr;
  }

  loom_op_t* FindLowPacket(loom_op_t* function_op) {
    loom_region_t* body = loom_low_function_body(function_op);
    loom_op_t* op = nullptr;
    loom_block_for_each_op(loom_region_entry_block(body), op) {
      if (loom_low_op_isa(op)) {
        return op;
      }
    }
    ADD_FAILURE() << "low packet not found";
    return nullptr;
  }

  iree_arena_block_pool_t block_pool_;
  loom_context_t context_;
  loom_low_descriptor_registry_t descriptor_registry_ = {};
};

TEST_F(LowMemoryAccessIrTest, AttachesAndReconstructsStridedInterval) {
  ModulePtr module = ParseModule(R"(
test.target<low_core> @target
low.func.def target<test.low.core>(@target) @memory_access(%address: reg<test.ptr>) -> (reg<test.i32 x4>) asm {
  %loaded = test.load.v4i32 %address
  return %loaded
}
)");
  ASSERT_NE(module, nullptr);
  loom_op_t* function_op = FindLowFunction(module.get());
  ASSERT_NE(function_op, nullptr);
  loom_op_t* packet_op = FindLowPacket(function_op);
  ASSERT_NE(packet_op, nullptr);

  loom_low_byte_interval_t interval = {
      /*.begin_facts=*/loom_value_facts_make(16, 4048, 16),
      /*.end_facts=*/loom_value_facts_make(32, 4064, 16),
      /*.begin_expr_id=*/LOOM_LOW_MEMORY_EXPR_ID_NONE,
      /*.end_expr_id=*/LOOM_LOW_MEMORY_EXPR_ID_NONE,
      /*.precision_flags=*/LOOM_LOW_BYTE_INTERVAL_PRECISION_BEGIN_RANGE |
          LOOM_LOW_BYTE_INTERVAL_PRECISION_END_RANGE,
  };
  loom_low_memory_access_summary_t summary = {
      /*.memory_space=*/LOOM_LOW_MEMORY_SPACE_WORKGROUP,
      /*.alias_root_id=*/7,
      /*.alias_group_id=*/LOOM_LOW_MEMORY_ALIAS_ID_NONE,
      /*.precision_flags=*/LOOM_LOW_MEMORY_ACCESS_PRECISION_SPACE |
          LOOM_LOW_MEMORY_ACCESS_PRECISION_ROOT |
          LOOM_LOW_MEMORY_ACCESS_PRECISION_INTERVAL |
          LOOM_LOW_MEMORY_ACCESS_PRECISION_STRIDED_INTERVAL,
      /*.strided_interval=*/
      {
          /*.stride_bytes=*/64,
          /*.begin_bytes=*/16,
          /*.end_bytes=*/32,
      },
      /*.byte_interval=*/&interval,
  };
  IREE_ASSERT_OK(
      loom_low_memory_access_ir_attach(module.get(), packet_op, &summary));

  iree_arena_allocator_t arena;
  iree_arena_initialize(&block_pool_, &arena);
  loom_low_memory_access_table_t table = {};
  IREE_ASSERT_OK(
      loom_low_memory_access_table_build_from_ir(function_op, &arena, &table));
  ASSERT_EQ(table.count, 1u);
  EXPECT_EQ(table.function_op, function_op);
  EXPECT_EQ(table.values[0].op, packet_op);
  EXPECT_EQ(table.values[0].summary.memory_space,
            LOOM_LOW_MEMORY_SPACE_WORKGROUP);
  EXPECT_EQ(table.values[0].summary.alias_root_id, 7u);
  EXPECT_EQ(table.values[0].summary.strided_interval.stride_bytes, 64u);
  EXPECT_EQ(table.values[0].summary.strided_interval.begin_bytes, 16u);
  EXPECT_EQ(table.values[0].summary.strided_interval.end_bytes, 32u);
  ASSERT_NE(table.values[0].summary.byte_interval, nullptr);
  EXPECT_EQ(table.values[0].summary.byte_interval->begin_facts.range_lo, 16);
  EXPECT_EQ(table.values[0].summary.byte_interval->begin_facts.range_hi, 4048);
  EXPECT_EQ(table.values[0].summary.byte_interval->end_facts.range_lo, 32);
  EXPECT_EQ(table.values[0].summary.byte_interval->end_facts.range_hi, 4064);
  iree_arena_deinitialize(&arena);
}

TEST_F(LowMemoryAccessIrTest, PublishesOwnedIntervalsAcrossCollectorChunks) {
  ModulePtr module = ParseModule(R"(
test.target<low_core> @target
low.func.def target<test.low.core>(@target) @memory_access(%address: reg<test.ptr>, %value: reg<test.i32 x4>) asm {
  test.store.v4i32 %address, %value
  test.store.v4i32 %address, %value
  test.store.v4i32 %address, %value
  test.store.v4i32 %address, %value
  test.store.v4i32 %address, %value
  test.store.v4i32 %address, %value
  test.store.v4i32 %address, %value
  test.store.v4i32 %address, %value
  test.store.v4i32 %address, %value
  test.store.v4i32 %address, %value
  test.store.v4i32 %address, %value
  test.store.v4i32 %address, %value
  test.store.v4i32 %address, %value
  test.store.v4i32 %address, %value
  test.store.v4i32 %address, %value
  test.store.v4i32 %address, %value
  test.store.v4i32 %address, %value
  return
}
)");
  ASSERT_NE(module, nullptr);
  loom_op_t* function_op = FindLowFunction(module.get());
  ASSERT_NE(function_op, nullptr);
  loom_block_t* block =
      loom_region_entry_block(loom_low_function_body(function_op));

  iree_arena_allocator_t scratch_arena;
  iree_arena_allocator_t arena;
  iree_arena_initialize(&block_pool_, &scratch_arena);
  iree_arena_initialize(&block_pool_, &arena);
  loom_low_memory_access_builder_t builder = {};
  loom_low_byte_interval_t interval = {
      /*.begin_facts=*/loom_value_facts_exact_i64(0),
      /*.end_facts=*/loom_value_facts_exact_i64(16),
      /*.begin_expr_id=*/LOOM_LOW_MEMORY_EXPR_ID_NONE,
      /*.end_expr_id=*/LOOM_LOW_MEMORY_EXPR_ID_NONE,
      /*.precision_flags=*/LOOM_LOW_BYTE_INTERVAL_PRECISION_BEGIN_RANGE |
          LOOM_LOW_BYTE_INTERVAL_PRECISION_END_RANGE |
          LOOM_LOW_BYTE_INTERVAL_PRECISION_EXACT_LENGTH,
  };
  const loom_low_byte_interval_t expected_interval = interval;
  loom_op_t* op = nullptr;
  loom_block_for_each_op(block, op) {
    if (!loom_low_op_isa(op)) {
      continue;
    }
    loom_low_memory_access_record_t record = {};
    record.position.block_index = 0;
    record.position.block_ordinal = op->block_ordinal;
    record.op = op;
    record.summary =
        *loom_low_memory_access_summary_for_space(LOOM_LOW_MEMORY_SPACE_GLOBAL);
    record.summary.alias_root_id = 1;
    record.summary.precision_flags |= LOOM_LOW_MEMORY_ACCESS_PRECISION_ROOT;
    if (builder.count % 2 == 0) {
      record.summary.precision_flags |=
          LOOM_LOW_MEMORY_ACCESS_PRECISION_INTERVAL;
      record.summary.byte_interval = &interval;
    }
    IREE_ASSERT_OK(loom_low_memory_access_builder_append(&builder, &record,
                                                         &scratch_arena));
  }
  loom_low_memory_access_table_t table = {};
  IREE_ASSERT_OK(loom_low_memory_access_builder_finish(&builder, function_op,
                                                       &arena, &table));
  iree_arena_reset(&scratch_arena);
  interval = {};

  ASSERT_EQ(table.count, 17u);
  EXPECT_EQ(table.function_op, function_op);
  op = block->first_op;
  for (iree_host_size_t i = 0; i < table.count; ++i, op = op->next_op) {
    const loom_low_memory_access_record_t& record = table.values[i];
    EXPECT_EQ(record.op, op);
    EXPECT_EQ(record.position.block_index, 0);
    EXPECT_EQ(record.position.block_ordinal, op->block_ordinal);
    EXPECT_EQ(record.summary.alias_root_id, 1u);
    if (i % 2 != 0) {
      EXPECT_EQ(record.summary.byte_interval, nullptr);
      continue;
    }
    ASSERT_NE(record.summary.byte_interval, nullptr);
    const loom_low_byte_interval_t& actual = *record.summary.byte_interval;
    EXPECT_EQ(actual.begin_facts.range_lo,
              expected_interval.begin_facts.range_lo);
    EXPECT_EQ(actual.begin_facts.range_hi,
              expected_interval.begin_facts.range_hi);
    EXPECT_EQ(actual.begin_facts.known_divisor,
              expected_interval.begin_facts.known_divisor);
    EXPECT_EQ(actual.end_facts.range_lo, expected_interval.end_facts.range_lo);
    EXPECT_EQ(actual.end_facts.range_hi, expected_interval.end_facts.range_hi);
    EXPECT_EQ(actual.end_facts.known_divisor,
              expected_interval.end_facts.known_divisor);
    EXPECT_EQ(actual.begin_facts.flags, expected_interval.begin_facts.flags);
    EXPECT_EQ(actual.end_facts.flags, expected_interval.end_facts.flags);
    EXPECT_EQ(actual.begin_facts.extension_id,
              expected_interval.begin_facts.extension_id);
    EXPECT_EQ(actual.end_facts.extension_id,
              expected_interval.end_facts.extension_id);
    EXPECT_EQ(actual.begin_expr_id, expected_interval.begin_expr_id);
    EXPECT_EQ(actual.end_expr_id, expected_interval.end_expr_id);
    EXPECT_EQ(actual.precision_flags, expected_interval.precision_flags);
  }
  iree_arena_deinitialize(&scratch_arena);
  iree_arena_deinitialize(&arena);
}

TEST_F(LowMemoryAccessIrTest, RejectsUnsupportedUnserializablePrecision) {
  ModulePtr module = ParseModule(R"(
test.target<low_core> @target
low.func.def target<test.low.core>(@target) @memory_access(%address: reg<test.ptr>) -> (reg<test.i32 x4>) asm {
  %loaded = test.load.v4i32 %address
  return %loaded
}
)");
  ASSERT_NE(module, nullptr);
  loom_op_t* packet_op = FindLowPacket(FindLowFunction(module.get()));
  ASSERT_NE(packet_op, nullptr);

  const loom_low_memory_access_summary_t summary = {
      /*.memory_space=*/LOOM_LOW_MEMORY_SPACE_WORKGROUP,
      /*.alias_root_id=*/LOOM_LOW_MEMORY_ALIAS_ID_NONE,
      /*.alias_group_id=*/LOOM_LOW_MEMORY_ALIAS_ID_NONE,
      /*.precision_flags=*/LOOM_LOW_MEMORY_ACCESS_PRECISION_SPACE |
          LOOM_LOW_MEMORY_ACCESS_PRECISION_EXACT_LANES,
  };
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_UNIMPLEMENTED,
      loom_low_memory_access_ir_attach(module.get(), packet_op, &summary));
  EXPECT_TRUE(loom_attr_is_absent(loom_low_op_memory_access(packet_op)));
}

TEST_F(LowMemoryAccessIrTest, RejectsFieldsWithoutMatchingPrecision) {
  ModulePtr module = ParseModule(R"(
test.target<low_core> @target
low.func.def target<test.low.core>(@target) @memory_access(%address: reg<test.ptr>) -> (reg<test.i32 x4>) asm {
  %loaded = test.load.v4i32 %address
  return %loaded
}
)");
  ASSERT_NE(module, nullptr);
  loom_op_t* packet_op = FindLowPacket(FindLowFunction(module.get()));
  ASSERT_NE(packet_op, nullptr);

  const loom_low_memory_access_summary_t summary = {
      /*.memory_space=*/LOOM_LOW_MEMORY_SPACE_WORKGROUP,
      /*.alias_root_id=*/7,
      /*.alias_group_id=*/LOOM_LOW_MEMORY_ALIAS_ID_NONE,
      /*.precision_flags=*/LOOM_LOW_MEMORY_ACCESS_PRECISION_SPACE,
  };
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      loom_low_memory_access_ir_attach(module.get(), packet_op, &summary));
  EXPECT_TRUE(loom_attr_is_absent(loom_low_op_memory_access(packet_op)));
}

TEST_F(LowMemoryAccessIrTest, RejectsMalformedFieldCount) {
  ModulePtr module = ParseModule(R"(
test.target<low_core> @target
low.func.def target<test.low.core>(@target) @memory_access(%address: reg<test.ptr>) -> (reg<test.i32 x4>) asm {
  %loaded = low.op<test.load.v4i32>(%address) memory_access([0, 3]) : (reg<test.ptr>) -> reg<test.i32 x4>
  return %loaded
}
)");
  ASSERT_NE(module, nullptr);
  loom_op_t* function_op = FindLowFunction(module.get());
  ASSERT_NE(function_op, nullptr);

  iree_arena_allocator_t arena;
  iree_arena_initialize(&block_pool_, &arena);
  loom_low_memory_access_table_t table = {};
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      loom_low_memory_access_table_build_from_ir(function_op, &arena, &table));
  iree_arena_deinitialize(&arena);
}

TEST_F(LowMemoryAccessIrTest, RejectsUnsupportedVersion) {
  ModulePtr module = ParseModule(R"(
test.target<low_core> @target
low.func.def target<test.low.core>(@target) @memory_access(%address: reg<test.ptr>) -> (reg<test.i32 x4>) asm {
  %loaded = low.op<test.load.v4i32>(%address) memory_access([1, 3, 7, -1, 35, 64, 0, 16, 0, 0, 0, 0, 0]) : (reg<test.ptr>) -> reg<test.i32 x4>
  return %loaded
}
)");
  ASSERT_NE(module, nullptr);
  loom_op_t* function_op = FindLowFunction(module.get());
  ASSERT_NE(function_op, nullptr);

  iree_arena_allocator_t arena;
  iree_arena_initialize(&block_pool_, &arena);
  loom_low_memory_access_table_t table = {};
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_UNIMPLEMENTED,
      loom_low_memory_access_table_build_from_ir(function_op, &arena, &table));
  iree_arena_deinitialize(&arena);
}

TEST_F(LowMemoryAccessIrTest, RejectsSentinelPreciseAliasRoot) {
  ModulePtr module = ParseModule(R"(
test.target<low_core> @target
low.func.def target<test.low.core>(@target) @memory_access(%address: reg<test.ptr>) -> (reg<test.i32 x4>) asm {
  %loaded = low.op<test.load.v4i32>(%address) memory_access([0, 3, 4294967295, -1, 35, 64, 0, 16, 0, 0, 0, 0, 0]) : (reg<test.ptr>) -> reg<test.i32 x4>
  return %loaded
}
)");
  ASSERT_NE(module, nullptr);
  loom_op_t* function_op = FindLowFunction(module.get());
  ASSERT_NE(function_op, nullptr);

  iree_arena_allocator_t arena;
  iree_arena_initialize(&block_pool_, &arena);
  loom_low_memory_access_table_t table = {};
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      loom_low_memory_access_table_build_from_ir(function_op, &arena, &table));
  iree_arena_deinitialize(&arena);
}

TEST_F(LowMemoryAccessIrTest, RejectsImpreciseConcreteSpace) {
  ModulePtr module = ParseModule(R"(
test.target<low_core> @target
low.func.def target<test.low.core>(@target) @memory_access(%address: reg<test.ptr>) -> (reg<test.i32 x4>) asm {
  %loaded = low.op<test.load.v4i32>(%address) memory_access([0, 3, 7, -1, 2, 0, 0, 0, 0, 0, 0, 0, 0]) : (reg<test.ptr>) -> reg<test.i32 x4>
  return %loaded
}
)");
  ASSERT_NE(module, nullptr);
  loom_op_t* function_op = FindLowFunction(module.get());
  ASSERT_NE(function_op, nullptr);

  iree_arena_allocator_t arena;
  iree_arena_initialize(&block_pool_, &arena);
  loom_low_memory_access_table_t table = {};
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_INVALID_ARGUMENT,
      loom_low_memory_access_table_build_from_ir(function_op, &arena, &table));
  iree_arena_deinitialize(&arena);
}

}  // namespace
}  // namespace loom
