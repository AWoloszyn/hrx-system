// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/verify/verify_value_types.h"

#include "loom/error/error_catalog.h"
#include "loom/ops/op_defs.h"
#include "loom/verify/verify_diagnostics.h"

//===----------------------------------------------------------------------===//
// SSA references carried by value types
//===----------------------------------------------------------------------===//

// Field names are diagnostic-only work. Keep formatting out of the valid path,
// including variadic results and block arguments whose names need an ordinal.
static loom_diagnostic_param_t loom_verify_type_ref_field_param(
    const loom_op_t* op, const loom_op_vtable_t* vtable, uint16_t field_index,
    bool is_result, char* buffer, iree_host_size_t buffer_size) {
  if (!vtable) {
    iree_snprintf(buffer, buffer_size, "block arg %u", field_index);
    return loom_param_string(iree_make_cstring_view(buffer));
  }
  const uint8_t category = is_result ? LOOM_FIELD_RESULT : LOOM_FIELD_OPERAND;
  const loom_diagnostic_field_kind_t kind =
      is_result ? LOOM_DIAGNOSTIC_FIELD_RESULT : LOOM_DIAGNOSTIC_FIELD_OPERAND;
  const iree_string_view_t field_name = loom_verify_value_field_name(
      vtable, op, category, field_index, buffer, buffer_size);
  return loom_param_with_field_ref(
      loom_param_string(field_name),
      loom_diagnostic_field_ref(kind, field_index));
}

static bool loom_verify_op_allows_declaration_local_refs(
    const loom_op_vtable_t* vtable) {
  return vtable->symbol_def &&
         loom_symbol_definition_implements(vtable->symbol_def,
                                           LOOM_SYMBOL_INTERFACE_GLOBAL) &&
         iree_any_bit_set(vtable->traits, LOOM_TRAIT_SYMBOL_DEFINE);
}

// Definition-site references may name co-results or global declaration-local
// placeholders. The constructor retains result ownership, so co-reference
// checks never search the result array for each referenced value.
static bool loom_verify_definition_ref_is_visible(
    const loom_verify_state_t* state, const loom_op_t* op,
    const loom_op_vtable_t* vtable, loom_value_id_t value_id,
    bool allows_local_definitions) {
  if (loom_verify_value_is_visible(state, value_id)) {
    return true;
  }
  if (!allows_local_definitions) {
    return false;
  }
  const loom_value_t* value = loom_module_value(state->module, value_id);
  if (loom_value_is_block_arg(value)) {
    return false;
  }
  const loom_op_t* defining_op = loom_value_def_op(value);
  return defining_op == op ||
         (!defining_op && value->name_id != LOOM_STRING_ID_INVALID &&
          loom_verify_op_allows_declaration_local_refs(vtable));
}

// Validates a single SSA encoding reference embedded in a value's type.
// If the type carries LOOM_ENCODING_FLAG_SSA, the encoding_id is a
// value_id that must be in range and have type LOOM_TYPE_ENCODING. It must
// also be defined in scope unless the reference is to a sibling result in the
// current op type annotation or to a declaration-local global placeholder.
static void loom_verify_encoding_ref(loom_verify_state_t* state,
                                     const loom_op_t* op,
                                     const loom_op_vtable_t* vtable,
                                     loom_type_t type, uint16_t field_index,
                                     bool is_result) {
  if (!loom_type_has_ssa_encoding(type)) {
    return;
  }
  uint16_t encoding_value_id = loom_type_encoding_value_id(type);
  if (encoding_value_id >= state->module->values.count) {
    char name_buffer[64];
    loom_diagnostic_param_t params[] = {
        loom_verify_type_ref_field_param(op, vtable, field_index, is_result,
                                         name_buffer, sizeof(name_buffer)),
        loom_param_u32(encoding_value_id),
        loom_param_u32((uint32_t)state->module->values.count),
    };
    loom_verify_emit_structured(state, op, LOOM_ERR_ENCODING_003, params,
                                IREE_ARRAYSIZE(params));
    return;
  }
  if (!loom_verify_definition_ref_is_visible(state, op, vtable,
                                             encoding_value_id, is_result)) {
    iree_string_view_t value_name =
        loom_verify_value_name(state, encoding_value_id);
    char name_buffer[64];
    loom_diagnostic_param_t params[] = {
        loom_verify_type_ref_field_param(op, vtable, field_index, is_result,
                                         name_buffer, sizeof(name_buffer)),
        loom_param_string(value_name),
    };
    loom_verify_emit_structured(state, op, LOOM_ERR_ENCODING_004, params,
                                IREE_ARRAYSIZE(params));
    return;
  }
  loom_type_t encoding_type =
      loom_module_value_type(state->module, encoding_value_id);
  if (!loom_type_is_encoding(encoding_type)) {
    iree_string_view_t value_name =
        loom_verify_value_name(state, encoding_value_id);
    char name_buffer[64];
    loom_diagnostic_param_t params[] = {
        loom_verify_type_ref_field_param(op, vtable, field_index, is_result,
                                         name_buffer, sizeof(name_buffer)),
        loom_param_string(value_name),
        loom_param_type(encoding_type),
    };
    loom_verify_emit_structured(state, op, LOOM_ERR_ENCODING_005, params,
                                IREE_ARRAYSIZE(params));
  }
}

// The type-use producer retains all nested references. Verify each definition's
// outgoing edges once instead of traversing its type tree at every operand use.
static void loom_verify_defined_type_refs(
    loom_verify_state_t* state, const loom_op_t* op,
    const loom_op_vtable_t* vtable, loom_value_id_t value_id, loom_type_t type,
    uint16_t field_index, bool is_result) {
  loom_verify_encoding_ref(state, op, vtable, type, field_index, is_result);
  const loom_value_id_t direct_encoding =
      loom_type_has_ssa_encoding(type) ? loom_type_encoding_value_id(type)
                                       : LOOM_VALUE_ID_INVALID;
  for (loom_type_use_id_t use_id =
           loom_module_value_first_outgoing_type_use(state->module, value_id);
       use_id != LOOM_TYPE_USE_ID_INVALID;) {
    const loom_type_use_t* use = &state->module->type_uses.records[use_id];
    use_id = use->next_outgoing_use_id;
    const loom_value_id_t referenced_id = use->referenced_value_id;
    if (referenced_id == direct_encoding ||
        loom_verify_definition_ref_is_visible(state, op, vtable, referenced_id,
                                              is_result)) {
      continue;
    }
    char name_buffer[64];
    loom_diagnostic_param_t params[] = {
        loom_verify_type_ref_field_param(op, vtable, field_index, is_result,
                                         name_buffer, sizeof(name_buffer)),
        loom_param_string(loom_verify_value_name(state, referenced_id)),
    };
    loom_verify_emit_structured(state, op, LOOM_ERR_DOMINANCE_016, params,
                                IREE_ARRAYSIZE(params));
    if (loom_verify_at_error_limit(state)) {
      return;
    }
  }
}

void loom_verify_value_type_refs(loom_verify_state_t* state,
                                 const loom_op_t* op,
                                 const loom_op_vtable_t* vtable) {
  const bool defines_arguments = loom_verify_has_func_signature_scope(vtable) &&
                                 loom_op_vtable_owns_operands(vtable);
  const loom_value_id_t* operands = loom_op_const_operands(op);
  for (uint16_t i = 0; i < op->operand_count; ++i) {
    if (operands[i] == LOOM_VALUE_ID_INVALID ||
        operands[i] >= state->module->values.count) {
      continue;
    }
    loom_type_t type = loom_module_value_type(state->module, operands[i]);
    if ((!loom_type_has_ssa_encoding(type) && !defines_arguments) ||
        !loom_type_may_reference_values(type)) {
      continue;
    }
    if (defines_arguments) {
      loom_verify_defined_type_refs(state, op, vtable, operands[i], type, i,
                                    /*is_result=*/false);
    } else {
      loom_verify_encoding_ref(state, op, vtable, type, i,
                               /*is_result=*/false);
    }
  }
  const loom_value_id_t* results = loom_op_const_results(op);
  for (uint16_t i = 0; i < op->result_count; ++i) {
    if (results[i] == LOOM_VALUE_ID_INVALID ||
        results[i] >= state->module->values.count) {
      continue;
    }
    loom_type_t type = loom_module_value_type(state->module, results[i]);
    if (!loom_type_may_reference_values(type)) {
      continue;
    }
    loom_verify_defined_type_refs(state, op, vtable, results[i], type, i,
                                  /*is_result=*/true);
  }
}

// Checks SSA references in block argument types after all arguments are defined
// but before operations are verified. Referenced values must already be visible
// in the current scope. Block arguments carry no source location; the owning
// operation anchors diagnostics at their containing scope.
void loom_verify_block_arg_type_refs(loom_verify_state_t* state,
                                     const loom_block_t* block,
                                     const loom_op_t* owner) {
  for (uint16_t a = 0; a < block->arg_count; ++a) {
    loom_value_id_t arg_id = loom_block_arg_id(block, a);
    if (arg_id == LOOM_VALUE_ID_INVALID ||
        arg_id >= state->module->values.count) {
      continue;
    }
    loom_type_t type = loom_module_value_type(state->module, arg_id);
    if (!loom_type_may_reference_values(type)) {
      continue;
    }
    loom_verify_defined_type_refs(state, owner, NULL, arg_id, type, a,
                                  /*is_result=*/false);
  }
}

IREE_ATTRIBUTE_NOINLINE IREE_ATTRIBUTE_COLD static void
loom_verify_emit_attribute_ref_not_visible(loom_verify_state_t* state,
                                           const loom_op_t* op,
                                           const loom_op_vtable_t* vtable,
                                           uint8_t attribute_index,
                                           loom_value_id_t value_id) {
  char name_buffer[32];
  iree_string_view_t field_name;
  if (vtable->attr_descriptors && attribute_index < vtable->attribute_count) {
    field_name =
        loom_bstring_view(vtable->attr_descriptors[attribute_index].name);
  } else {
    // Structural verification diagnoses undescribed slots separately.
    iree_snprintf(name_buffer, sizeof(name_buffer), "attribute %u",
                  attribute_index);
    field_name = iree_make_cstring_view(name_buffer);
  }
  loom_diagnostic_param_t params[] = {
      loom_verify_param_string_for_diagnostic_field(
          field_name, LOOM_DIAGNOSTIC_FIELD_ATTRIBUTE, attribute_index),
      loom_param_string(loom_verify_value_name(state, value_id)),
  };
  loom_verify_emit_structured(state, op, LOOM_ERR_DOMINANCE_017, params,
                              IREE_ARRAYSIZE(params));
}

// Attributes retain their nested type and predicate references at construction.
// Signature predicates may describe the declaration's own results or global
// shape placeholders; ordinary attributes require a dominating definition.
void loom_verify_attribute_value_refs(loom_verify_state_t* state,
                                      const loom_op_t* op,
                                      const loom_op_vtable_t* vtable) {
  const bool allows_local_definitions =
      iree_any_bit_set(vtable->traits, LOOM_TRAIT_SYMBOL_DEFINE);
  const loom_attribute_use_id_t* heads = loom_op_attribute_use_heads(op);
  for (uint8_t i = 0; i < op->attribute_count; ++i) {
    for (loom_attribute_use_id_t use_id = heads[i]; use_id;) {
      const loom_attribute_use_t* use =
          &state->module->attribute_uses.records[use_id - 1];
      use_id = use->next_outgoing;
      if (loom_verify_definition_ref_is_visible(
              state, op, vtable, use->value_id, allows_local_definitions)) {
        continue;
      }
      loom_verify_emit_attribute_ref_not_visible(state, op, vtable, i,
                                                 use->value_id);
      if (loom_verify_at_error_limit(state)) {
        return;
      }
    }
  }
}
