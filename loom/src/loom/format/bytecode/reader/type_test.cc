// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/format/bytecode/reader/type.h"

#include <cstring>
#include <vector>

#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/format/bytecode/format.h"
#include "loom/format/bytecode/reader/module_view.h"
#include "loom/format/bytecode/reader/type_validator.h"
#include "loom/ops/type_registry.h"

namespace loom {
namespace {

struct DiagnosticSnapshot {
  // Static error descriptor identifying the failed validation rule.
  const loom_error_def_t* error = nullptr;
  // First byte of the diagnosed wire range.
  iree_host_size_t start = 0;
  // Exclusive end of the diagnosed wire range.
  iree_host_size_t end = 0;
};

static iree_status_t CaptureDiagnostic(void* user_data,
                                       const loom_diagnostic_t* diagnostic) {
  auto* snapshot = static_cast<DiagnosticSnapshot*>(user_data);
  *snapshot = {diagnostic->error, diagnostic->origin.start,
               diagnostic->origin.end};
  return iree_ok_status();
}

static const loom_attr_descriptor_t kParameters[] = {
    {/*.name=*/LOOM_BSTRING_REF(5, "first"),
     /*.attr_kind=*/LOOM_ATTR_I64,
     /*.flags=*/0},
    {/*.name=*/LOOM_BSTRING_REF(6, "middle"),
     /*.attr_kind=*/LOOM_ATTR_I64,
     /*.flags=*/LOOM_ATTR_OPTIONAL},
    {/*.name=*/LOOM_BSTRING_REF(4, "last"),
     /*.attr_kind=*/LOOM_ATTR_I64,
     /*.flags=*/0},
};
static const loom_parameterized_type_descriptor_t kParameterDescriptor = {
    /*.name=*/LOOM_BSTRING_REF(15, "wire.parameters"),
    /*.parameter_descriptors=*/kParameters,
    /*.ir_kind=*/LOOM_TYPE_PARAMETERIZED,
    /*.type_flags=*/0,
    /*.parameter_count=*/IREE_ARRAYSIZE(kParameters),
    /*.flags=*/0,
};
static const loom_type_descriptor_t kTypeDescriptor = {
    /*.name=*/LOOM_BSTRING_REF(15, "wire.parameters<"),
    /*.ir_kind=*/LOOM_TYPE_PARAMETERIZED,
    /*.param_count=*/IREE_ARRAYSIZE(kParameters),
    /*.fact_domain=*/nullptr,
    /*.semantics=*/{},
    /*.format_elements=*/nullptr,
    /*.format_element_count=*/0,
    /*.parameterized=*/&kParameterDescriptor,
};
static const loom_type_registry_entry_t kTypeRegistry[] = {
    {IREE_SV("wire.parameters"), &kTypeDescriptor},
};

class BytecodeTypeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    iree_arena_block_pool_initialize(4096, iree_allocator_system(),
                                     &block_pool_);
    iree_arena_initialize(&block_pool_, &scratch_arena_);
    loom_context_initialize(iree_allocator_system(), &context_);
    IREE_ASSERT_OK(loom_type_registry_register_types(
        &context_, kTypeRegistry, IREE_ARRAYSIZE(kTypeRegistry)));
    IREE_ASSERT_OK(loom_context_finalize(&context_));
    IREE_ASSERT_OK(loom_module_allocate(&context_, IREE_SV("type_test"),
                                        &block_pool_, nullptr,
                                        iree_allocator_system(), &module_));
    loom_bytecode_reader_decoder_initialize(
        loom_diagnostic_sink_t{CaptureDiagnostic, &diagnostic_},
        IREE_SV("type_test.loombc"), &error_count_, &decoder_);
  }

  void TearDown() override {
    loom_module_free(module_);
    loom_context_deinitialize(&context_);
    iree_arena_deinitialize(&scratch_arena_);
    iree_arena_block_pool_deinitialize(&block_pool_);
  }

  iree_status_t BuildPlan(const uint8_t* data, iree_host_size_t length) {
    return loom_bytecode_type_plan_build(
        &decoder_, &context_, &module_view_, &scratch_arena_,
        iree_make_const_byte_span(data, length),
        /*section_absolute_offset=*/0);
  }

  iree_status_t DecodeEntry(loom_type_id_t type_index, const uint8_t* data,
                            iree_host_size_t length, uint64_t absolute_offset,
                            loom_bytecode_type_plan_entry_t* out_entry,
                            loom_bytecode_type_fact_t** out_fact) {
    return loom_bytecode_type_plan_decode_indexed_entry(
        &decoder_, &context_, &module_view_, &scratch_arena_, type_index,
        iree_make_const_byte_span(data, length), absolute_offset, out_entry,
        out_fact);
  }

  loom_bytecode_type_materializer_t MakeMaterializer(const uint8_t* data,
                                                     iree_host_size_t length) {
    return loom_bytecode_type_materializer_t{
        /*.decoder=*/&decoder_,
        /*.bytecode=*/iree_make_const_byte_span(data, length),
        /*.context=*/&context_,
        /*.module_view=*/&module_view_,
        /*.scratch_arena=*/&scratch_arena_,
        /*.output_module=*/module_,
    };
  }

  void CheckValidationModes(const uint8_t* data, iree_host_size_t length,
                            iree_status_code_t expected_status) {
    module_view_.types = {};
    error_count_ = 0;
    diagnostic_ = {};
    auto status = BuildPlan(data, length);
    const auto plan_status = iree_status_code(status);
    iree_status_ignore(status);
    ASSERT_EQ(plan_status, expected_status);
    const DiagnosticSnapshot expected_diagnostic = diagnostic_;
    const auto expected_count = module_view_.types.count;
    std::vector<uint64_t> offsets;
    if (expected_status == IREE_STATUS_OK) {
      for (iree_host_size_t i = 0; i < expected_count; ++i) {
        offsets.push_back(module_view_.types.entries[i].bytecode_offset);
      }
      offsets.push_back(length);
    }
    iree_arena_reset(&scratch_arena_);
    module_view_.types = {};
    error_count_ = 0;
    diagnostic_ = {};
    status = loom_bytecode_type_table_validate(
        &decoder_, &context_, &module_view_,
        iree_make_const_byte_span(data, length), 0);
    EXPECT_EQ(iree_status_code(status), expected_status);
    iree_status_ignore(status);
    EXPECT_EQ(module_view_.types.count, expected_count);
    EXPECT_EQ(module_view_.types.entries, nullptr);
    EXPECT_EQ(module_view_.types.facts, nullptr);
    EXPECT_EQ(scratch_arena_.used_allocation_size, 0u);
    EXPECT_EQ(diagnostic_.error, expected_diagnostic.error);
    EXPECT_EQ(diagnostic_.start, expected_diagnostic.start);
    EXPECT_EQ(diagnostic_.end, expected_diagnostic.end);

    module_view_.types = {};
    error_count_ = 0;
    diagnostic_ = {};
    iree_arena_allocator_t retained_arena;
    iree_arena_initialize(&block_pool_, &retained_arena);
    loom_bytecode_table_entry_metadata_t* entries = nullptr;
    iree_host_size_t count = 0;
    status =
        loom_bytecode_type_table_index(&decoder_, &context_, &module_view_,
                                       iree_make_const_byte_span(data, length),
                                       0, &retained_arena, &entries, &count);
    EXPECT_EQ(iree_status_code(status), expected_status);
    iree_status_ignore(status);
    EXPECT_EQ(module_view_.types.entries, nullptr);
    EXPECT_EQ(module_view_.types.facts, nullptr);
    EXPECT_EQ(diagnostic_.error, expected_diagnostic.error);
    EXPECT_EQ(diagnostic_.start, expected_diagnostic.start);
    EXPECT_EQ(diagnostic_.end, expected_diagnostic.end);
    if (expected_status == IREE_STATUS_OK) {
      EXPECT_EQ(count, expected_count);
      EXPECT_EQ(retained_arena.used_allocation_size,
                expected_count * sizeof(*entries));
      // Index entries remain usable after validation scratch is reset.
      iree_arena_reset(&scratch_arena_);
      for (iree_host_size_t i = 0; i < count; ++i) {
        EXPECT_EQ(entries[i].entry_offset, offsets[i]);
        EXPECT_EQ(entries[i].entry_length, offsets[i + 1] - offsets[i]);
      }
    } else {
      EXPECT_EQ(entries, nullptr);
      EXPECT_EQ(count, 0u);
    }
    iree_arena_deinitialize(&retained_arena);
  }

  // Last diagnostic's stable identity and wire range.
  DiagnosticSnapshot diagnostic_;
  // Minimal validated module facts receiving each test's type plan.
  loom_bytecode_reader_module_view_t module_view_ = {};
  // Bounded wire decoder sharing this fixture's diagnostic count.
  loom_bytecode_reader_decoder_t decoder_ = {};
  // Number of accepted malformed-input diagnostics.
  uint32_t error_count_ = 0;
  // Block source shared by scratch and output-module arenas.
  iree_arena_block_pool_t block_pool_;
  // Storage owning the immutable plan and temporary type payloads.
  iree_arena_allocator_t scratch_arena_;
  // Finalized registry with one descriptor-backed wire fixture family.
  loom_context_t context_;
  // Output module receiving canonical materialized types.
  loom_module_t* module_ = nullptr;
};

TEST_F(BytecodeTypeTest, BuildsAndMaterializesTopologicalPlan) {
  const uint8_t data[] = {
      0x03,
      LOOM_BYTECODE_TYPE_NONE,
      LOOM_BYTECODE_TYPE_SCALAR,
      LOOM_SCALAR_TYPE_I32,
      LOOM_BYTECODE_TYPE_FUNCTION,
      0x01,
      0x01,
      0x01,
      0x00,
  };
  IREE_ASSERT_OK(BuildPlan(data, sizeof(data)));

  ASSERT_EQ(module_view_.types.count, 3u);
  EXPECT_EQ(module_view_.types.entries[0].bytecode_offset, 1u);
  EXPECT_EQ(module_view_.types.entries[1].bytecode_offset, 2u);
  EXPECT_EQ(module_view_.types.entries[2].bytecode_offset, 4u);
  ASSERT_NE(module_view_.types.facts, nullptr);
  EXPECT_EQ(module_view_.types.facts->type_id, 2u);
  EXPECT_EQ(module_view_.types.facts->next, nullptr);

  loom_bytecode_type_materializer_t materializer =
      MakeMaterializer(data, sizeof(data));
  IREE_ASSERT_OK(loom_bytecode_type_materialize(&materializer));
  ASSERT_EQ(module_->types.count, 3u);
  EXPECT_EQ(loom_type_kind(module_->types.entries[0]), LOOM_TYPE_NONE);
  EXPECT_EQ(loom_type_kind(module_->types.entries[1]), LOOM_TYPE_SCALAR);
  EXPECT_EQ(loom_type_kind(module_->types.entries[2]), LOOM_TYPE_FUNCTION);
  EXPECT_EQ(error_count_, 0u);
}

TEST_F(BytecodeTypeTest, RetainsOnlyRequestedFactsForMixedTypes) {
  iree_string_view_t strings[] = {
      IREE_SV("wire.parameters"), IREE_SV("first"),
      IREE_SV("middle"),          IREE_SV("last"),
      IREE_SV("example.wrapper"),
  };
  module_view_.strings = {strings, IREE_ARRAYSIZE(strings)};
  const uint8_t data[] = {
      7,
      LOOM_BYTECODE_TYPE_NONE,
      LOOM_BYTECODE_TYPE_SCALAR,
      LOOM_SCALAR_TYPE_I32,
      LOOM_BYTECODE_TYPE_TILE,
      LOOM_SCALAR_TYPE_I32,
      3,
      LOOM_BYTECODE_ENCODING_ATTACHMENT_NONE,
      0,
      0,
      1,
      0,
      2,
      0,
      3,
      LOOM_BYTECODE_TYPE_FUNCTION,
      1,
      1,
      1,
      2,
      LOOM_BYTECODE_TYPE_DIALECT,
      4,
      1,
      3,
      LOOM_BYTECODE_TYPE_REGISTER,
      1,
      0x80,
      0x80,
      0x04,
      1,
      1,
      LOOM_BYTECODE_TYPE_PARAMETERIZED,
      0,
      2,
      1,
      LOOM_BYTECODE_ATTR_I64,
      2,
      3,
      LOOM_BYTECODE_ATTR_I64,
      4,
  };
  ASSERT_NO_FATAL_FAILURE(
      CheckValidationModes(data, sizeof(data), IREE_STATUS_OK));
  for (iree_host_size_t length = 0; length < sizeof(data); ++length) {
    SCOPED_TRACE(length);
    ASSERT_NO_FATAL_FAILURE(
        CheckValidationModes(data, length, IREE_STATUS_DEFERRED));
  }
}

TEST_F(BytecodeTypeTest, EmptyValidationAndIndexNeedNoStorage) {
  const uint8_t data[] = {0};
  ASSERT_NO_FATAL_FAILURE(
      CheckValidationModes(data, sizeof(data), IREE_STATUS_OK));
}

TEST_F(BytecodeTypeTest, RetentionDoesNotChangeParameterValidation) {
  iree_string_view_t strings[] = {IREE_SV("wire.parameters"), IREE_SV("first"),
                                  IREE_SV("middle"), IREE_SV("last")};
  module_view_.strings = {strings, IREE_ARRAYSIZE(strings)};
  const std::vector<std::vector<uint8_t>> entries = {
      // Both required parameters absent.
      {0},
      // Required leading parameter absent.
      {1, 3, LOOM_BYTECODE_ATTR_I64, 2},
      // Required trailing parameter absent.
      {1, 1, LOOM_BYTECODE_ATTR_I64, 2},
      // Declared parameters out of order.
      {2, 3, LOOM_BYTECODE_ATTR_I64, 2, 1, LOOM_BYTECODE_ATTR_I64, 2},
      // Repeated parameter.
      {2, 1, LOOM_BYTECODE_ATTR_I64, 2, 1, LOOM_BYTECODE_ATTR_I64, 2},
      // Unknown parameter name.
      {1, 0, LOOM_BYTECODE_ATTR_I64, 2},
      // Value kind mismatches its descriptor.
      {1, 1, LOOM_BYTECODE_ATTR_TYPE, 0},
      // Present count exceeds the descriptor.
      {4},
  };
  for (const auto& parameters : entries) {
    std::vector<uint8_t> data = {1, LOOM_BYTECODE_TYPE_PARAMETERIZED, 0};
    data.insert(data.end(), parameters.begin(), parameters.end());
    ASSERT_NO_FATAL_FAILURE(
        CheckValidationModes(data.data(), data.size(), IREE_STATUS_DEFERRED));
  }
}

TEST_F(BytecodeTypeTest, RejectsNonTopologicalTypeReference) {
  const uint8_t data[] = {
      0x01, LOOM_BYTECODE_TYPE_FUNCTION, 0x01, 0x00, 0x00,
  };
  IREE_EXPECT_STATUS_IS(IREE_STATUS_DEFERRED, BuildPlan(data, sizeof(data)));
  EXPECT_EQ(error_count_, 1u);
}

TEST_F(BytecodeTypeTest, MaterializesEmptyFunctionPayload) {
  const uint8_t data[] = {0x01, LOOM_BYTECODE_TYPE_FUNCTION, 0x00, 0x00};
  IREE_ASSERT_OK(BuildPlan(data, sizeof(data)));
  loom_bytecode_type_materializer_t materializer =
      MakeMaterializer(data, sizeof(data));
  IREE_ASSERT_OK(loom_bytecode_type_materialize(&materializer));
  ASSERT_EQ(module_->types.count, 1u);
  const loom_func_type_data_t* payload =
      loom_type_func_data(module_->types.entries[0]);
  ASSERT_NE(payload, nullptr);
  EXPECT_EQ(payload->arg_count, 0u);
  EXPECT_EQ(payload->result_count, 0u);
  EXPECT_EQ(payload->reserved, 0u);
}

TEST_F(BytecodeTypeTest, MaterializesFullWidthFunctionSignature) {
  std::vector<uint8_t> data = {
      0x03,
      LOOM_BYTECODE_TYPE_SCALAR,
      LOOM_SCALAR_TYPE_I32,
      LOOM_BYTECODE_TYPE_SCALAR,
      LOOM_SCALAR_TYPE_F32,
      LOOM_BYTECODE_TYPE_FUNCTION,
      0xFF,
      0xFF,
      0x03,  // UINT16_MAX arguments.
      0xFF,
      0xFF,
      0x03,  // UINT16_MAX results.
  };
  data.insert(data.end(), UINT16_MAX, /*argument_type_id=*/0);
  data.insert(data.end(), UINT16_MAX, /*result_type_id=*/1);
  IREE_ASSERT_OK(BuildPlan(data.data(), data.size()));
  EXPECT_EQ(module_view_.types.entries[2].structural.dependency_count,
            2u * UINT16_MAX);
  loom_bytecode_type_materializer_t materializer =
      MakeMaterializer(data.data(), data.size());
  IREE_ASSERT_OK(loom_bytecode_type_materialize(&materializer));
  ASSERT_EQ(module_->types.count, 3u);
  const loom_func_type_data_t* payload =
      loom_type_func_data(module_->types.entries[2]);
  ASSERT_NE(payload, nullptr);
  EXPECT_EQ(payload->arg_count, UINT16_MAX);
  EXPECT_EQ(payload->result_count, UINT16_MAX);
  EXPECT_EQ(payload->reserved, 0u);
  for (iree_host_size_t i = 0; i < 2u * UINT16_MAX; ++i) {
    EXPECT_TRUE(loom_type_equal(
        payload->types[i], module_->types.entries[i < UINT16_MAX ? 0 : 1]));
  }
}

TEST_F(BytecodeTypeTest, RejectsOversizedFunctionCountsBeforeAllocation) {
  const uint8_t oversized_arguments[] = {
      0x01, LOOM_BYTECODE_TYPE_FUNCTION, 0x80, 0x80, 0x04, 0x00};
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_DEFERRED,
      BuildPlan(oversized_arguments, sizeof(oversized_arguments)));
  const uint8_t oversized_results[] = {
      0x01, LOOM_BYTECODE_TYPE_FUNCTION, 0x00, 0x80, 0x80, 0x04};
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_DEFERRED,
      BuildPlan(oversized_results, sizeof(oversized_results)));
  EXPECT_EQ(error_count_, 2u);
  EXPECT_EQ(module_->types.count, 0u);
}

TEST_F(BytecodeTypeTest, RejectsNoneScalarType) {
  const uint8_t data[] = {
      LOOM_BYTECODE_TYPE_SCALAR,
      LOOM_SCALAR_TYPE_NONE,
  };
  loom_bytecode_type_plan_entry_t entry = {};
  loom_bytecode_type_fact_t* fact = nullptr;
  IREE_EXPECT_STATUS_IS(IREE_STATUS_DEFERRED,
                        DecodeEntry(/*type_index=*/0, data, sizeof(data),
                                    /*absolute_offset=*/0, &entry, &fact));
  EXPECT_EQ(error_count_, 1u);
}

TEST_F(BytecodeTypeTest, RejectsNoneShapedElementType) {
  const uint8_t data[] = {
      LOOM_BYTECODE_TYPE_TENSOR,
      LOOM_SCALAR_TYPE_NONE,
      /*rank=*/0,
      LOOM_BYTECODE_ENCODING_ATTACHMENT_NONE,
      /*encoding_instance=*/0,
  };
  loom_bytecode_type_plan_entry_t entry = {};
  loom_bytecode_type_fact_t* fact = nullptr;
  IREE_EXPECT_STATUS_IS(IREE_STATUS_DEFERRED,
                        DecodeEntry(/*type_index=*/0, data, sizeof(data),
                                    /*absolute_offset=*/0, &entry, &fact));
  EXPECT_EQ(error_count_, 1u);
}

TEST_F(BytecodeTypeTest, DecodesOneIndexedEntry) {
  const uint8_t data[] = {
      LOOM_BYTECODE_TYPE_FUNCTION, 0x01, 0x01, 0x01, 0x00,
  };
  loom_bytecode_type_plan_entry_t entry = {};
  loom_bytecode_type_fact_t* fact = nullptr;
  IREE_ASSERT_OK(DecodeEntry(/*type_index=*/2, data, sizeof(data),
                             /*absolute_offset=*/41, &entry, &fact));

  EXPECT_EQ(entry.bytecode_offset, 41u);
  ASSERT_NE(fact, nullptr);
  EXPECT_EQ(fact->type_id, 2u);
  EXPECT_EQ(fact->kind, LOOM_TYPE_FUNCTION);
  const auto* structural_fact =
      reinterpret_cast<const loom_bytecode_structural_type_fact_t*>(fact);
  loom_func_type_data_t payload_header;
  memcpy(&payload_header, structural_fact->payload_prefix,
         sizeof(payload_header));
  EXPECT_EQ(payload_header.arg_count, 1u);
  EXPECT_EQ(payload_header.result_count, 1u);
  EXPECT_EQ(payload_header.reserved, 0u);
  EXPECT_EQ(entry.structural.dependency_count, 2u);
  EXPECT_EQ(structural_fact->type_ids[0], 1u);
  EXPECT_EQ(structural_fact->type_ids[1], 0u);
  EXPECT_EQ(error_count_, 0u);
}

}  // namespace
}  // namespace loom
