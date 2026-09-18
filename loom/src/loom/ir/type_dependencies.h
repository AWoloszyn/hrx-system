// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Canonical type-dependency membership and active value ownership.
//
// Producers combine retained child sets and immediate SSA references once at
// type construction. Immutable compressed radix sets share disjoint children;
// a provider has exactly one path to any containing root. Active reverse edges
// enumerate carriers without expanding provider/carrier pairs or deduplicating
// queries. Stored types alone do not keep their providers live.

#ifndef LOOM_IR_TYPE_DEPENDENCIES_H_
#define LOOM_IR_TYPE_DEPENDENCIES_H_

#include "loom/ir/ir.h"

#ifdef __cplusplus
extern "C" {
#endif

// Combines distinct, nonempty canonical sets. The inline union handles empty
// and identical roots before entering recursive membership construction.
iree_status_t loom_type_dependencies_union_nonempty(
    loom_type_use_table_t* table, loom_type_dependency_id_t first,
    loom_type_dependency_id_t second, loom_type_dependency_id_t* out_root);

// Unions two canonical sets. Empty and identical sets require no index access.
// Allocation failure can retain immutable facts but leaves active ownership
// unchanged. Keeping identity cases inline makes dependency-free construction a
// value operation without entering the recursive set implementation.
static inline iree_status_t loom_type_dependencies_union(
    loom_type_use_table_t* table, loom_type_dependency_id_t first,
    loom_type_dependency_id_t second, loom_type_dependency_id_t* out_root) {
  if (!first || first == second) {
    *out_root = second;
    return iree_ok_status();
  }
  if (!second) {
    *out_root = first;
    return iree_ok_status();
  }
  return loom_type_dependencies_union_nonempty(table, first, second, out_root);
}

// Adds one immediate SSA reference, including not-yet-defined value IDs.
iree_status_t loom_type_dependencies_add(loom_type_use_table_t* table,
                                         loom_type_dependency_id_t root,
                                         loom_value_id_t provider,
                                         loom_type_dependency_id_t* out_root);

// Adds immediate dimensions, encodings and parameter attributes to a summary.
// Types with only structural children are handled by the inline collector.
iree_status_t loom_type_dependencies_collect_immediate(
    loom_module_t* module, loom_type_t type,
    loom_type_dependency_id_t* out_root);

// Completes a canonical type's summary from its immediate fields and retained
// structural-child membership. The payload has passed construction validation;
// TYPE-valued parameters consume canonical roots instead of walking their
// types. Structural containers already have their entire summary and need no
// collector call, including containers whose retained membership is empty.
static inline iree_status_t loom_type_dependencies_collect(
    loom_module_t* module, loom_type_t type,
    loom_type_dependency_id_t structural_dependencies,
    loom_type_dependency_id_t* out_root) {
  *out_root = structural_dependencies;
  switch (loom_type_kind(type)) {
    case LOOM_TYPE_FUNCTION:
    case LOOM_TYPE_DIALECT:
    case LOOM_TYPE_REGISTER:
      return iree_ok_status();
    default:
      return loom_type_dependencies_collect_immediate(module, type, out_root);
  }
}

// Prepared assignment between a fallible construction and infallible commit.
// No index mutation may intervene between preparation and commit.
typedef struct loom_type_dependency_assignment_t {
  // Full declared set, retained even while some providers are unavailable.
  loom_type_dependency_id_t declared;
  // Prefix containing only providers below the supplied availability limit.
  loom_type_dependency_id_t active;
  // Prepared carrier slot, or zero when the declaration is dependency-free.
  uint32_t carrier;
} loom_type_dependency_assignment_t;

// Prepares an assignment for an existing or newly reserved value-table slot.
// Neither the value nor its active dependencies change on allocation failure.
iree_status_t loom_type_dependencies_prepare(
    loom_type_use_table_t* table, loom_value_id_t value_id,
    loom_type_dependency_id_t root, iree_host_size_t available_value_count,
    loom_type_dependency_assignment_t* out_assignment);

// Publishes prepared ownership. The caller publishes the corresponding type in
// the same infallible construction step. Dependency-free carriers are recycled.
void loom_type_dependencies_commit(
    loom_type_use_table_t* table, loom_value_id_t value_id,
    const loom_type_dependency_assignment_t* assignment);

// Reactivates the retained declared set over the current value-table prefix.
iree_status_t loom_type_dependencies_refresh(loom_type_use_table_t* table,
                                             loom_value_id_t value_id);

// Prepares all live carriers before changing any ownership, including bodyless
// signature arguments retained by operand uses. Consumes declared facts, never
// reconstructs them from value payloads. Allocation failure preserves
// ownership.
iree_status_t loom_type_dependencies_recompute(loom_type_use_table_t* table);

// Removes active ownership while retaining the declaration for later refresh.
void loom_type_dependencies_drop(loom_type_use_table_t* table,
                                 loom_value_id_t value_id);

// Constant-time active ownership query. Out-of-range values have no users.
bool loom_type_dependencies_has_users(const loom_type_use_table_t* table,
                                      loom_value_id_t value_id);

// Bounded cursor for either direction. Mutation invalidates a live cursor.
// Outgoing membership is in increasing provider-ID order; incoming carriers
// have deterministic but unspecified order. Each result appears exactly once.
typedef struct loom_type_use_iterator_t {
  // Immutable membership and active links borrowed for the cursor lifetime.
  const loom_type_dependency_index_t* index;
  // Pending nodes (outgoing) or parent edges (incoming), at most 32 levels.
  uint32_t pending[33];
  // Number of initialized pending entries.
  uint32_t pending_count;
  // Next carrier at the current incoming node; unused for outgoing iteration.
  uint32_t carrier;
} loom_type_use_iterator_t;

// Begins iterating the active dependencies of a value, or an empty range for an
// out-of-range value. A dropped carrier has no active dependencies.
void loom_type_dependencies_begin(const loom_type_use_table_t* table,
                                  loom_value_id_t value_id,
                                  loom_type_use_iterator_t* out_iterator);

// Begins iterating carriers whose active types reference a provider.
void loom_type_users_begin(const loom_type_use_table_t* table,
                           loom_value_id_t value_id,
                           loom_type_use_iterator_t* out_iterator);

// Returns the next provider or LOOM_VALUE_ID_INVALID at the end of the range.
loom_value_id_t loom_type_dependencies_next(loom_type_use_iterator_t* iterator);

// Returns the next carrier or LOOM_VALUE_ID_INVALID at the end of the range.
loom_value_id_t loom_type_users_next(loom_type_use_iterator_t* iterator);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_IR_TYPE_DEPENDENCIES_H_
