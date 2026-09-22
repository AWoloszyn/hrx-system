// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <cstdio>
#include <map>
#include <vector>

#include "iree/base/internal/arena.h"
#include "iree/testing/gtest.h"
#include "iree/testing/status_matchers.h"
#include "loom/ir/context.h"
#include "loom/ir/module.h"

namespace loom {
namespace {

class ModuleDictionaryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    iree_arena_block_pool_initialize(32768, iree_allocator_system(), &pool_);
    loom_context_initialize(iree_allocator_system(), &context_);
    IREE_ASSERT_OK(loom_context_finalize(&context_));
    IREE_ASSERT_OK(loom_module_allocate(&context_, IREE_SV("dictionary"),
                                        &pool_, nullptr,
                                        iree_allocator_system(), &module_));
  }

  void TearDown() override {
    loom_module_free(module_);
    loom_context_deinitialize(&context_);
    iree_arena_block_pool_deinitialize(&pool_);
  }

  std::vector<loom_named_attr_t> Entries(uint32_t count) {
    std::vector<loom_named_attr_t> entries(count);
    // Intern in reverse spelling order so comparing IDs cannot sort the keys.
    for (uint32_t i = count; i > 0; --i) {
      char key[32];
      std::snprintf(key, sizeof(key), "parameter_%05u", i - 1);
      IREE_CHECK_OK(loom_module_intern_string(
          module_, iree_make_cstring_view(key), &entries[i - 1].name_id));
      entries[i - 1].value = loom_attr_i64(i - 1);
    }
    return entries;
  }

  void Check(const std::vector<loom_named_attr_t>& entries) {
    loom_attribute_t dictionary = loom_attr_absent();
    IREE_ASSERT_OK(loom_module_make_canonical_attr_dict(
        module_, loom_make_named_attr_slice(entries.data(), entries.size()),
        &dictionary));
    ASSERT_EQ(dictionary.kind, LOOM_ATTR_DICT);
    ASSERT_EQ(dictionary.count, entries.size());
    if (!entries.empty()) {
      EXPECT_NE(dictionary.dict_entries, entries.data());
    }
    for (iree_host_size_t i = 0; i < entries.size(); ++i) {
      const auto& entry = dictionary.dict_entries[i];
      EXPECT_EQ(entry.value.kind, LOOM_ATTR_I64);
      EXPECT_EQ(entry.value.i64, i);
      EXPECT_EQ(entry.reserved, 0u);
      char key[32];
      std::snprintf(key, sizeof(key), "parameter_%05u",
                    static_cast<unsigned>(i));
      EXPECT_TRUE(iree_string_view_equal(
          loom_string_table_get(&module_->strings, entry.name_id),
          iree_make_cstring_view(key)));
    }
    IREE_EXPECT_OK(loom_module_verify_canonical_attr_dict(module_, dictionary));
  }

  // Backing pool for canonical module-owned storage.
  iree_arena_block_pool_t pool_ = {};
  // Minimal context; attribute APIs need no registered operations.
  loom_context_t context_ = {};
  // Module owning interned key identities and copied dictionary payloads.
  loom_module_t* module_ = nullptr;
};

TEST_F(ModuleDictionaryTest, OrderedReversedAndShuffledAcrossSortBoundary) {
  constexpr uint32_t counts[] = {0, 1, 2, 63, 64, 65, 256, 4096, UINT16_MAX};
  for (uint32_t count : counts) {
    SCOPED_TRACE(count);
    auto entries = Entries(count);
    Check(entries);
    std::reverse(entries.begin(), entries.end());
    Check(entries);
    uint32_t random = 0x91441u;
    for (uint32_t i = count; i > 1; --i) {
      random = random * 1664525u + 1013904223u;
      std::swap(entries[i - 1], entries[random % i]);
    }
    Check(entries);
  }
}

TEST_F(ModuleDictionaryTest, SeparatedDuplicatesAreRejectedAfterReordering) {
  for (uint32_t count : {2, 64, 65, 4096}) {
    SCOPED_TRACE(count);
    auto entries = Entries(count);
    std::reverse(entries.begin(), entries.end());
    entries.back().name_id = entries.front().name_id;
    loom_attribute_t dictionary = loom_attr_absent();
    IREE_EXPECT_STATUS_IS(
        IREE_STATUS_INVALID_ARGUMENT,
        loom_module_make_canonical_attr_dict(
            module_, loom_make_named_attr_slice(entries.data(), entries.size()),
            &dictionary));
    EXPECT_EQ(dictionary.kind, LOOM_ATTR_ABSENT);
  }
}

TEST_F(ModuleDictionaryTest, ReorderedPayloadsAreCopiedOnce) {
  auto entries = Entries(65);
  int64_t payload[] = {3, 5, 8};
  for (auto& entry : entries) {
    entry.value = loom_attr_i64_array(payload, IREE_ARRAYSIZE(payload));
  }
  std::reverse(entries.begin(), entries.end());
  const auto used_before = module_->arena.used_allocation_size;
  loom_attribute_t dictionary = loom_attr_absent();
  IREE_ASSERT_OK(loom_module_make_canonical_attr_dict(
      module_, loom_make_named_attr_slice(entries.data(), entries.size()),
      &dictionary));
  const auto used = module_->arena.used_allocation_size - used_before;
  // One output array and one aligned payload copy per entry, with no sort
  // scratch or repeated canonicalization of values during comparisons.
  const auto expected =
      iree_host_align(entries.size() * sizeof(loom_named_attr_t),
                      iree_max_align_t) +
      entries.size() * iree_host_align(sizeof(payload), iree_max_align_t);
  EXPECT_EQ(used, expected);
  payload[0] = 99;
  for (uint16_t i = 0; i < dictionary.count; ++i) {
    const auto& value = dictionary.dict_entries[i].value;
    EXPECT_EQ(value.kind, LOOM_ATTR_I64_ARRAY);
    EXPECT_EQ(value.count, IREE_ARRAYSIZE(payload));
    EXPECT_NE(value.i64_array, payload);
    EXPECT_EQ(value.i64_array[0], 3);
    EXPECT_EQ(value.i64_array[1], 5);
    EXPECT_EQ(value.i64_array[2], 8);
  }
}

TEST_F(ModuleDictionaryTest, InvalidKeysAreRejectedBeforeSorting) {
  auto entries = Entries(65);
  std::reverse(entries.begin(), entries.end());
  for (uint32_t index : {0, 32, 64}) {
    SCOPED_TRACE(index);
    const auto valid_id = entries[index].name_id;
    for (loom_string_id_t invalid_id :
         {static_cast<loom_string_id_t>(module_->strings.count),
          LOOM_STRING_ID_INVALID}) {
      entries[index].name_id = invalid_id;
      loom_attribute_t dictionary = loom_attr_absent();
      IREE_EXPECT_STATUS_IS(
          IREE_STATUS_INVALID_ARGUMENT,
          loom_module_make_canonical_attr_dict(
              module_,
              loom_make_named_attr_slice(entries.data(), entries.size()),
              &dictionary));
      EXPECT_EQ(dictionary.kind, LOOM_ATTR_ABSENT);
    }
    entries[index].name_id = valid_id;
  }
}

TEST_F(ModuleDictionaryTest, ReplacementCapacityUsesTheActualResult) {
  constexpr uint32_t counts[] = {0, 1, 16, 17, 64, 65, 40000, UINT16_MAX};
  for (uint32_t count : counts) {
    SCOPED_TRACE(count);
    auto entries = Entries(count);
    std::vector<loom_named_attr_update_t> updates;
    for (const auto& entry : entries) {
      updates.push_back(loom_named_attr_replace(
          entry.name_id, loom_attr_i64(entry.value.i64 + 1)));
    }
    std::reverse(updates.begin(), updates.end());
    const auto used_before = module_->arena.used_allocation_size;
    loom_attribute_t dictionary = {};
    IREE_ASSERT_OK(loom_module_replace_canonical_attr_dict(
        module_, loom_make_named_attr_slice(entries.data(), entries.size()),
        loom_make_named_attr_update_slice(updates.data(), updates.size()),
        &dictionary));
    ASSERT_EQ(dictionary.count, count);
    // Pooled allocations include alignment padding; oversized requests account
    // for exact payload bytes. Neither retains update ordering scratch.
    const auto retained = module_->arena.used_allocation_size - used_before;
    EXPECT_GE(retained, count * sizeof(loom_named_attr_t));
    EXPECT_LT(retained, count * sizeof(loom_named_attr_t) + iree_max_align_t);
    for (uint32_t i = 0; i < count; ++i) {
      EXPECT_EQ(dictionary.dict_entries[i].name_id, entries[i].name_id);
      EXPECT_EQ(dictionary.dict_entries[i].value.i64, i + 1);
      EXPECT_EQ(entries[i].value.i64, i);
      EXPECT_EQ(updates[i].name_id, entries[count - i - 1].name_id);
      EXPECT_EQ(updates[i].value.i64, count - i);
    }

    for (auto& update : updates) {
      update = loom_named_attr_remove(update.name_id);
    }
    const auto before_removal = module_->arena.used_allocation_size;
    IREE_ASSERT_OK(loom_module_replace_canonical_attr_dict(
        module_, loom_make_named_attr_slice(entries.data(), entries.size()),
        loom_make_named_attr_update_slice(updates.data(), updates.size()),
        &dictionary));
    EXPECT_EQ(dictionary.kind, LOOM_ATTR_DICT);
    EXPECT_EQ(dictionary.count, 0u);
    EXPECT_EQ(module_->arena.used_allocation_size, before_removal);
  }
}

TEST_F(ModuleDictionaryTest, CapacityAccountsForInsertsAndMissingRemovals) {
  auto entries = Entries(UINT16_MAX);
  loom_string_id_t first_key = LOOM_STRING_ID_INVALID;
  IREE_ASSERT_OK(
      loom_module_intern_string(module_, IREE_SV("first"), &first_key));
  const auto base = loom_make_named_attr_slice(entries.data(), entries.size());
  loom_attribute_t dictionary = {};
  const loom_named_attr_update_t missing_removal =
      loom_named_attr_remove(first_key);
  IREE_ASSERT_OK(loom_module_replace_canonical_attr_dict(
      module_, base, loom_make_named_attr_update_slice(&missing_removal, 1),
      &dictionary));
  EXPECT_EQ(dictionary.count, UINT16_MAX);

  const loom_named_attr_update_t updates[] = {
      loom_named_attr_replace(first_key, loom_attr_i64(-1)),
      loom_named_attr_remove(entries.back().name_id),
  };
  IREE_ASSERT_OK(loom_module_replace_canonical_attr_dict(
      module_, base, loom_make_named_attr_update_slice(updates, 2),
      &dictionary));
  ASSERT_EQ(dictionary.count, UINT16_MAX);
  EXPECT_EQ(dictionary.dict_entries[0].name_id, first_key);
  EXPECT_EQ(dictionary.dict_entries[0].value.i64, -1);
  EXPECT_EQ(dictionary.dict_entries[UINT16_MAX - 1].name_id,
            entries[UINT16_MAX - 2].name_id);

  const auto used_before = module_->arena.used_allocation_size;
  IREE_EXPECT_STATUS_IS(
      IREE_STATUS_RESOURCE_EXHAUSTED,
      loom_module_replace_canonical_attr_dict(
          module_, base, loom_make_named_attr_update_slice(updates, 1),
          &dictionary));
  EXPECT_TRUE(loom_attr_is_absent(dictionary));
  EXPECT_EQ(module_->arena.used_allocation_size, used_before);
}

TEST_F(ModuleDictionaryTest, MixedUnorderedUpdatesMatchIndependentMap) {
  auto entries = Entries(257);
  uint32_t random = 0x91441u;
  for (int iteration = 0; iteration < 16; ++iteration) {
    std::vector<loom_named_attr_t> base;
    std::vector<loom_named_attr_update_t> updates;
    std::map<uint32_t, int64_t> expected;
    for (uint32_t i = 0; i < entries.size(); ++i) {
      if (i % 3 != 0) {
        base.push_back(entries[i]);
        expected[i] = i;
      }
      random = random * 1664525u + 1013904223u;
      switch ((random >> 16) & 3u) {
        case 0:
          break;
        case 1:
          updates.push_back(loom_named_attr_remove(entries[i].name_id));
          expected.erase(i);
          break;
        default:
          updates.push_back(loom_named_attr_replace(
              entries[i].name_id, loom_attr_i64(-int64_t(i))));
          expected[i] = -int64_t(i);
          break;
      }
    }
    for (size_t i = updates.size(); i > 1; --i) {
      random = random * 1664525u + 1013904223u;
      std::swap(updates[i - 1], updates[random % i]);
    }
    loom_attribute_t dictionary = {};
    IREE_ASSERT_OK(loom_module_replace_canonical_attr_dict(
        module_, loom_make_named_attr_slice(base.data(), base.size()),
        loom_make_named_attr_update_slice(updates.data(), updates.size()),
        &dictionary));
    ASSERT_EQ(dictionary.count, expected.size());
    size_t index = 0;
    for (const auto& [ordinal, value] : expected) {
      EXPECT_EQ(dictionary.dict_entries[index].name_id,
                entries[ordinal].name_id);
      EXPECT_EQ(dictionary.dict_entries[index].value.i64, value);
      ++index;
    }
    IREE_ASSERT_OK(loom_module_verify_canonical_attr_dict(module_, dictionary));
  }
}

TEST_F(ModuleDictionaryTest, UpdatePayloadsAreCopiedOnceAndBasePayloadsReused) {
  auto entries = Entries(66);
  int64_t payload[] = {3, 5, 8};
  for (auto& entry : entries) {
    entry.value = loom_attr_i64_array(payload, 3);
  }
  loom_attribute_t base = {};
  IREE_ASSERT_OK(loom_module_make_canonical_attr_dict(
      module_, loom_make_named_attr_slice(entries.data(), entries.size()),
      &base));
  loom_attribute_t unchanged = {};
  IREE_ASSERT_OK(loom_module_replace_canonical_attr_dict(
      module_, loom_make_named_attr_slice(base.dict_entries, base.count),
      loom_make_named_attr_update_slice(nullptr, 0), &unchanged));
  EXPECT_NE(unchanged.dict_entries, base.dict_entries);
  ASSERT_EQ(unchanged.count, base.count);
  for (uint16_t i = 0; i < base.count; ++i) {
    EXPECT_EQ(unchanged.dict_entries[i].value.i64_array,
              base.dict_entries[i].value.i64_array);
  }
  std::vector<loom_named_attr_update_t> updates;
  for (size_t i = 0; i < 65; ++i) {
    updates.push_back(loom_named_attr_replace(entries[i].name_id,
                                              loom_attr_i64_array(payload, 3)));
  }
  std::reverse(updates.begin(), updates.end());
  const auto used_before = module_->arena.used_allocation_size;
  loom_attribute_t dictionary = {};
  IREE_ASSERT_OK(loom_module_replace_canonical_attr_dict(
      module_, loom_make_named_attr_slice(base.dict_entries, base.count),
      loom_make_named_attr_update_slice(updates.data(), updates.size()),
      &dictionary));
  EXPECT_EQ(
      module_->arena.used_allocation_size - used_before,
      entries.size() * sizeof(loom_named_attr_t) +
          updates.size() * iree_host_align(sizeof(payload), iree_max_align_t));
  payload[0] = 99;
  for (size_t i = 0; i < dictionary.count; ++i) {
    const auto* result = dictionary.dict_entries[i].value.i64_array;
    EXPECT_EQ(result[0], 3);
    EXPECT_EQ(result[1], 5);
    EXPECT_EQ(result[2], 8);
    if (i == 65) {
      EXPECT_EQ(result, base.dict_entries[i].value.i64_array);
    } else {
      EXPECT_NE(result, base.dict_entries[i].value.i64_array);
    }
  }
}

TEST_F(ModuleDictionaryTest, InvalidUpdatesFailBeforePublishingTheResult) {
  auto entries = Entries(65);
  std::vector<loom_named_attr_update_t> updates;
  for (const auto& entry : entries) {
    updates.push_back(loom_named_attr_replace(entry.name_id, entry.value));
  }
  std::reverse(updates.begin(), updates.end());
  const auto base = loom_make_named_attr_slice(entries.data(), entries.size());
  const auto valid_update = updates.back();
  for (const auto& invalid : {
           loom_named_attr_remove(updates.front().name_id),
           loom_named_attr_remove(LOOM_STRING_ID_INVALID),
           loom_named_attr_replace(valid_update.name_id, loom_attr_absent()),
       }) {
    updates.back() = invalid;
    loom_attribute_t dictionary = loom_attr_i64(42);
    IREE_EXPECT_STATUS_IS(
        IREE_STATUS_INVALID_ARGUMENT,
        loom_module_replace_canonical_attr_dict(
            module_, base,
            loom_make_named_attr_update_slice(updates.data(), updates.size()),
            &dictionary));
    EXPECT_TRUE(loom_attr_is_absent(dictionary));
  }
}

}  // namespace
}  // namespace loom
