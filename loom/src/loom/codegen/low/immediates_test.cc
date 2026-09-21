// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/codegen/low/immediates.h"

#include "iree/base/internal/arena.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/ir/context.h"
#include "loom/util/bstring.h"

namespace loom {
namespace {

static const uint8_t kStrings[] = LOOM_BSTRING_LITERAL(4, "mode")
    LOOM_BSTRING_LITERAL(7, "reverse") LOOM_BSTRING_LITERAL(7, "forward");
static constexpr uint32_t kReverseOffset = 1 + 4;
static constexpr uint32_t kForwardOffset = kReverseOffset + 1 + 7;

class LowImmediatesTest : public ::testing::Test {
 protected:
  void SetUp() override {
    iree_arena_block_pool_initialize(32 * 1024, iree_allocator_system(),
                                     &block_pool_);
    loom_context_initialize(iree_allocator_system(), &context_);
    IREE_ASSERT_OK(loom_context_finalize(&context_));
    IREE_ASSERT_OK(loom_module_allocate(&context_, IREE_SV("test"),
                                        &block_pool_, nullptr,
                                        iree_allocator_system(), &module_));
    immediate_.kind = LOOM_LOW_IMMEDIATE_KIND_ENUM;
    immediate_.value_step = 1;
    domain_.value_count = IREE_ARRAYSIZE(values_);
    descriptor_set_.string_table = {kStrings, sizeof(kStrings)};
    descriptor_set_.immediates = &immediate_;
    descriptor_set_.immediate_count = 1;
    descriptor_set_.enum_domains = &domain_;
    descriptor_set_.enum_domain_count = 1;
    descriptor_set_.enum_values = values_;
    descriptor_set_.enum_value_count = IREE_ARRAYSIZE(values_);
    descriptor_.immediate_count = 1;
    descriptor_.flags = LOOM_LOW_DESCRIPTOR_FLAG_ENUM_IMMEDIATES;
  }

  void TearDown() override {
    loom_module_free(module_);
    loom_context_deinitialize(&context_);
    iree_arena_block_pool_deinitialize(&block_pool_);
  }

  loom_string_id_t Intern(iree_string_view_t string) {
    loom_string_id_t id = LOOM_STRING_ID_INVALID;
    IREE_CHECK_OK(loom_module_intern_string(module_, string, &id));
    return id;
  }

  // Backing storage for the module's immutable dictionaries.
  iree_arena_block_pool_t block_pool_;
  // Context owning module-independent IR metadata.
  loom_context_t context_;
  // Module owning names, values, and dictionaries.
  loom_module_t* module_ = nullptr;
  // Descriptor's single enum-valued immediate.
  loom_low_immediate_t immediate_ = {};
  // Domain whose semantic values differ from their table positions.
  loom_low_enum_domain_t domain_ = {};
  // Sparse signed values exercising semantic rather than ordinal resolution.
  loom_low_enum_value_t values_[2] = {{kReverseOffset, -5},
                                      {kForwardOffset, 7}};
  // Metadata reached by the descriptor during enum resolution.
  loom_low_descriptor_set_t descriptor_set_ = {};
  // Descriptor owning the mode field.
  loom_low_descriptor_t descriptor_ = {};
};

TEST_F(LowImmediatesTest, ResolvesSemanticValuesWithoutMutatingSharedInput) {
  for (const auto& value : values_) {
    loom_named_attr_t entries[] = {
        {/*.name_id=*/Intern(IREE_SV("mode")),
         /*.reserved=*/0, /*.value=*/
         loom_attr_string(Intern(loom_low_descriptor_set_string(
             &descriptor_set_, value.token_string_offset)))},
        {/*.name_id=*/Intern(IREE_SV("other")),
         /*.reserved=*/0,
         /*.value=*/loom_attr_string(Intern(IREE_SV("forward")))},
    };
    loom_attribute_t original = {};
    IREE_ASSERT_OK(loom_module_make_canonical_attr_dict(
        module_, loom_make_named_attr_slice(entries, IREE_ARRAYSIZE(entries)),
        &original));
    loom_attribute_t resolved = original;
    IREE_ASSERT_OK(loom_low_resolve_immediate_enums(module_, &descriptor_set_,
                                                    &descriptor_, &resolved));
    EXPECT_NE(original.dict_entries, resolved.dict_entries);
    EXPECT_EQ(original.dict_entries[0].value.kind, LOOM_ATTR_STRING);
    EXPECT_EQ(resolved.dict_entries[0].value.kind, LOOM_ATTR_I64);
    EXPECT_EQ(resolved.dict_entries[0].value.i64, value.value);
    EXPECT_EQ(resolved.dict_entries[1].value.kind, LOOM_ATTR_STRING);
    EXPECT_EQ(resolved.dict_entries[1].value.string_id,
              original.dict_entries[1].value.string_id);

    const loom_named_attr_t* canonical_entries = resolved.dict_entries;
    IREE_ASSERT_OK(loom_low_resolve_immediate_enums(module_, &descriptor_set_,
                                                    &descriptor_, &resolved));
    EXPECT_EQ(resolved.dict_entries, canonical_entries);
  }
}

TEST_F(LowImmediatesTest, PreservesNumericAndUnrecognizedInputForVerification) {
  const loom_attribute_t values[] = {
      loom_attr_i64(7), loom_attr_i64(99),
      loom_attr_string(Intern(IREE_SV("unknown"))), loom_attr_bool(true)};
  for (const auto& value : values) {
    loom_named_attr_t entry = {/*.name_id=*/Intern(IREE_SV("mode")),
                               /*.reserved=*/0, /*.value=*/value};
    loom_attribute_t attrs = {};
    IREE_ASSERT_OK(loom_module_make_canonical_attr_dict(
        module_, loom_make_named_attr_slice(&entry, 1), &attrs));
    const loom_named_attr_t* original_entries = attrs.dict_entries;
    IREE_ASSERT_OK(loom_low_resolve_immediate_enums(module_, &descriptor_set_,
                                                    &descriptor_, &attrs));
    EXPECT_EQ(attrs.dict_entries, original_entries);
  }
}

}  // namespace
}  // namespace loom
