// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Borrowed source resolution for diagnostics and source-aware compilation.

#ifndef LOOM_ERROR_SOURCE_H_
#define LOOM_ERROR_SOURCE_H_

#include "iree/base/api.h"
#include "loom/error/diagnostic.h"
#include "loom/ir/ir.h"

#ifdef __cplusplus
extern "C" {
#endif

// Resolves a module-local location to a borrowed source range. Returns false
// when the location cannot be resolved; |out_range| is only valid on success.
// The resolver owner keeps the returned filename and source bytes alive for
// the duration of their use. Consumers retaining source data must copy it.
typedef bool (*loom_source_resolver_fn_t)(void* user_data,
                                          const loom_module_t* module,
                                          loom_location_id_t location,
                                          loom_source_range_t* out_range);

typedef struct loom_source_resolver_t {
  // Optional callback. NULL indicates that source resolution is unavailable.
  loom_source_resolver_fn_t fn;
  // Borrowed callback context, live through the final resolution call.
  void* user_data;
} loom_source_resolver_t;

// Resolves |location| when a callback is available. On success the returned
// range and its strings borrow the resolver owner's storage.
static inline bool loom_source_resolve(loom_source_resolver_t resolver,
                                       const loom_module_t* module,
                                       loom_location_id_t location,
                                       loom_source_range_t* out_range) {
  if (resolver.fn) {
    return resolver.fn(resolver.user_data, module, location, out_range);
  }
  return false;
}

// Source bytes associated with a source identity in the module being resolved.
typedef struct loom_source_entry_t {
  // Module-local source identity, or LOOM_SOURCE_ID_INVALID for an empty entry.
  loom_source_id_t source_id;
  // Borrowed original source bytes.
  iree_string_view_t source;
  // Borrowed source filename.
  iree_string_view_t filename;
} loom_source_entry_t;

// Borrowed source entries for loom_source_table_resolve. Entries need not be
// dense or ordered by source ID. Linking projects IDs into the target module
// before its locations are resolved against this table.
typedef struct loom_source_table_resolver_t {
  // Borrowed entries and their strings, live through the final resolution use.
  const loom_source_entry_t* entries;
  // Number of entries, including any empty entries.
  iree_host_size_t count;
} loom_source_table_resolver_t;

// Resolves file locations against a loom_source_table_resolver_t passed as
// |user_data|. Unknown, non-file, and missing source locations return false.
bool loom_source_table_resolve(void* user_data, const loom_module_t* module,
                               loom_location_id_t location,
                               loom_source_range_t* out_range);

// Computes the byte offset of a one-based line and Unicode code-point column.
// Line zero resolves to offset zero. A column at or before one resolves to the
// line's start; a column past the line resolves to its end. A line past the
// source resolves to source.size. This matches Loom text tokenizer coordinates.
iree_host_size_t loom_source_byte_offset(iree_string_view_t source,
                                         uint32_t line, uint32_t column);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_ERROR_SOURCE_H_
