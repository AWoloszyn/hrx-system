// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/codegen/low/allocation/relocation_group.h"

#include <string.h>

#include "loom/codegen/low/allocation/storage.h"

static uint32_t loom_low_allocation_relocation_group_find(
    uint32_t* representatives, uint32_t assignment_index) {
  uint32_t representative = assignment_index;
  while (representatives[representative] != representative) {
    representative = representatives[representative];
  }
  while (assignment_index != representative) {
    const uint32_t next = representatives[assignment_index];
    representatives[assignment_index] = representative;
    assignment_index = next;
  }
  return representative;
}

iree_status_t loom_low_allocation_relocation_groups_initialize(
    const loom_low_descriptor_set_t* descriptor_set,
    const loom_low_placement_table_t* placement,
    const loom_low_allocation_assignment_map_t* assignment_map,
    iree_arena_allocator_t* arena,
    loom_low_allocation_relocation_groups_t* out_groups) {
  *out_groups = (loom_low_allocation_relocation_groups_t){0};
  const iree_host_size_t count = assignment_map->assignment_count;
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, count, sizeof(*out_groups->representatives),
      (void**)&out_groups->representatives));
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate_array(arena, count, sizeof(*out_groups->next_members),
                                (void**)&out_groups->next_members));
  for (iree_host_size_t i = 0; i < count; ++i) {
    out_groups->representatives[i] = (uint32_t)i;
    out_groups->next_members[i] = (uint32_t)i;
  }

  const iree_arena_checkpoint_t checkpoint = iree_arena_checkpoint_save(arena);
  uint8_t* ranks = NULL;
  IREE_RETURN_IF_ERROR(
      iree_arena_allocate_array(arena, count, sizeof(*ranks), (void**)&ranks));
  memset(ranks, 0, count * sizeof(*ranks));
  for (iree_host_size_t i = 0; i < placement->relation_count; ++i) {
    const loom_low_placement_relation_t* relation = &placement->relations[i];
    if (!loom_low_placement_relation_can_alias(relation) ||
        loom_low_placement_cause_is_edge(relation->cause)) {
      continue;
    }
    uint32_t result_index = 0;
    uint32_t source_index = 0;
    const loom_low_allocation_assignment_t* result =
        loom_low_allocation_assignment_map_assignment_for_value_ordinal(
            assignment_map, relation->result_ordinal, &result_index);
    const loom_low_allocation_assignment_t* source =
        loom_low_allocation_assignment_map_assignment_for_value_ordinal(
            assignment_map, relation->source_ordinal, &source_index);
    if (result == NULL || source == NULL ||
        !loom_low_allocation_assignment_is_register_like(result) ||
        !loom_low_allocation_assignment_is_register_like(source) ||
        !loom_low_allocation_storage_placement_relation_satisfied(
            descriptor_set, relation, result, source)) {
      continue;
    }
    uint32_t result_root = loom_low_allocation_relocation_group_find(
        out_groups->representatives, result_index);
    uint32_t source_root = loom_low_allocation_relocation_group_find(
        out_groups->representatives, source_index);
    if (result_root == source_root) {
      continue;
    }
    if (ranks[result_root] < ranks[source_root]) {
      const uint32_t temporary = result_root;
      result_root = source_root;
      source_root = temporary;
    }
    out_groups->representatives[source_root] = result_root;
    if (ranks[result_root] == ranks[source_root]) {
      ++ranks[result_root];
    }
    const uint32_t next = out_groups->next_members[result_root];
    out_groups->next_members[result_root] =
        out_groups->next_members[source_root];
    out_groups->next_members[source_root] = next;
  }
  for (iree_host_size_t i = 0; i < count; ++i) {
    out_groups->representatives[i] = loom_low_allocation_relocation_group_find(
        out_groups->representatives, (uint32_t)i);
  }
  iree_arena_checkpoint_restore(&checkpoint);
  return iree_ok_status();
}
