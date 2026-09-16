// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// SSA references embedded in types and operation attributes: payload traversal,
// immutable replacement, and exact incoming/outgoing attribute-use ownership.

#ifndef LOOM_IR_VALUE_REFS_H_
#define LOOM_IR_VALUE_REFS_H_

#include "loom/ir/ir.h"

#ifdef __cplusplus
extern "C" {
#endif

// Walks outgoing SSA references owned by |op| and its nested regions: ordinary
// operands, result/block-argument type references, and attribute references.
// Uses retained reference records without reconstructing type or attribute
// payloads. Values may be visited more than once; ordering is unspecified.
//
// Erase and DCE paths use this before unlinking a subtree to notify providers
// whose users will disappear. The callback must not mutate IR structure or
// reference lists. The walk allocates no storage and propagates callback
// errors.
iree_status_t loom_op_walk_subtree_value_refs(
    const loom_module_t* module, const loom_op_t* op,
    loom_type_value_ref_callback_t callback, void* user_data);

// Replaces SSA references to |old_id| embedded in |type| with |new_id| and
// interns the resulting type in |module|. The module value table and type-use
// side table are not mutated; callers decide which carrier value, if any, owns
// the returned type.
iree_status_t loom_module_replace_type_value_references(
    loom_module_t* module, loom_type_t type, loom_value_id_t old_id,
    loom_value_id_t new_id, loom_type_t* out_type, bool* out_changed);

// Replaces all SSA references to |old_id| embedded in value types with
// |new_id| and updates the module's type-use side table.
iree_status_t loom_module_replace_value_type_uses(loom_module_t* module,
                                                  loom_value_id_t old_id,
                                                  loom_value_id_t new_id);

// Walks SSA value references embedded in |attr|. Type-valued attributes are
// resolved through |module| and aggregate attributes are visited in structural
// order. References are not deduplicated.
iree_status_t loom_module_walk_attribute_value_refs(
    const loom_module_t* module, loom_attribute_t attr,
    loom_type_value_ref_callback_t callback, void* user_data);

// Replaces SSA references to |old_id| embedded in |attr| with |new_id|.
// Aggregate payloads and type-valued attributes are rebuilt in |module| only
// when a nested reference changes.
iree_status_t loom_module_replace_attribute_value_references(
    loom_module_t* module, loom_attribute_t attr, loom_value_id_t old_id,
    loom_value_id_t new_id, loom_attribute_t* out_attr, bool* out_changed);

// Incoming attribute-use heads for one defined module value.
static inline const loom_value_attribute_use_heads_t*
loom_module_value_attribute_use_heads(const loom_module_t* module,
                                      loom_value_id_t value_id) {
  const loom_value_segment_t* segment =
      loom_value_table_const_segment_for_id(&module->values, value_id);
  return &segment->attribute_use_heads[value_id & LOOM_VALUE_SEGMENT_MASK];
}

// First exact attribute use of a defined value, or zero when there are none.
static inline loom_attribute_use_id_t loom_module_value_first_attribute_use(
    const loom_module_t* module, loom_value_id_t value_id) {
  const loom_value_attribute_use_heads_t* heads =
      loom_module_value_attribute_use_heads(module, value_id);
  return heads->type ? heads->type : heads->predicate;
}

// Replaces an operation attribute and its exact reference records. The old
// attribute and index remain unchanged on allocation or payload-validation
// failure. References to not-yet-defined values are ignored during construction
// and resolved by the reader's final use rebuild. Generic semantic traits are
// maintained by loom_op_set_attr, not this storage-level mutation.
iree_status_t loom_module_set_op_attribute(loom_module_t* module, loom_op_t* op,
                                           uint8_t attribute_index,
                                           loom_attribute_t attribute);

// Replaces all references to |old_id| in one known attribute owner with the
// distinct, defined |new_id|. The retained index establishes that this slot
// references |old_id|. Identity substitution preserves reference multiplicity
// and type/predicate classification, so existing records are retargeted without
// index allocation. Payload reconstruction may allocate; on failure the old
// attribute and its records remain intact. The caller maintains semantic traits
// and summaries after success, as with loom_module_set_op_attribute.
iree_status_t loom_module_replace_op_attribute_value_references(
    loom_module_t* module, loom_op_t* op, uint8_t attribute_index,
    loom_value_id_t old_id, loom_value_id_t new_id);

// Registers attributes at the operation construction boundary. Also refreshes
// existing records if a bulk construction path populated the attributes
// directly.
iree_status_t loom_module_refresh_op_attribute_uses(loom_module_t* module,
                                                    loom_op_t* op);

// Drops the records owned by a single attribute without allocating or changing
// its payload. Used when clearing a reference-carrying attribute during
// erasure.
void loom_module_drop_attribute_uses(loom_module_t* module, loom_op_t* op,
                                     uint8_t attribute_index);

// Drops every outgoing attribute reference when its operation is erased.
void loom_module_drop_op_attribute_uses(loom_module_t* module, loom_op_t* op);

// Clears the index before the bulk reader rebuilds live operation use records.
// Storage is retained for reuse; incoming and outgoing heads are reset
// together.
void loom_module_reset_attribute_uses(loom_module_t* module);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_IR_VALUE_REFS_H_
