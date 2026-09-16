// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Coalesced storage components that relocate as a unit.

#ifndef LOOM_CODEGEN_LOW_ALLOCATION_RELOCATION_GROUP_H_
#define LOOM_CODEGEN_LOW_ALLOCATION_RELOCATION_GROUP_H_

#include "iree/base/internal/arena.h"
#include "loom/codegen/low/allocation/assignment_map.h"
#include "loom/codegen/low/descriptors.h"
#include "loom/codegen/low/placement.h"

#ifdef __cplusplus
extern "C" {
#endif

// Components of the final allocation's coalesced non-edge placement relations.
// Moving all members by the same physical offset preserves slice, concat,
// copy and tied-result aliases. Control-flow transfers remain outside these
// components: their distinct dynamic values must satisfy edge-handoff proofs.
// Whole-component translations preserve this index; changing member-relative
// locations or assignments requires rebuilding it.
typedef struct loom_low_allocation_relocation_groups_t {
  // Component representative indexed by assignment table index.
  uint32_t* representatives;
  // Circular member list indexed by assignment table index. A singleton
  // references itself, so any member can start a complete component traversal.
  uint32_t* next_members;
} loom_low_allocation_relocation_groups_t;

// Constructs components once from retained placement and concrete assignments.
// All output storage belongs to |arena|; the input tables remain unchanged.
iree_status_t loom_low_allocation_relocation_groups_initialize(
    const loom_low_descriptor_set_t* descriptor_set,
    const loom_low_placement_table_t* placement,
    const loom_low_allocation_assignment_map_t* assignment_map,
    iree_arena_allocator_t* arena,
    loom_low_allocation_relocation_groups_t* out_groups);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_CODEGEN_LOW_ALLOCATION_RELOCATION_GROUP_H_
