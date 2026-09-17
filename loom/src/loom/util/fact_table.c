// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/util/fact_table.h"

#include <stdint.h>
#include <string.h>

#include "loom/ir/context.h"
#include "loom/ir/module.h"
#include "loom/ops/op_defs.h"
#include "loom/util/fact_cfg.h"

//===----------------------------------------------------------------------===//
// Capacity management
//===----------------------------------------------------------------------===//

struct loom_value_fact_cfg_graph_entry_t {
  // Region whose successor structure is represented by graph.
  const loom_region_t* region;
  // CFG and forwarding components retained for the populated fact scope.
  const loom_value_fact_cfg_region_t* structure;
  // Next entry in the region-address hash collision chain.
  loom_value_fact_cfg_graph_entry_t* next_bucket;
  // Next entry in the complete cache entry list.
  loom_value_fact_cfg_graph_entry_t* next_entry;
};

static iree_status_t loom_value_fact_table_ensure_capacity(
    loom_value_fact_table_t* table, iree_host_size_t capacity) {
  if (capacity <= table->capacity) {
    return iree_ok_status();
  }
  const iree_host_size_t old_capacity = table->capacity;
  iree_host_size_t new_capacity = old_capacity;
  loom_value_facts_t* entries = table->entries;
  IREE_RETURN_IF_ERROR(iree_arena_grow_array(
      table->arena, old_capacity, capacity, sizeof(loom_value_facts_t),
      &new_capacity, (void**)&entries));
  memset(entries + old_capacity, 0,
         (new_capacity - old_capacity) * sizeof(loom_value_facts_t));
  iree_host_size_t old_word_count = (old_capacity + 63) / 64;
  iree_host_size_t word_count = (new_capacity + 63) / 64;
  uint64_t* touched_bits = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      table->arena, word_count, sizeof(*touched_bits), (void**)&touched_bits));
  if (old_word_count) {
    memcpy(touched_bits, table->touched_bits,
           old_word_count * sizeof(*touched_bits));
  }
  memset(touched_bits + old_word_count, 0,
         (word_count - old_word_count) * sizeof(*touched_bits));
  table->entries = entries;
  table->capacity = new_capacity;
  table->touched_bits = touched_bits;
  return iree_ok_status();
}

static iree_status_t loom_value_fact_table_ensure_uniform_origin_capacity(
    loom_value_fact_table_t* table, iree_host_size_t capacity) {
  if (capacity <= table->uniform_element_origins.capacity) {
    return iree_ok_status();
  }
  const iree_host_size_t old_capacity = table->uniform_element_origins.capacity;
  IREE_RETURN_IF_ERROR(iree_arena_grow_array(
      table->arena, old_capacity, capacity, sizeof(loom_value_id_t),
      &table->uniform_element_origins.capacity,
      (void**)&table->uniform_element_origins.entries));
  for (iree_host_size_t i = old_capacity;
       i < table->uniform_element_origins.capacity; ++i) {
    table->uniform_element_origins.entries[i] = LOOM_VALUE_ID_INVALID;
  }
  return iree_ok_status();
}

static loom_value_fact_static_lane_origin_t
loom_value_fact_static_lane_origin_invalid(void) {
  return (loom_value_fact_static_lane_origin_t){
      .source_value_id = LOOM_VALUE_ID_INVALID,
  };
}

static loom_value_fact_uniform_scale_origin_t
loom_value_fact_uniform_scale_origin_invalid(void) {
  return (loom_value_fact_uniform_scale_origin_t){
      .source_value_id = LOOM_VALUE_ID_INVALID,
      .scale_value_id = LOOM_VALUE_ID_INVALID,
  };
}

static iree_status_t loom_value_fact_table_ensure_static_lane_origin_capacity(
    loom_value_fact_table_t* table, iree_host_size_t capacity) {
  if (capacity <= table->static_lane_origins.capacity) {
    return iree_ok_status();
  }
  const iree_host_size_t old_capacity = table->static_lane_origins.capacity;
  IREE_RETURN_IF_ERROR(
      iree_arena_grow_array(table->arena, old_capacity, capacity,
                            sizeof(loom_value_fact_static_lane_origin_t),
                            &table->static_lane_origins.capacity,
                            (void**)&table->static_lane_origins.entries));
  for (iree_host_size_t i = old_capacity;
       i < table->static_lane_origins.capacity; ++i) {
    table->static_lane_origins.entries[i] =
        loom_value_fact_static_lane_origin_invalid();
  }
  return iree_ok_status();
}

static iree_status_t loom_value_fact_table_ensure_uniform_scale_origin_capacity(
    loom_value_fact_table_t* table, iree_host_size_t capacity) {
  if (capacity <= table->uniform_scale_origins.capacity) {
    return iree_ok_status();
  }
  const iree_host_size_t old_capacity = table->uniform_scale_origins.capacity;
  IREE_RETURN_IF_ERROR(
      iree_arena_grow_array(table->arena, old_capacity, capacity,
                            sizeof(loom_value_fact_uniform_scale_origin_t),
                            &table->uniform_scale_origins.capacity,
                            (void**)&table->uniform_scale_origins.entries));
  for (iree_host_size_t i = old_capacity;
       i < table->uniform_scale_origins.capacity; ++i) {
    table->uniform_scale_origins.entries[i] =
        loom_value_fact_uniform_scale_origin_invalid();
  }
  return iree_ok_status();
}

static iree_status_t
loom_value_fact_table_ensure_contextual_query_origin_capacity(
    loom_value_fact_table_t* table, iree_host_size_t capacity) {
  if (capacity <= table->contextual_query_origins.capacity) {
    return iree_ok_status();
  }
  const iree_host_size_t old_capacity =
      table->contextual_query_origins.capacity;
  IREE_RETURN_IF_ERROR(iree_arena_grow_array(
      table->arena, old_capacity, capacity, sizeof(uint32_t),
      &table->contextual_query_origins.capacity,
      (void**)&table->contextual_query_origins.entries));
  memset(table->contextual_query_origins.entries + old_capacity, 0,
         (table->contextual_query_origins.capacity - old_capacity) *
             sizeof(uint32_t));
  return iree_ok_status();
}

static iree_status_t loom_value_fact_table_allocate_initial_capacity(
    loom_value_fact_table_t* table, iree_host_size_t capacity) {
  return loom_value_fact_table_ensure_capacity(table, capacity);
}

static iree_status_t loom_value_fact_table_append_touched_value(
    loom_value_fact_table_t* table, loom_value_id_t value_id) {
  if (table->touched_count >= table->touched_capacity) {
    IREE_RETURN_IF_ERROR(iree_arena_grow_array(
        table->arena, table->touched_count, table->touched_count + 1,
        sizeof(*table->touched_values), &table->touched_capacity,
        (void**)&table->touched_values));
  }
  table->touched_values[table->touched_count++] = value_id;
  return iree_ok_status();
}

static iree_status_t loom_value_fact_table_append_touched_uniform_origin(
    loom_value_fact_table_t* table, loom_value_id_t value_id) {
  if (table->uniform_element_origins.touched_count >=
      table->uniform_element_origins.touched_capacity) {
    IREE_RETURN_IF_ERROR(iree_arena_grow_array(
        table->arena, table->uniform_element_origins.touched_count,
        table->uniform_element_origins.touched_count + 1,
        sizeof(*table->uniform_element_origins.touched_values),
        &table->uniform_element_origins.touched_capacity,
        (void**)&table->uniform_element_origins.touched_values));
  }
  table->uniform_element_origins
      .touched_values[table->uniform_element_origins.touched_count++] =
      value_id;
  return iree_ok_status();
}

static iree_status_t loom_value_fact_table_append_touched_static_lane_origin(
    loom_value_fact_table_t* table, loom_value_id_t value_id) {
  if (table->static_lane_origins.touched_count >=
      table->static_lane_origins.touched_capacity) {
    IREE_RETURN_IF_ERROR(iree_arena_grow_array(
        table->arena, table->static_lane_origins.touched_count,
        table->static_lane_origins.touched_count + 1,
        sizeof(*table->static_lane_origins.touched_values),
        &table->static_lane_origins.touched_capacity,
        (void**)&table->static_lane_origins.touched_values));
  }
  table->static_lane_origins
      .touched_values[table->static_lane_origins.touched_count++] = value_id;
  return iree_ok_status();
}

static iree_status_t loom_value_fact_table_append_touched_uniform_scale_origin(
    loom_value_fact_table_t* table, loom_value_id_t value_id) {
  if (table->uniform_scale_origins.touched_count >=
      table->uniform_scale_origins.touched_capacity) {
    IREE_RETURN_IF_ERROR(iree_arena_grow_array(
        table->arena, table->uniform_scale_origins.touched_count,
        table->uniform_scale_origins.touched_count + 1,
        sizeof(*table->uniform_scale_origins.touched_values),
        &table->uniform_scale_origins.touched_capacity,
        (void**)&table->uniform_scale_origins.touched_values));
  }
  table->uniform_scale_origins
      .touched_values[table->uniform_scale_origins.touched_count++] = value_id;
  return iree_ok_status();
}

static iree_status_t
loom_value_fact_table_append_touched_contextual_query_origin(
    loom_value_fact_table_t* table, loom_value_id_t value_id) {
  if (table->contextual_query_origins.touched_count >=
      table->contextual_query_origins.touched_capacity) {
    IREE_RETURN_IF_ERROR(iree_arena_grow_array(
        table->arena, table->contextual_query_origins.touched_count,
        table->contextual_query_origins.touched_count + 1,
        sizeof(*table->contextual_query_origins.touched_values),
        &table->contextual_query_origins.touched_capacity,
        (void**)&table->contextual_query_origins.touched_values));
  }
  table->contextual_query_origins
      .touched_values[table->contextual_query_origins.touched_count++] =
      value_id;
  return iree_ok_status();
}

//===----------------------------------------------------------------------===//
// Scratch buffers
//===----------------------------------------------------------------------===//

iree_status_t loom_value_fact_table_facts_scratch(
    loom_value_fact_table_t* table, iree_host_size_t count,
    loom_value_facts_t** out) {
  if (count <= table->scratch.facts.capacity) {
    *out = table->scratch.facts.values;
    return iree_ok_status();
  }
  loom_value_facts_t* new_scratch = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(table->transient_arena, count,
                                                 sizeof(loom_value_facts_t),
                                                 (void**)&new_scratch));
  table->scratch.facts.values = new_scratch;
  table->scratch.facts.capacity = count;
  *out = new_scratch;
  return iree_ok_status();
}

iree_status_t loom_value_fact_table_value_id_scratch(
    loom_value_fact_table_t* table, iree_host_size_t count,
    loom_value_id_t** out) {
  if (count <= table->scratch.value_ids.capacity) {
    *out = table->scratch.value_ids.values;
    return iree_ok_status();
  }
  loom_value_id_t* new_scratch = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(table->transient_arena, count,
                                                 sizeof(loom_value_id_t),
                                                 (void**)&new_scratch));
  table->scratch.value_ids.values = new_scratch;
  table->scratch.value_ids.capacity = count;
  *out = new_scratch;
  return iree_ok_status();
}

//===----------------------------------------------------------------------===//
// Public API
//===----------------------------------------------------------------------===//

iree_status_t loom_value_fact_table_initialize(
    loom_value_fact_table_t* table, iree_arena_allocator_t* arena,
    iree_host_size_t initial_capacity) {
  return loom_value_fact_table_initialize_with_arenas(table, arena, arena,
                                                      initial_capacity);
}

iree_status_t loom_value_fact_table_initialize_with_arenas(
    loom_value_fact_table_t* table, iree_arena_allocator_t* arena,
    iree_arena_allocator_t* transient_arena,
    iree_host_size_t initial_capacity) {
  memset(table, 0, sizeof(*table));
  table->arena = arena;
  table->transient_arena = transient_arena;
  table->context.table = table;
  return loom_value_fact_table_allocate_initial_capacity(table,
                                                         initial_capacity);
}

void loom_value_fact_table_clear_scope(loom_value_fact_table_t* table) {
  for (iree_host_size_t i = 0; i < table->touched_count; ++i) {
    loom_value_id_t value_id = table->touched_values[i];
    table->entries[value_id] = (loom_value_facts_t){0};
    table->touched_bits[value_id / 64] &= ~(UINT64_C(1) << (value_id % 64));
  }
  for (iree_host_size_t i = 0; i < table->uniform_element_origins.touched_count;
       ++i) {
    table->uniform_element_origins
        .entries[table->uniform_element_origins.touched_values[i]] =
        LOOM_VALUE_ID_INVALID;
  }
  for (iree_host_size_t i = 0; i < table->static_lane_origins.touched_count;
       ++i) {
    table->static_lane_origins
        .entries[table->static_lane_origins.touched_values[i]] =
        loom_value_fact_static_lane_origin_invalid();
  }
  for (iree_host_size_t i = 0; i < table->uniform_scale_origins.touched_count;
       ++i) {
    table->uniform_scale_origins
        .entries[table->uniform_scale_origins.touched_values[i]] =
        loom_value_fact_uniform_scale_origin_invalid();
  }
  for (iree_host_size_t i = 0;
       i < table->contextual_query_origins.touched_count; ++i) {
    table->contextual_query_origins
        .entries[table->contextual_query_origins.touched_values[i]] = 0;
  }
  table->touched_count = 0;
  table->count = 0;
  table->extensions.entries = NULL;
  table->extensions.capacity = 0;
  table->extensions.count = 0;
  table->extensions.buckets = NULL;
  table->extensions.bucket_count = 0;
  table->cfg_graphs.buckets = NULL;
  table->cfg_graphs.bucket_count = 0;
  table->cfg_graphs.count = 0;
  table->cfg_graphs.entries = NULL;
  table->uniform_element_origins.touched_count = 0;
  table->static_lane_origins.touched_count = 0;
  table->uniform_scale_origins.touched_count = 0;
  table->contextual_query_origins.touched_count = 0;
  table->contextual_query_origins.origin_count = 0;
  table->scratch.facts.values = NULL;
  table->scratch.facts.capacity = 0;
  table->scratch.value_ids.values = NULL;
  table->scratch.value_ids.capacity = 0;
  table->scratch.alias_ordinals.values = NULL;
  table->scratch.alias_ordinals.capacity = 0;
  table->context.table = table;
  table->context.function = (loom_func_like_t){0};
  table->context.reference_origin = (loom_value_fact_reference_origin_t){0};
  table->context.target_facts = NULL;
}

static iree_host_size_t loom_value_fact_table_cfg_region_hash(
    const loom_region_t* region) {
  uintptr_t bits = (uintptr_t)region;
  bits ^= bits >> 17;
  bits *= (uintptr_t)0xed5ad4bbU;
  bits ^= bits >> 11;
  return (iree_host_size_t)bits;
}

static iree_status_t loom_value_fact_table_rehash_cfg_graphs(
    loom_value_fact_table_t* table, iree_host_size_t new_bucket_count) {
  loom_value_fact_cfg_graph_entry_t** new_buckets = NULL;
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate_array(table->transient_arena, new_bucket_count,
                                sizeof(*new_buckets), (void**)&new_buckets));
  memset(new_buckets, 0, new_bucket_count * sizeof(*new_buckets));
  for (loom_value_fact_cfg_graph_entry_t* entry = table->cfg_graphs.entries;
       entry; entry = entry->next_entry) {
    const iree_host_size_t bucket_index =
        loom_value_fact_table_cfg_region_hash(entry->region) &
        (new_bucket_count - 1);
    entry->next_bucket = new_buckets[bucket_index];
    new_buckets[bucket_index] = entry;
  }
  table->cfg_graphs.buckets = new_buckets;
  table->cfg_graphs.bucket_count = new_bucket_count;
  return iree_ok_status();
}

static iree_status_t loom_value_fact_table_ensure_cfg_graph_buckets(
    loom_value_fact_table_t* table, iree_host_size_t minimum_count) {
  iree_host_size_t bucket_count = table->cfg_graphs.bucket_count;
  if (bucket_count == 0) {
    bucket_count = 8;
  }
  while (minimum_count > bucket_count - bucket_count / 4) {
    if (bucket_count > SIZE_MAX / 2) {
      return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                              "CFG graph cache capacity overflow");
    }
    bucket_count *= 2;
  }
  if (bucket_count == table->cfg_graphs.bucket_count) {
    return iree_ok_status();
  }
  return loom_value_fact_table_rehash_cfg_graphs(table, bucket_count);
}

static loom_value_fact_cfg_graph_entry_t*
loom_value_fact_table_lookup_cfg_entry(const loom_value_fact_table_t* table,
                                       const loom_region_t* region) {
  IREE_ASSERT_ARGUMENT(table);
  IREE_ASSERT_ARGUMENT(region);
  if (table->cfg_graphs.bucket_count == 0) {
    return NULL;
  }
  const iree_host_size_t bucket_index =
      loom_value_fact_table_cfg_region_hash(region) &
      (table->cfg_graphs.bucket_count - 1);
  for (loom_value_fact_cfg_graph_entry_t* entry =
           table->cfg_graphs.buckets[bucket_index];
       entry; entry = entry->next_bucket) {
    if (entry->region == region) {
      return entry;
    }
  }
  return NULL;
}

const loom_value_fact_cfg_region_t* loom_value_fact_table_lookup_cfg_region(
    const loom_value_fact_table_t* table, const loom_region_t* region) {
  const loom_value_fact_cfg_graph_entry_t* entry =
      loom_value_fact_table_lookup_cfg_entry(table, region);
  return entry ? entry->structure : NULL;
}

const loom_cfg_graph_t* loom_value_fact_table_lookup_cfg_graph(
    const loom_value_fact_table_t* table, const loom_region_t* region) {
  const loom_value_fact_cfg_graph_entry_t* entry =
      loom_value_fact_table_lookup_cfg_entry(table, region);
  return entry && entry->structure ? &entry->structure->graph : NULL;
}

iree_status_t loom_value_fact_table_set_cfg_region(
    loom_value_fact_table_t* table, const loom_region_t* region,
    const loom_value_fact_cfg_region_t* structure) {
  loom_value_fact_cfg_graph_entry_t* entry =
      loom_value_fact_table_lookup_cfg_entry(table, region);
  if (!entry) {
    const iree_host_size_t new_count = table->cfg_graphs.count + 1;
    IREE_RETURN_IF_ERROR(
        loom_value_fact_table_ensure_cfg_graph_buckets(table, new_count));
    IREE_RETURN_IF_ERROR(iree_arena_allocate(table->transient_arena,
                                             sizeof(*entry), (void**)&entry));
    memset(entry, 0, sizeof(*entry));
    entry->region = region;
    const iree_host_size_t bucket_index =
        loom_value_fact_table_cfg_region_hash(region) &
        (table->cfg_graphs.bucket_count - 1);
    entry->next_bucket = table->cfg_graphs.buckets[bucket_index];
    table->cfg_graphs.buckets[bucket_index] = entry;
    entry->next_entry = table->cfg_graphs.entries;
    table->cfg_graphs.entries = entry;
    table->cfg_graphs.count = new_count;
  }
  entry->structure = structure;
  return iree_ok_status();
}

void loom_value_fact_table_forget_cfg_region(loom_value_fact_table_t* table,
                                             const loom_region_t* region) {
  loom_value_fact_cfg_graph_entry_t* entry =
      loom_value_fact_table_lookup_cfg_entry(table, region);
  if (entry) {
    entry->structure = NULL;
  }
}

iree_status_t loom_value_fact_table_get_or_build_cfg_region(
    loom_value_fact_table_t* table, const loom_module_t* module,
    const loom_region_t* region,
    const loom_value_fact_cfg_region_t** out_region) {
  loom_value_fact_cfg_graph_entry_t* entry =
      loom_value_fact_table_lookup_cfg_entry(table, region);
  *out_region = entry ? entry->structure : NULL;
  if (*out_region) {
    return iree_ok_status();
  }

  loom_value_fact_cfg_region_t* structure = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate(
      table->transient_arena, sizeof(*structure), (void**)&structure));
  IREE_RETURN_IF_ERROR(loom_value_fact_cfg_region_initialize(
      module, region, table->transient_arena, structure));
  IREE_RETURN_IF_ERROR(
      loom_value_fact_table_set_cfg_region(table, region, structure));
  *out_region = structure;
  return iree_ok_status();
}

iree_status_t loom_value_fact_table_define(loom_value_fact_table_t* table,
                                           loom_value_id_t value_id,
                                           loom_value_facts_t facts) {
  IREE_ASSERT_NE(facts.known_divisor, 0);
  IREE_RETURN_IF_ERROR(loom_value_fact_table_ensure_capacity(
      table, (iree_host_size_t)value_id + 1));
  uint64_t touched_bit = UINT64_C(1) << (value_id % 64);
  if (table->entries[value_id].known_divisor == 0 &&
      !(table->touched_bits[value_id / 64] & touched_bit)) {
    IREE_RETURN_IF_ERROR(
        loom_value_fact_table_append_touched_value(table, value_id));
    table->touched_bits[value_id / 64] |= touched_bit;
  }
  table->entries[value_id] = facts;
  if ((iree_host_size_t)value_id + 1 > table->count) {
    table->count = (iree_host_size_t)value_id + 1;
  }
  return iree_ok_status();
}

void loom_value_fact_table_undefine(loom_value_fact_table_t* table,
                                    loom_value_id_t value_id) {
  if (value_id < table->capacity) {
    table->entries[value_id] = (loom_value_facts_t){0};
  }
}

static bool loom_value_fact_table_lookup_uniform_element_origin(
    const loom_value_fact_table_t* table, loom_value_id_t value_id,
    loom_value_id_t* out_scalar_value_id) {
  if (out_scalar_value_id) {
    *out_scalar_value_id = LOOM_VALUE_ID_INVALID;
  }
  if (value_id >= table->uniform_element_origins.capacity ||
      table->uniform_element_origins.entries == NULL) {
    return false;
  }
  const loom_value_id_t scalar_value_id =
      table->uniform_element_origins.entries[value_id];
  if (scalar_value_id == LOOM_VALUE_ID_INVALID) {
    return false;
  }
  if (out_scalar_value_id) {
    *out_scalar_value_id = scalar_value_id;
  }
  return true;
}

iree_status_t loom_value_fact_table_define_uniform_element_origin(
    loom_value_fact_table_t* table, loom_value_id_t value_id,
    loom_value_id_t scalar_value_id) {
  if (value_id == LOOM_VALUE_ID_INVALID ||
      scalar_value_id == LOOM_VALUE_ID_INVALID) {
    return iree_ok_status();
  }
  IREE_RETURN_IF_ERROR(loom_value_fact_table_ensure_uniform_origin_capacity(
      table, (iree_host_size_t)value_id + 1));
  if (table->uniform_element_origins.entries[value_id] ==
      LOOM_VALUE_ID_INVALID) {
    IREE_RETURN_IF_ERROR(
        loom_value_fact_table_append_touched_uniform_origin(table, value_id));
  }
  table->uniform_element_origins.entries[value_id] = scalar_value_id;
  return iree_ok_status();
}

bool loom_value_fact_table_query_uniform_element_origin(
    const loom_value_fact_table_t* table, const loom_module_t* module,
    loom_value_id_t value_id, loom_value_id_t* out_scalar_value_id) {
  if (out_scalar_value_id) {
    *out_scalar_value_id = LOOM_VALUE_ID_INVALID;
  }
  if (table == NULL || module == NULL || value_id >= module->values.count) {
    return false;
  }
  const loom_type_t value_type = loom_module_value_type(module, value_id);
  if (!loom_type_is_shaped(value_type)) {
    return false;
  }

  loom_value_id_t scalar_value_id = LOOM_VALUE_ID_INVALID;
  if (!loom_value_fact_table_lookup_uniform_element_origin(table, value_id,
                                                           &scalar_value_id) ||
      scalar_value_id >= module->values.count) {
    return false;
  }
  const loom_type_t scalar_type =
      loom_module_value_type(module, scalar_value_id);
  if (!loom_type_is_scalar(scalar_type) ||
      loom_type_element_type(scalar_type) !=
          loom_type_element_type(value_type)) {
    return false;
  }
  if (out_scalar_value_id) {
    *out_scalar_value_id = scalar_value_id;
  }
  return true;
}

static bool loom_value_fact_table_lookup_static_lane_origin(
    const loom_value_fact_table_t* table, loom_value_id_t value_id,
    loom_value_fact_static_lane_origin_t* out_origin) {
  if (out_origin) {
    *out_origin = loom_value_fact_static_lane_origin_invalid();
  }
  if (value_id >= table->static_lane_origins.capacity ||
      table->static_lane_origins.entries == NULL) {
    return false;
  }
  const loom_value_fact_static_lane_origin_t origin =
      table->static_lane_origins.entries[value_id];
  if (origin.source_value_id == LOOM_VALUE_ID_INVALID) {
    return false;
  }
  if (out_origin) {
    *out_origin = origin;
  }
  return true;
}

iree_status_t loom_value_fact_table_define_static_lane_origin(
    loom_value_fact_table_t* table, loom_value_id_t value_id,
    loom_value_fact_static_lane_origin_t origin) {
  if (value_id == LOOM_VALUE_ID_INVALID ||
      origin.source_value_id == LOOM_VALUE_ID_INVALID) {
    return iree_ok_status();
  }
  IREE_ASSERT_NE(origin.source_lane_stride, 0u);
  IREE_RETURN_IF_ERROR(loom_value_fact_table_ensure_static_lane_origin_capacity(
      table, (iree_host_size_t)value_id + 1));
  if (table->static_lane_origins.entries[value_id].source_value_id ==
      LOOM_VALUE_ID_INVALID) {
    IREE_RETURN_IF_ERROR(
        loom_value_fact_table_append_touched_static_lane_origin(table,
                                                                value_id));
  }
  table->static_lane_origins.entries[value_id] = origin;
  return iree_ok_status();
}

bool loom_value_fact_table_query_static_lane_origin(
    const loom_value_fact_table_t* table, const loom_module_t* module,
    loom_value_id_t value_id,
    loom_value_fact_static_lane_origin_t* out_origin) {
  if (out_origin) {
    *out_origin = loom_value_fact_static_lane_origin_invalid();
  }
  if (table == NULL || module == NULL || value_id >= module->values.count) {
    return false;
  }
  loom_value_fact_static_lane_origin_t origin =
      loom_value_fact_static_lane_origin_invalid();
  if (!loom_value_fact_table_lookup_static_lane_origin(table, value_id,
                                                       &origin) ||
      origin.source_value_id >= module->values.count ||
      origin.source_lane_stride == 0) {
    return false;
  }

  const loom_type_t value_type = loom_module_value_type(module, value_id);
  const loom_type_t source_type =
      loom_module_value_type(module, origin.source_value_id);
  if (!loom_type_is_vector(value_type) || !loom_type_is_vector(source_type)) {
    return false;
  }

  uint64_t value_lane_count = 0;
  uint64_t source_lane_count = 0;
  if (!loom_type_static_element_count(value_type, &value_lane_count) ||
      !loom_type_static_element_count(source_type, &source_lane_count)) {
    return false;
  }
  uint64_t max_source_lane = origin.source_lane_offset;
  if (value_lane_count > 0) {
    const uint64_t lane_delta_count = value_lane_count - 1u;
    const uint64_t stride = origin.source_lane_stride;
    if (lane_delta_count > (UINT64_MAX - max_source_lane) / stride) {
      return false;
    }
    max_source_lane += lane_delta_count * stride;
  }
  if (max_source_lane >= source_lane_count) {
    return false;
  }

  if (out_origin) {
    *out_origin = origin;
  }
  return true;
}

static bool loom_value_fact_table_lookup_uniform_scale_origin(
    const loom_value_fact_table_t* table, loom_value_id_t value_id,
    loom_value_fact_uniform_scale_origin_t* out_origin) {
  if (out_origin) {
    *out_origin = loom_value_fact_uniform_scale_origin_invalid();
  }
  if (value_id >= table->uniform_scale_origins.capacity ||
      table->uniform_scale_origins.entries == NULL) {
    return false;
  }
  const loom_value_fact_uniform_scale_origin_t origin =
      table->uniform_scale_origins.entries[value_id];
  if (origin.source_value_id == LOOM_VALUE_ID_INVALID ||
      origin.scale_value_id == LOOM_VALUE_ID_INVALID) {
    return false;
  }
  if (out_origin) {
    *out_origin = origin;
  }
  return true;
}

iree_status_t loom_value_fact_table_define_uniform_scale_origin(
    loom_value_fact_table_t* table, loom_value_id_t value_id,
    loom_value_fact_uniform_scale_origin_t origin) {
  if (value_id == LOOM_VALUE_ID_INVALID ||
      origin.source_value_id == LOOM_VALUE_ID_INVALID ||
      origin.scale_value_id == LOOM_VALUE_ID_INVALID) {
    return iree_ok_status();
  }
  IREE_RETURN_IF_ERROR(
      loom_value_fact_table_ensure_uniform_scale_origin_capacity(
          table, (iree_host_size_t)value_id + 1));
  if (table->uniform_scale_origins.entries[value_id].source_value_id ==
      LOOM_VALUE_ID_INVALID) {
    IREE_RETURN_IF_ERROR(
        loom_value_fact_table_append_touched_uniform_scale_origin(table,
                                                                  value_id));
  }
  table->uniform_scale_origins.entries[value_id] = origin;
  return iree_ok_status();
}

bool loom_value_fact_table_query_uniform_scale_origin(
    const loom_value_fact_table_t* table, const loom_module_t* module,
    loom_value_id_t value_id,
    loom_value_fact_uniform_scale_origin_t* out_origin) {
  if (out_origin) {
    *out_origin = loom_value_fact_uniform_scale_origin_invalid();
  }
  if (table == NULL || module == NULL || value_id >= module->values.count) {
    return false;
  }
  loom_value_fact_uniform_scale_origin_t origin =
      loom_value_fact_uniform_scale_origin_invalid();
  if (!loom_value_fact_table_lookup_uniform_scale_origin(table, value_id,
                                                         &origin) ||
      origin.source_value_id >= module->values.count ||
      origin.scale_value_id >= module->values.count) {
    return false;
  }

  const loom_type_t value_type = loom_module_value_type(module, value_id);
  const loom_type_t source_type =
      loom_module_value_type(module, origin.source_value_id);
  const loom_type_t scale_type =
      loom_module_value_type(module, origin.scale_value_id);
  if (!loom_type_is_vector(value_type) || !loom_type_is_vector(source_type) ||
      !loom_type_is_scalar(scale_type) ||
      loom_type_element_type(value_type) !=
          loom_type_element_type(source_type) ||
      loom_type_element_type(value_type) !=
          loom_type_element_type(scale_type)) {
    return false;
  }

  uint64_t value_lane_count = 0;
  uint64_t source_lane_count = 0;
  if (!loom_type_static_element_count(value_type, &value_lane_count) ||
      !loom_type_static_element_count(source_type, &source_lane_count) ||
      value_lane_count != source_lane_count) {
    return false;
  }

  if (out_origin) {
    *out_origin = origin;
  }
  return true;
}

static bool loom_value_fact_contextual_query_origins_equal(
    loom_value_fact_contextual_query_origin_t left,
    loom_value_fact_contextual_query_origin_t right) {
  return left.family_kind == right.family_kind &&
         loom_attribute_equal(&left.key, &right.key);
}

static bool loom_value_fact_table_lookup_contextual_query_origin(
    const loom_value_fact_table_t* table, loom_value_id_t value_id,
    loom_value_fact_contextual_query_origin_t* out_origin) {
  if (out_origin) {
    *out_origin = (loom_value_fact_contextual_query_origin_t){0};
  }
  if (value_id >= table->contextual_query_origins.capacity ||
      table->contextual_query_origins.entries == NULL) {
    return false;
  }
  const uint32_t origin_id = table->contextual_query_origins.entries[value_id];
  if (origin_id == 0 ||
      origin_id > table->contextual_query_origins.origin_count) {
    return false;
  }
  if (out_origin) {
    *out_origin = table->contextual_query_origins.origins[origin_id - 1];
  }
  return true;
}

iree_status_t loom_value_fact_table_define_contextual_query_origin(
    loom_value_fact_table_t* table, loom_value_id_t value_id,
    loom_value_fact_contextual_query_origin_t origin) {
  if (value_id == LOOM_VALUE_ID_INVALID) {
    return iree_ok_status();
  }
  IREE_ASSERT_NE(origin.family_kind, LOOM_PARAMETERIZED_ATTR_KIND_ANY);
  IREE_ASSERT_EQ(origin.reserved, 0u);
  IREE_RETURN_IF_ERROR(
      loom_value_fact_table_ensure_contextual_query_origin_capacity(
          table, (iree_host_size_t)value_id + 1));

  loom_value_fact_contextual_query_origin_t existing_origin = {0};
  if (loom_value_fact_table_lookup_contextual_query_origin(table, value_id,
                                                           &existing_origin)) {
    IREE_ASSERT(loom_value_fact_contextual_query_origins_equal(existing_origin,
                                                               origin));
    return iree_ok_status();
  }

  if (table->contextual_query_origins.origin_count >= UINT32_MAX) {
    return iree_make_status(
        IREE_STATUS_RESOURCE_EXHAUSTED,
        "contextual query origin count exceeds uint32_t range");
  }
  if (table->contextual_query_origins.origin_count >=
      table->contextual_query_origins.origin_capacity) {
    IREE_RETURN_IF_ERROR(iree_arena_grow_array(
        table->arena, table->contextual_query_origins.origin_count,
        table->contextual_query_origins.origin_count + 1,
        sizeof(*table->contextual_query_origins.origins),
        &table->contextual_query_origins.origin_capacity,
        (void**)&table->contextual_query_origins.origins));
  }
  const uint32_t origin_id =
      (uint32_t)++table->contextual_query_origins.origin_count;
  table->contextual_query_origins.origins[origin_id - 1] = origin;
  table->contextual_query_origins.entries[value_id] = origin_id;
  return loom_value_fact_table_append_touched_contextual_query_origin(table,
                                                                      value_id);
}

bool loom_value_fact_table_query_contextual_query_origin(
    const loom_value_fact_table_t* table, const loom_module_t* module,
    loom_value_id_t value_id,
    loom_value_fact_contextual_query_origin_t* out_origin) {
  if (out_origin) {
    *out_origin = (loom_value_fact_contextual_query_origin_t){0};
  }
  if (table == NULL || module == NULL || value_id >= module->values.count ||
      !loom_type_is_scalar(loom_module_value_type(module, value_id))) {
    return false;
  }
  return loom_value_fact_table_lookup_contextual_query_origin(table, value_id,
                                                              out_origin);
}

iree_status_t loom_value_fact_table_clone_defined_facts(
    loom_value_fact_table_t* target, const loom_value_fact_table_t* source,
    const loom_module_t* module) {
  for (iree_host_size_t i = 0; i < source->touched_count; ++i) {
    const loom_value_id_t value_id = source->touched_values[i];
    if (!loom_value_fact_table_has_entry(source, value_id)) {
      continue;
    }
    loom_value_facts_t cloned_facts = loom_value_facts_unknown();
    if (module && value_id < module->values.count) {
      IREE_RETURN_IF_ERROR(loom_value_fact_table_clone_fact_for_type(
          target, source, module, loom_module_value_type(module, value_id),
          source->entries[value_id], &cloned_facts));
    } else {
      IREE_RETURN_IF_ERROR(loom_value_fact_table_clone_fact(
          target, source, source->entries[value_id], &cloned_facts));
    }
    IREE_RETURN_IF_ERROR(
        loom_value_fact_table_define(target, value_id, cloned_facts));
    loom_value_id_t scalar_origin = LOOM_VALUE_ID_INVALID;
    if (loom_value_fact_table_lookup_uniform_element_origin(source, value_id,
                                                            &scalar_origin)) {
      IREE_RETURN_IF_ERROR(loom_value_fact_table_define_uniform_element_origin(
          target, value_id, scalar_origin));
    }
    loom_value_fact_static_lane_origin_t lane_origin =
        loom_value_fact_static_lane_origin_invalid();
    if (loom_value_fact_table_lookup_static_lane_origin(source, value_id,
                                                        &lane_origin)) {
      IREE_RETURN_IF_ERROR(loom_value_fact_table_define_static_lane_origin(
          target, value_id, lane_origin));
    }
    loom_value_fact_uniform_scale_origin_t scale_origin =
        loom_value_fact_uniform_scale_origin_invalid();
    if (loom_value_fact_table_lookup_uniform_scale_origin(source, value_id,
                                                          &scale_origin)) {
      IREE_RETURN_IF_ERROR(loom_value_fact_table_define_uniform_scale_origin(
          target, value_id, scale_origin));
    }
    loom_value_fact_contextual_query_origin_t query_origin = {0};
    if (loom_value_fact_table_lookup_contextual_query_origin(source, value_id,
                                                             &query_origin)) {
      IREE_RETURN_IF_ERROR(loom_value_fact_table_define_contextual_query_origin(
          target, value_id, query_origin));
    }
  }
  return iree_ok_status();
}

static iree_status_t loom_value_fact_table_forward_uniform_origin(
    loom_value_fact_table_t* table, loom_value_id_t source_value_id,
    loom_value_id_t result_value_id) {
  loom_value_id_t existing_origin = LOOM_VALUE_ID_INVALID;
  if (loom_value_fact_table_lookup_uniform_element_origin(
          table, result_value_id, &existing_origin)) {
    return iree_ok_status();
  }
  loom_value_id_t scalar_origin = LOOM_VALUE_ID_INVALID;
  if (!loom_value_fact_table_lookup_uniform_element_origin(
          table, source_value_id, &scalar_origin)) {
    return iree_ok_status();
  }
  return loom_value_fact_table_define_uniform_element_origin(
      table, result_value_id, scalar_origin);
}

static iree_status_t loom_value_fact_table_forward_static_lane_origin(
    loom_value_fact_table_t* table, loom_value_id_t source_value_id,
    loom_value_id_t result_value_id) {
  loom_value_fact_static_lane_origin_t existing_origin =
      loom_value_fact_static_lane_origin_invalid();
  if (loom_value_fact_table_lookup_static_lane_origin(table, result_value_id,
                                                      &existing_origin)) {
    return iree_ok_status();
  }
  loom_value_fact_static_lane_origin_t lane_origin =
      loom_value_fact_static_lane_origin_invalid();
  if (!loom_value_fact_table_lookup_static_lane_origin(table, source_value_id,
                                                       &lane_origin)) {
    return iree_ok_status();
  }
  return loom_value_fact_table_define_static_lane_origin(table, result_value_id,
                                                         lane_origin);
}

static iree_status_t loom_value_fact_table_forward_uniform_scale_origin(
    loom_value_fact_table_t* table, loom_value_id_t source_value_id,
    loom_value_id_t result_value_id) {
  loom_value_fact_uniform_scale_origin_t existing_origin =
      loom_value_fact_uniform_scale_origin_invalid();
  if (loom_value_fact_table_lookup_uniform_scale_origin(table, result_value_id,
                                                        &existing_origin)) {
    return iree_ok_status();
  }
  loom_value_fact_uniform_scale_origin_t scale_origin =
      loom_value_fact_uniform_scale_origin_invalid();
  if (!loom_value_fact_table_lookup_uniform_scale_origin(table, source_value_id,
                                                         &scale_origin)) {
    return iree_ok_status();
  }
  return loom_value_fact_table_define_uniform_scale_origin(
      table, result_value_id, scale_origin);
}

static iree_status_t loom_value_fact_table_forward_contextual_query_origin(
    loom_value_fact_table_t* table, loom_value_id_t source_value_id,
    loom_value_id_t result_value_id) {
  if (result_value_id == LOOM_VALUE_ID_INVALID ||
      source_value_id == LOOM_VALUE_ID_INVALID) {
    return iree_ok_status();
  }
  if (result_value_id < table->contextual_query_origins.capacity &&
      table->contextual_query_origins.entries != NULL &&
      table->contextual_query_origins.entries[result_value_id] != 0) {
    return iree_ok_status();
  }
  if (source_value_id >= table->contextual_query_origins.capacity ||
      table->contextual_query_origins.entries == NULL) {
    return iree_ok_status();
  }
  const uint32_t origin_id =
      table->contextual_query_origins.entries[source_value_id];
  if (origin_id == 0) {
    return iree_ok_status();
  }

  IREE_RETURN_IF_ERROR(
      loom_value_fact_table_ensure_contextual_query_origin_capacity(
          table, (iree_host_size_t)result_value_id + 1));
  table->contextual_query_origins.entries[result_value_id] = origin_id;
  return loom_value_fact_table_append_touched_contextual_query_origin(
      table, result_value_id);
}

iree_status_t loom_value_fact_table_propagate_origins(
    loom_value_fact_table_t* table, const loom_module_t* module,
    const loom_op_t* op) {
  const loom_trait_flags_t traits = loom_op_effective_traits(module, op);
  const loom_value_id_t* operands = loom_op_const_operands(op);
  const loom_value_id_t* results = loom_op_const_results(op);
  if (loom_traits_are_value_alias(traits) && op->operand_count >= 1 &&
      op->result_count >= 1) {
    IREE_RETURN_IF_ERROR(loom_value_fact_table_forward_uniform_origin(
        table, operands[0], results[0]));
    IREE_RETURN_IF_ERROR(loom_value_fact_table_forward_static_lane_origin(
        table, operands[0], results[0]));
    IREE_RETURN_IF_ERROR(loom_value_fact_table_forward_uniform_scale_origin(
        table, operands[0], results[0]));
    IREE_RETURN_IF_ERROR(loom_value_fact_table_forward_contextual_query_origin(
        table, operands[0], results[0]));
  }
  if (loom_traits_are_fact_identity(traits)) {
    const uint16_t pair_count = op->operand_count < op->result_count
                                    ? op->operand_count
                                    : op->result_count;
    for (uint16_t i = 0; i < pair_count; ++i) {
      IREE_RETURN_IF_ERROR(loom_value_fact_table_forward_uniform_origin(
          table, operands[i], results[i]));
      IREE_RETURN_IF_ERROR(loom_value_fact_table_forward_static_lane_origin(
          table, operands[i], results[i]));
      IREE_RETURN_IF_ERROR(loom_value_fact_table_forward_uniform_scale_origin(
          table, operands[i], results[i]));
      IREE_RETURN_IF_ERROR(
          loom_value_fact_table_forward_contextual_query_origin(
              table, operands[i], results[i]));
    }
  }
  return iree_ok_status();
}
