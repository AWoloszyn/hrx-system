// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/error/source.h"

#include "iree/base/internal/unicode.h"
#include "loom/ir/module.h"

iree_host_size_t loom_source_byte_offset(iree_string_view_t source,
                                         uint32_t line, uint32_t column) {
  if (line == 0) {
    return 0;
  }
  // Scan newlines to find the byte offset of the start of |line|.
  uint32_t current_line = 1;
  iree_host_size_t offset = 0;
  while (current_line < line && offset < source.size) {
    if (source.data[offset] == '\n') {
      ++current_line;
    }
    ++offset;
  }
  if (current_line < line) {
    return source.size;
  }
  // Walk UTF-8 codepoints to reach the target column (1-based).
  // Column 1 means "start of line" = offset stays where it is.
  uint32_t current_column = 1;
  while (current_column < column && offset < source.size &&
         source.data[offset] != '\n') {
    iree_unicode_utf8_decode(source, &offset);
    ++current_column;
  }
  return offset > source.size ? source.size : offset;
}

bool loom_source_table_resolve(void* user_data, const loom_module_t* module,
                               loom_location_id_t location,
                               loom_source_range_t* out_range) {
  const loom_source_table_resolver_t* table =
      (const loom_source_table_resolver_t*)user_data;
  if (!table || table->count == 0) {
    return false;
  }
  if (location == LOOM_LOCATION_UNKNOWN) {
    return false;
  }

  // Look up the location entry from the module's location table.
  if ((iree_host_size_t)location >= module->locations.count) {
    return false;
  }
  const loom_location_entry_t* entry = &module->locations.entries[location];
  if (entry->kind != LOOM_LOCATION_FILE) {
    return false;
  }

  // Find the matching source buffer by source_id.
  const loom_source_entry_t* source_entry = NULL;
  for (iree_host_size_t i = 0; i < table->count; ++i) {
    if (table->entries[i].source_id == entry->file.source_id) {
      source_entry = &table->entries[i];
      break;
    }
  }
  if (!source_entry) {
    return false;
  }

  // Compute byte offsets from line/column into the source buffer.
  iree_host_size_t start_offset = loom_source_byte_offset(
      source_entry->source, entry->file.start_line, entry->file.start_col);
  iree_host_size_t end_offset = loom_source_byte_offset(
      source_entry->source, entry->file.end_line, entry->file.end_col);

  *out_range = (loom_source_range_t){
      .provenance = LOOM_SOURCE_PROVENANCE_EXACT_SOURCE,
      .filename = source_entry->filename,
      .source = source_entry->source,
      .start = start_offset,
      .end = end_offset,
      .start_line = entry->file.start_line,
      .start_column = entry->file.start_col,
      .end_line = entry->file.end_line,
      .end_column = entry->file.end_col,
  };
  return true;
}
