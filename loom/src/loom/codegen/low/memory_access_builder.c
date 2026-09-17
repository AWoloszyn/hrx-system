// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/codegen/low/memory_access_builder.h"

// Small chunks bound unused construction storage without copying earlier
// records when a source operation expands to additional memory packets.
#define LOOM_LOW_MEMORY_ACCESS_CHUNK_CAPACITY 4u

struct loom_low_memory_access_chunk_t {
  // Next chunk in append order, or NULL at the tail.
  loom_low_memory_access_chunk_t* next;
  // Records populated in order; only the final chunk may have an unused tail.
  loom_low_memory_access_record_t rows[LOOM_LOW_MEMORY_ACCESS_CHUNK_CAPACITY];
};

iree_status_t loom_low_memory_access_builder_append(
    loom_low_memory_access_builder_t* builder,
    const loom_low_memory_access_record_t* record,
    iree_arena_allocator_t* scratch_arena) {
  loom_low_memory_access_record_t owned_record = *record;
  if (record->summary.byte_interval != NULL) {
    const iree_host_size_t interval_index =
        builder->intervals.count % LOOM_LOW_MEMORY_ACCESS_CHUNK_CAPACITY;
    if (interval_index == 0) {
      IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
          scratch_arena, LOOM_LOW_MEMORY_ACCESS_CHUNK_CAPACITY,
          sizeof(*builder->intervals.tail), (void**)&builder->intervals.tail));
    }
    builder->intervals.tail[interval_index] = *record->summary.byte_interval;
    owned_record.summary.byte_interval =
        &builder->intervals.tail[interval_index];
    ++builder->intervals.count;
  }

  const iree_host_size_t chunk_index =
      builder->count % LOOM_LOW_MEMORY_ACCESS_CHUNK_CAPACITY;
  if (chunk_index == 0) {
    loom_low_memory_access_chunk_t* chunk = NULL;
    IREE_RETURN_IF_ERROR(
        iree_arena_allocate(scratch_arena, sizeof(*chunk), (void**)&chunk));
    chunk->next = NULL;
    if (builder->chunks.last != NULL) {
      builder->chunks.last->next = chunk;
    } else {
      builder->chunks.first = chunk;
    }
    builder->chunks.last = chunk;
  }
  builder->chunks.last->rows[chunk_index] = owned_record;
  ++builder->count;
  return iree_ok_status();
}

iree_status_t loom_low_memory_access_builder_finish(
    const loom_low_memory_access_builder_t* builder,
    const loom_op_t* function_op, iree_arena_allocator_t* arena,
    loom_low_memory_access_table_t* out_table) {
  *out_table = loom_low_memory_access_table_empty();
  if (builder->count == 0) {
    return iree_ok_status();
  }

  loom_low_memory_access_record_t* records = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, builder->count, sizeof(*records), (void**)&records));
  loom_low_byte_interval_t* intervals = NULL;
  if (builder->intervals.count != 0) {
    IREE_RETURN_IF_ERROR(
        iree_arena_allocate_array(arena, builder->intervals.count,
                                  sizeof(*intervals), (void**)&intervals));
  }
  iree_host_size_t record_index = 0;
  iree_host_size_t interval_index = 0;
  for (const loom_low_memory_access_chunk_t* chunk = builder->chunks.first;
       chunk != NULL; chunk = chunk->next) {
    const iree_host_size_t count = iree_min(
        builder->count - record_index, LOOM_LOW_MEMORY_ACCESS_CHUNK_CAPACITY);
    for (iree_host_size_t i = 0; i < count; ++i) {
      loom_low_memory_access_record_t* record = &records[record_index++];
      *record = chunk->rows[i];
      if (record->summary.byte_interval != NULL) {
        intervals[interval_index] = *record->summary.byte_interval;
        record->summary.byte_interval = &intervals[interval_index++];
      }
    }
  }
  *out_table = (loom_low_memory_access_table_t){
      .function_op = function_op,
      .values = records,
      .count = builder->count,
  };
  return iree_ok_status();
}
