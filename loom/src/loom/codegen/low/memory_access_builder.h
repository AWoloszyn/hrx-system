// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// One-pass collection and exact publication of low memory access records.

#ifndef LOOM_CODEGEN_LOW_MEMORY_ACCESS_BUILDER_H_
#define LOOM_CODEGEN_LOW_MEMORY_ACCESS_BUILDER_H_

#include "iree/base/internal/arena.h"
#include "loom/codegen/low/memory_access.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loom_low_memory_access_chunk_t loom_low_memory_access_chunk_t;

// Zero-initialized collector for records appended in function order. Scratch
// chunks hold records and present interval payloads until exact publication
// into the retained arena. Neither arena is owned or reset by the builder. An
// allocation failure terminates construction.
typedef struct loom_low_memory_access_builder_t {
  // Scratch chunks in append order, or NULL endpoints while empty.
  struct {
    // First chunk copied during publication.
    loom_low_memory_access_chunk_t* first;
    // Last chunk receiving appended records.
    loom_low_memory_access_chunk_t* last;
  } chunks;
  // Populated interval payloads collected independently of record chunks.
  struct {
    // Current scratch batch; earlier batches remain referenced by records.
    loom_low_byte_interval_t* tail;
    // Number of initialized intervals across all scratch batches.
    iree_host_size_t count;
  } intervals;
  // Number of initialized records across the chunks.
  iree_host_size_t count;
} loom_low_memory_access_builder_t;

// Copies one record and its present interval payload. |record| and its payload
// may be temporary. All appends use the same scratch arena.
iree_status_t loom_low_memory_access_builder_append(
    loom_low_memory_access_builder_t* builder,
    const loom_low_memory_access_record_t* record,
    iree_arena_allocator_t* scratch_arena);

// Publishes exactly sized record and populated interval arrays in |arena|.
// The resulting table survives scratch-arena reset. Empty builders produce
// an empty table without allocating.
iree_status_t loom_low_memory_access_builder_finish(
    const loom_low_memory_access_builder_t* builder,
    const loom_op_t* function_op, iree_arena_allocator_t* arena,
    loom_low_memory_access_table_t* out_table);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_CODEGEN_LOW_MEMORY_ACCESS_BUILDER_H_
