// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/ops/format.h"

//===----------------------------------------------------------------------===//
// Keyword B-string table
//===----------------------------------------------------------------------===//

// Generated from KEYWORD_MAP in loom.gen.assembly.tokens.
static const loom_bstring_t loom_keyword_bstrings[LOOM_KW_COUNT_] = {
#include "loom/ops/keyword_table.inc"
};

loom_bstring_t loom_keyword_bstring(loom_keyword_id_t keyword_id) {
  if (keyword_id >= LOOM_KW_COUNT_) {
    return NULL;
  }
  return loom_keyword_bstrings[keyword_id];
}

const loom_op_assembly_format_t* loom_op_assembly_format_lookup_name(
    const loom_op_assembly_format_table_t* table, iree_string_view_t name) {
  if (!table) {
    return NULL;
  }
  uint16_t first = 0;
  uint16_t last = table->entry_count;
  while (first < last) {
    uint16_t middle = first + (last - first) / 2;
    const loom_op_assembly_format_t* entry = &table->entries[middle];
    int order = iree_string_view_compare(name, loom_bstring_view(entry->name));
    if (order == 0) {
      return entry;
    }
    if (order < 0) {
      last = middle;
    } else {
      first = middle + 1;
    }
  }
  return NULL;
}

const loom_op_assembly_format_t* loom_op_assembly_format_lookup_kind(
    const loom_op_assembly_format_table_t* table, loom_op_kind_t kind) {
  if (!table || loom_op_dialect_id(kind) != table->dialect_id) {
    return NULL;
  }
  uint8_t index = table->op_indices[loom_op_dialect_index(kind)];
  return index == UINT8_MAX ? NULL : &table->entries[index];
}
