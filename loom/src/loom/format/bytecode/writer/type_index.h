// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LOOM_FORMAT_BYTECODE_WRITER_TYPE_INDEX_H_
#define LOOM_FORMAT_BYTECODE_WRITER_TYPE_INDEX_H_

#include "iree/base/internal/arena.h"
#include "loom/ir/ir.h"

#ifdef __cplusplus
extern "C" {
#endif

// Exact canonical storage node and its dependencies. The module owns
// all borrowed type payloads; the index owns the ordered dependency slice.
typedef struct loom_bytecode_type_node_t {
  // Borrowed by-value type retaining the first-use physical payload.
  loom_type_t type;
  // Hash of exact storage, including payload addresses and SSA identities.
  uint32_t storage_hash;
  // Canonical module index identifying this exact type.
  loom_type_id_t module_index;
  // Ordered immediate child-node references, including repeated occurrences.
  struct {
    // Beginning of the slice in the index dependency array.
    iree_host_size_t begin;
    // Number of immediate dependencies.
    iree_host_size_t count;
    // Explicit child slots at the start of the slice, excluding scalar closure.
    iree_host_size_t explicit_count;
  } dependencies;
  // Whether this node or a child contains an actual SSA binding.
  bool has_bindings;
  // Scope generation which owns the completed binding ordinal.
  uint32_t binding_generation;
  // One-based completed binding ordinal in that scope.
  uint32_t binding;
} loom_bytecode_type_node_t;

// Explicit postorder continuation retained from graph construction.
typedef struct loom_bytecode_type_frame_t {
  // Node being completed.
  uint32_t node;
  // Next immediate edge.
  iree_host_size_t next_dependency;
} loom_bytecode_type_frame_t;

// Invocation-owned immediate dependency graph over canonical module types.
// Each physically shared type is analyzed once. Static types use global wire
// IDs; SSA-dependent types use scope-local records without shape folding.
// The immutable module outlives the index, and the supplied scratch arena owns
// all index storage.
typedef struct loom_bytecode_type_index_t {
  // Immutable module owning all borrowed type and attribute payloads.
  const loom_module_t* module;
  // Distinct by-value canonical storage nodes.
  loom_bytecode_type_node_t* nodes;
  // Number of populated storage nodes.
  iree_host_size_t count;
  // Allocated storage-node capacity.
  iree_host_size_t capacity;
  // Storage-node IDs indexed by exact representation hash, or UINT32_MAX.
  uint32_t* slots;
  // Power-of-two capacity of slots.
  iree_host_size_t slot_capacity;
  // Retained ordered immediate dependency node IDs, owned by the scratch arena.
  uint32_t* dependencies;
  // Reusable explicit traversal stack.
  loom_bytecode_type_frame_t* stack;
  // Completed nodes in the current extension batch, allocated lazily.
  uint32_t* pending;
  // Monotonic generation assigned to each independent value scope.
  uint32_t binding_generation;
} loom_bytecode_type_index_t;

// Builds immediate dependency slices without changing module or wire order.
// Fallibility is limited to scratch allocation and index-size representation.
iree_status_t loom_bytecode_type_index_initialize(
    const loom_module_t* module, iree_arena_allocator_t* arena,
    loom_bytecode_type_index_t* out_index);

// Returns the exact physical node, or NULL when the type has no retained
// storage. The node owns its canonical module index and immediate dependencies.
const loom_bytecode_type_node_t* loom_bytecode_type_index_lookup_node(
    const loom_bytecode_type_index_t* index, loom_type_t type);

// Returns the canonical module-table entry for a retained type, or
// LOOM_TYPE_ID_INVALID when the type is not in the module's retained closure.
// No structural traversal or allocation occurs during lookup.
loom_type_id_t loom_bytecode_type_index_lookup(
    const loom_bytecode_type_index_t* index, loom_type_t type);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_FORMAT_BYTECODE_WRITER_TYPE_INDEX_H_
