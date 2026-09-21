// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_IR_TYPE_TABLE_H_
#define LOOM_IR_TYPE_TABLE_H_

#include "loom/ir/types.h"
#include "loom/util/segmented_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

// Canonical set of SSA dependencies in a type. Zero is the empty set.
typedef uint32_t loom_type_dependency_id_t;

// Number of canonical types and parallel facts in each stable segment.
#define LOOM_TYPE_SEGMENT_CAPACITY 32u

// Shift mapping a type ID to its segment index.
#define LOOM_TYPE_SEGMENT_SHIFT 5u

// Mask mapping a type ID to its row within a segment.
#define LOOM_TYPE_SEGMENT_MASK (LOOM_TYPE_SEGMENT_CAPACITY - 1u)

static_assert((1u << LOOM_TYPE_SEGMENT_SHIFT) == LOOM_TYPE_SEGMENT_CAPACITY,
              "type segment capacity must match its index shift");
static_assert((uint64_t)LOOM_TYPE_SEGMENT_CAPACITY *
                      LOOM_SEGMENTED_STORAGE_MAX_SEGMENT_COUNT >=
                  (uint64_t)LOOM_TYPE_ID_INVALID,
              "type storage must cover the full type ID domain");

// Canonical rows and facts with identical lifetime and cardinality. Parallel
// arrays keep type-only access dense without separate allocation or growth.
typedef struct loom_type_segment_t {
  // Immutable module-owned types.
  loom_type_t entries[LOOM_TYPE_SEGMENT_CAPACITY];
  // Structural hashes for the same canonical IDs.
  uint32_t hashes[LOOM_TYPE_SEGMENT_CAPACITY];
  // Canonical SSA dependency sets, including forward value IDs.
  loom_type_dependency_id_t dependencies[LOOM_TYPE_SEGMENT_CAPACITY];
} loom_type_segment_t;

static_assert(sizeof(loom_type_segment_t) == 1024,
              "type rows and facts must retain the 1 KiB segment layout");

// Append-only canonical type storage owned by the module arena. Rows and
// payloads remain immutable and stable until module destruction. Only the
// initialized prefix named by count may be read; unused chunk rows are inert.
typedef struct loom_type_table_t {
  // Number of published canonical types and parallel facts.
  iree_host_size_t count;
  // Lazily allocated, stable row chunks and their pointer directory.
  loom_segmented_storage_t segments;
} loom_type_table_t;

// Returns the number of allocated rows, including the unpublished tail.
static inline iree_host_size_t loom_type_table_capacity(
    const loom_type_table_t* table) {
  return (iree_host_size_t)table->segments.segment_count *
         LOOM_TYPE_SEGMENT_CAPACITY;
}

// Returns the segment containing a published canonical type ID.
static inline const loom_type_segment_t* loom_type_table_segment_for_id(
    const loom_type_table_t* table, loom_type_id_t type_id) {
  IREE_ASSERT((iree_host_size_t)type_id < table->count);
  return (const loom_type_segment_t*)loom_segmented_storage_const_segment(
      &table->segments, type_id >> LOOM_TYPE_SEGMENT_SHIFT);
}

// Returns the stable, immutable row for a published canonical type ID.
static inline const loom_type_t* loom_type_table_entry(
    const loom_type_table_t* table, loom_type_id_t type_id) {
  const loom_type_segment_t* segment =
      loom_type_table_segment_for_id(table, type_id);
  return &segment->entries[type_id & LOOM_TYPE_SEGMENT_MASK];
}

// Returns a published canonical type by value.
static inline loom_type_t loom_type_table_get(const loom_type_table_t* table,
                                              loom_type_id_t type_id) {
  return *loom_type_table_entry(table, type_id);
}

// Returns the retained structural hash for a published canonical type ID.
static inline uint32_t loom_type_table_hash(const loom_type_table_t* table,
                                            loom_type_id_t type_id) {
  const loom_type_segment_t* segment =
      loom_type_table_segment_for_id(table, type_id);
  return segment->hashes[type_id & LOOM_TYPE_SEGMENT_MASK];
}

// Returns the retained SSA dependency set for a published canonical type ID.
static inline loom_type_dependency_id_t loom_type_table_dependencies(
    const loom_type_table_t* table, loom_type_id_t type_id) {
  const loom_type_segment_t* segment =
      loom_type_table_segment_for_id(table, type_id);
  return segment->dependencies[type_id & LOOM_TYPE_SEGMENT_MASK];
}

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_IR_TYPE_TABLE_H_
