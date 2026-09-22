// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/codegen/low/allocation/destructive_reuse.h"

#include <string.h>

// Structural SSA relations form a DAG even when CFG block order differs from
// definition order. Edge transfers are not identity relations in this graph.
static bool loom_low_allocation_reuse_relation(
    const loom_low_placement_relation_t* relation) {
  return loom_low_placement_relation_can_alias(relation) &&
         relation->cause >= LOOM_LOW_PLACEMENT_CAUSE_TIED_RESULT &&
         relation->cause <= LOOM_LOW_PLACEMENT_CAUSE_LOW_CONCAT;
}

static iree_status_t loom_low_allocation_refine_destructive_reuse_build(
    const loom_low_allocation_unit_liveness_t* unit_liveness,
    loom_low_placement_table_t* placement, iree_arena_allocator_t* scratch) {
  uint32_t* pending_users = NULL;
  loom_value_ordinal_t* order = NULL;
  uint32_t* preservation_ends = NULL;
  uint32_t* first_writes = NULL;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      scratch, placement->value_count, sizeof(*pending_users),
      (void**)&pending_users));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      scratch, placement->value_count, sizeof(*order), (void**)&order));
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      scratch, unit_liveness->point_count, sizeof(*preservation_ends),
      (void**)&preservation_ends));
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate_array(scratch, unit_liveness->point_count,
                                sizeof(*first_writes), (void**)&first_writes));
  memset(pending_users, 0, placement->value_count * sizeof(*pending_users));
  memcpy(preservation_ends, unit_liveness->end_points,
         unit_liveness->point_count * sizeof(*preservation_ends));
  memset(first_writes, 0xFF,
         unit_liveness->point_count * sizeof(*first_writes));
  const uint32_t* unit_starts = unit_liveness->point_starts_by_value_ordinal;
  for (iree_host_size_t i = 0; i < placement->relation_count; ++i) {
    const loom_low_placement_relation_t* relation = &placement->relations[i];
    if (!loom_low_allocation_reuse_relation(relation)) {
      continue;
    }
    ++pending_users[relation->source_ordinal];
    if (iree_any_bit_set(relation->flags,
                         LOOM_LOW_PLACEMENT_RELATION_FLAG_WRITES_STORAGE)) {
      const uint32_t source_start =
          unit_starts[relation->source_ordinal] + relation->source_unit_offset;
      for (uint32_t unit = 0; unit < relation->unit_count; ++unit) {
        first_writes[source_start + unit] =
            iree_min(first_writes[source_start + unit], relation->write_point);
      }
    }
  }

  // Visit users before sources. Required equalities retain the latest storage
  // observation through each tied-result chain, independently of SSA lifetime.
  loom_value_ordinal_t order_count = 0;
  for (loom_value_ordinal_t i = 0; i < placement->value_count; ++i) {
    if (pending_users[i] == 0) {
      order[order_count++] = i;
    }
  }
  for (loom_value_ordinal_t cursor = 0; cursor < order_count; ++cursor) {
    const loom_low_placement_relation_range_t range =
        placement->ranges_by_result_ordinal[order[cursor]];
    for (uint32_t i = 0; i < range.count; ++i) {
      const loom_low_placement_relation_t* relation =
          &placement->relations[range.start + i];
      if (!loom_low_allocation_reuse_relation(relation)) {
        continue;
      }
      if (relation->cause == LOOM_LOW_PLACEMENT_CAUSE_TIED_RESULT) {
        const uint32_t source_start = unit_starts[relation->source_ordinal] +
                                      relation->source_unit_offset;
        const uint32_t result_start = unit_starts[relation->result_ordinal] +
                                      relation->result_unit_offset;
        for (uint32_t unit = 0; unit < relation->unit_count; ++unit) {
          preservation_ends[source_start + unit] =
              iree_max(preservation_ends[source_start + unit],
                       preservation_ends[result_start + unit]);
        }
      }
      if (--pending_users[relation->source_ordinal] == 0) {
        order[order_count++] = relation->source_ordinal;
      }
    }
  }
  IREE_ASSERT_EQ(order_count, placement->value_count,
                 "structural SSA storage relations must be acyclic");

  // Identity siblings also share the family's storage observation bound.
  for (loom_value_ordinal_t cursor = order_count; cursor > 0; --cursor) {
    const loom_low_placement_relation_range_t range =
        placement->ranges_by_result_ordinal[order[cursor - 1]];
    for (uint32_t i = 0; i < range.count; ++i) {
      const loom_low_placement_relation_t* relation =
          &placement->relations[range.start + i];
      if (relation->cause != LOOM_LOW_PLACEMENT_CAUSE_TIED_RESULT) {
        continue;
      }
      const uint32_t source_start =
          unit_starts[relation->source_ordinal] + relation->source_unit_offset;
      const uint32_t result_start =
          unit_starts[relation->result_ordinal] + relation->result_unit_offset;
      for (uint32_t unit = 0; unit < relation->unit_count; ++unit) {
        preservation_ends[result_start + unit] =
            preservation_ends[source_start + unit];
      }
    }
  }

  // Retain the first possible write through each optional identity path. A
  // materialized relation cuts that path, so its sources do not inherit the
  // result's writes. Each relation and its unit mapping are visited once.
  for (loom_value_ordinal_t cursor = 0; cursor < order_count; ++cursor) {
    const loom_low_placement_relation_range_t range =
        placement->ranges_by_result_ordinal[order[cursor]];
    for (uint32_t i = 0; i < range.count; ++i) {
      loom_low_placement_relation_t* relation =
          &placement->relations[range.start + i];
      if (!loom_low_allocation_reuse_relation(relation)) {
        continue;
      }
      const uint32_t source_start =
          unit_starts[relation->source_ordinal] + relation->source_unit_offset;
      const uint32_t result_start =
          unit_starts[relation->result_ordinal] + relation->result_unit_offset;
      bool requires_copy = false;
      if (relation->cause != LOOM_LOW_PLACEMENT_CAUSE_TIED_RESULT) {
        for (uint32_t unit = 0; unit < relation->unit_count; ++unit) {
          requires_copy |= preservation_ends[source_start + unit] >
                           first_writes[result_start + unit];
        }
      }
      if (requires_copy) {
        relation->flags &= ~LOOM_LOW_PLACEMENT_RELATION_FLAG_CAN_ALIAS_STORAGE;
        continue;
      }
      for (uint32_t unit = 0; unit < relation->unit_count; ++unit) {
        first_writes[source_start + unit] =
            iree_min(first_writes[source_start + unit],
                     first_writes[result_start + unit]);
      }
    }
  }
  return iree_ok_status();
}

iree_status_t loom_low_allocation_refine_destructive_reuse(
    const loom_low_allocation_unit_liveness_t* unit_liveness,
    loom_low_placement_table_t* placement, iree_arena_allocator_t* arena) {
  bool has_write = false;
  bool has_optional_alias = false;
  for (iree_host_size_t i = 0; i < placement->relation_count; ++i) {
    const loom_low_placement_relation_t* relation = &placement->relations[i];
    has_write |= iree_any_bit_set(
        relation->flags, LOOM_LOW_PLACEMENT_RELATION_FLAG_WRITES_STORAGE);
    has_optional_alias |=
        relation->cause != LOOM_LOW_PLACEMENT_CAUSE_TIED_RESULT &&
        loom_low_allocation_reuse_relation(relation);
  }
  if (!has_write || !has_optional_alias) {
    return iree_ok_status();
  }
  iree_arena_allocator_t scratch;
  iree_arena_initialize(arena->block_pool, &scratch);
  iree_status_t status = loom_low_allocation_refine_destructive_reuse_build(
      unit_liveness, placement, &scratch);
  iree_arena_deinitialize(&scratch);
  return status;
}
