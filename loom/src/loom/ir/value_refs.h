// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

// SSA references embedded in types and operation attributes: indexed traversal
// and exact incoming/outgoing ownership maintained during mutation.

#ifndef LOOM_IR_VALUE_REFS_H_
#define LOOM_IR_VALUE_REFS_H_

#include "loom/ir/ir.h"

#ifdef __cplusplus
extern "C" {
#endif

// Walks outgoing SSA references owned by |op| and its nested regions: ordinary
// operands, result/block/signature-argument type references, and attribute
// references. Declaration-owned operands are definitions, not references.
// Uses retained reference indexes without reconstructing type or attribute
// payloads. Values may be visited more than once; ordering is unspecified.
//
// Erase and DCE paths use this before unlinking a subtree to notify providers
// whose users will disappear. The callback must not mutate IR structure or
// reference lists. The walk allocates no storage and propagates callback
// errors.
iree_status_t loom_op_walk_subtree_value_refs(
    const loom_module_t* module, const loom_op_t* op,
    loom_type_value_ref_callback_t callback, void* user_data);

typedef struct loom_value_replacement_t loom_value_replacement_t;

// Applies one fixed substitution to every active value type carrying its old
// identity. Each carrier and its use index publish together; earlier carriers
// may already have changed if reconstructing a later carrier fails.
iree_status_t loom_value_replacement_apply_types(
    loom_value_replacement_t* replacement);

// Replaces all SSA references to |old_id| embedded in value types with
// |new_id| and updates the module's type-use side table. Uses one shared
// replacement context for all carriers, with the same partial-progress failure
// semantics as loom_value_replacement_apply_types.
iree_status_t loom_module_replace_value_type_uses(loom_module_t* module,
                                                  loom_value_id_t old_id,
                                                  loom_value_id_t new_id);

// Walks SSA value references embedded in |attr|. Type-valued attributes are
// resolved through |module| and aggregate attributes are visited in structural
// order. References are not deduplicated.
iree_status_t loom_module_walk_attribute_value_refs(
    const loom_module_t* module, loom_attribute_t attr,
    loom_type_value_ref_callback_t callback, void* user_data);

// Replaces an operation attribute and its retained dependency ownership. The
// old attribute and index remain unchanged on allocation or payload-validation
// failure. References to not-yet-defined values remain inactive until refresh
// or the reader's final use rebuild. Generic semantic traits are maintained by
// loom_op_set_attr, not this storage-level mutation.
iree_status_t loom_module_set_op_attribute(loom_module_t* module, loom_op_t* op,
                                           uint8_t attribute_index,
                                           loom_attribute_t attribute);

// Applies the fixed substitution to one known attribute owner. The retained
// index establishes that this slot references the old identity. Payload and
// membership preparation may allocate; on failure the old attribute and its
// ownership remain intact. The caller maintains semantic traits
// and summaries after success, as with loom_module_set_op_attribute.
iree_status_t loom_value_replacement_apply_attribute(
    loom_value_replacement_t* replacement, loom_op_t* op,
    uint8_t attribute_index);

// Registers attributes at the operation construction boundary. Also refreshes
// retained ownership if a bulk construction path populated the attributes
// directly.
iree_status_t loom_module_refresh_op_attribute_uses(loom_module_t* module,
                                                    loom_op_t* op);

// Drops ownership of a single attribute without allocating or changing
// its payload. Used when clearing a reference-carrying attribute during
// erasure.
void loom_module_drop_attribute_uses(loom_module_t* module, loom_op_t* op,
                                     uint8_t attribute_index);

// Drops every outgoing attribute reference when its operation is erased.
void loom_module_drop_op_attribute_uses(loom_module_t* module, loom_op_t* op);

// Clears attribute ownership before the bulk reader rebuilds live operations.
// Storage and canonical membership are retained for reuse. Value-type ownership
// is unchanged.
void loom_module_reset_attribute_uses(loom_module_t* module);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // LOOM_IR_VALUE_REFS_H_
