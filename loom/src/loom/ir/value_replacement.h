// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// Immutable substitution of one SSA identity through shared types and
// attributes.

#ifndef LOOM_IR_VALUE_REPLACEMENT_H_
#define LOOM_IR_VALUE_REPLACEMENT_H_

#include "loom/ir/ir.h"

#ifdef __cplusplus
extern "C" {
#endif

// Completed canonical substitution and its intrusive binary radix branch.
typedef struct loom_value_replacement_memo_t {
  // Low-bit-tagged leaves or untagged branches; zero is the empty map.
  uintptr_t edges[2];
  // Immutable module-local source type identity.
  loom_type_id_t source;
  // Canonical replacement type identity in the same module.
  loom_type_id_t result;
  // Differing source-ID bit, strictly decreasing along branch paths.
  uint32_t bit;
} loom_value_replacement_memo_t;

// One fixed substitution, shared across all carriers and attribute owners.
// Canonical types are immutable, so completed results remain valid while the
// context lives even as new types are interned or active owners are retargeted.
// Temporary storage comes from the module's block pool and never escapes into
// returned payloads. The module and both values outlive the context, whose
// address remains stable until deinitialization.
typedef struct loom_value_replacement_t {
  // Borrowed module owning input identities and canonical results.
  loom_module_t* module;
  // Defined SSA identity being removed from payloads.
  loom_value_id_t old_id;
  // Distinct defined SSA identity substituted for old_id.
  loom_value_id_t new_id;
  // Invocation-local memo, continuations and reconstruction arrays.
  iree_arena_allocator_t scratch;
  // Tagged root of the completed canonical-ID map.
  uintptr_t memo;
  // Inline first completed result; single-node substitutions need no memo
  // arena.
  loom_value_replacement_memo_t first_result;
  // Reusable arena-owned continuations, independent of completed memo entries.
  struct loom_value_replacement_frame_t* free_frames;
} loom_value_replacement_t;

// Begins substitution between two distinct, defined module values. Initializing
// an empty context does not allocate or inspect the module's type table.
void loom_value_replacement_initialize(
    loom_module_t* module, loom_value_id_t old_id, loom_value_id_t new_id,
    loom_value_replacement_t* out_replacement);

// Returns temporary blocks to the shared pool. Published types and attributes
// remain owned by the module, including results completed before a failure.
void loom_value_replacement_deinitialize(loom_value_replacement_t* replacement);

// Replaces references in a valid type without changing active ownership. Input
// payloads may be temporary; changed results are canonical and module-owned.
// Unchanged results preserve the original by-value input. On failure,
// |out_type| is the input and |out_changed| is false; completed canonical child
// types may remain in the module.
iree_status_t loom_value_replacement_type(loom_value_replacement_t* replacement,
                                          loom_type_t type,
                                          loom_type_t* out_type,
                                          bool* out_changed);

// Replaces references in a valid attribute using module-local identities.
// Changed aggregate payloads are module-owned; unchanged payloads are shared
// with the input. Does not mutate operation owners or their reference records.
iree_status_t loom_value_replacement_attribute(
    loom_value_replacement_t* replacement, loom_attribute_t attribute,
    loom_attribute_t* out_attribute, bool* out_changed);

// Standalone substitutions with value-ID precondition checking. These create a
// context for one input; callers replacing several owners of the same fixed
// pair use the shared context above. Identity substitution is a no-op.
iree_status_t loom_module_replace_type_value_references(
    loom_module_t* module, loom_type_t type, loom_value_id_t old_id,
    loom_value_id_t new_id, loom_type_t* out_type, bool* out_changed);

iree_status_t loom_module_replace_attribute_value_references(
    loom_module_t* module, loom_attribute_t attribute, loom_value_id_t old_id,
    loom_value_id_t new_id, loom_attribute_t* out_attribute, bool* out_changed);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_IR_VALUE_REPLACEMENT_H_
