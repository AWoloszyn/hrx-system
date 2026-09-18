// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/ir/value_refs.h"

#include <string.h>

#include "loom/ir/context.h"
#include "loom/ir/module.h"

static iree_status_t loom_value_walk_outgoing_type_refs(
    const loom_module_t* module, loom_value_id_t value_id,
    loom_type_value_ref_callback_t callback, void* user_data) {
  loom_type_use_iterator_t dependencies;
  loom_module_value_type_dependencies(module, value_id, &dependencies);
  for (loom_value_id_t provider = loom_type_dependencies_next(&dependencies);
       provider != LOOM_VALUE_ID_INVALID;
       provider = loom_type_dependencies_next(&dependencies)) {
    IREE_RETURN_IF_ERROR(callback(provider, user_data));
  }
  return iree_ok_status();
}

iree_status_t loom_op_walk_subtree_value_refs(
    const loom_module_t* module, const loom_op_t* op,
    loom_type_value_ref_callback_t callback, void* user_data) {
  const loom_value_id_t* operands = loom_op_const_operands(op);
  // Only symbol signatures define operand values. Ordinary users need no
  // interface metadata lookup on this per-operation path.
  if (op->operand_count > 0 &&
      iree_any_bit_set(op->traits, LOOM_TRAIT_SYMBOL_DEFINE) &&
      loom_op_vtable_owns_operands(loom_op_vtable(module, op))) {
    for (uint16_t i = 0; i < op->operand_count; ++i) {
      IREE_RETURN_IF_ERROR(loom_value_walk_outgoing_type_refs(
          module, operands[i], callback, user_data));
    }
  } else {
    for (uint16_t i = 0; i < op->operand_count; ++i) {
      if (operands[i] != LOOM_VALUE_ID_INVALID) {
        IREE_RETURN_IF_ERROR(callback(operands[i], user_data));
      }
    }
  }

  const loom_value_id_t* results = loom_op_const_results(op);
  for (uint16_t i = 0; i < op->result_count; ++i) {
    IREE_RETURN_IF_ERROR(loom_value_walk_outgoing_type_refs(
        module, results[i], callback, user_data));
  }

  const loom_attribute_use_id_t* heads = loom_op_attribute_use_heads(op);
  for (uint8_t i = 0; i < op->attribute_count; ++i) {
    for (loom_attribute_use_id_t use_id = heads[i]; use_id;) {
      const loom_attribute_use_t* use =
          &module->attribute_uses.records[use_id - 1];
      IREE_RETURN_IF_ERROR(callback(use->value_id, user_data));
      use_id = use->next_outgoing;
    }
  }

  loom_region_t** regions = loom_op_regions(op);
  for (uint8_t i = 0; i < op->region_count; ++i) {
    loom_region_t* region = regions[i];
    if (!region) {
      continue;
    }
    loom_block_t* block = NULL;
    loom_region_for_each_block(region, block) {
      for (uint16_t arg_index = 0; arg_index < block->arg_count; ++arg_index) {
        loom_value_id_t arg_id = loom_block_arg_id(block, arg_index);
        IREE_RETURN_IF_ERROR(loom_value_walk_outgoing_type_refs(
            module, arg_id, callback, user_data));
      }
      loom_op_t* child_op = NULL;
      loom_block_for_each_op(block, child_op) {
        IREE_RETURN_IF_ERROR(loom_op_walk_subtree_value_refs(
            module, child_op, callback, user_data));
      }
    }
  }

  return iree_ok_status();
}

static iree_status_t loom_module_walk_attribute_value_refs_impl(
    const loom_module_t* module, loom_attribute_t attr, uint8_t depth,
    loom_type_value_ref_callback_t callback,
    loom_type_value_ref_callback_t predicate_callback, void* user_data) {
  switch ((loom_attr_kind_t)attr.kind) {
    case LOOM_ATTR_TYPE:
      if (attr.type_id == LOOM_TYPE_ID_INVALID ||
          attr.type_id >= module->types.count) {
        return iree_make_status(
            IREE_STATUS_INVALID_ARGUMENT,
            "type attribute id %u is out of range (module has %" PRIhsz
            " types)",
            (unsigned)attr.type_id, module->types.count);
      }
      return loom_type_walk_value_refs(
          module, module->types.entries[attr.type_id], callback, user_data);

    case LOOM_ATTR_PREDICATE_LIST:
      for (uint16_t i = 0; i < attr.count; ++i) {
        const loom_predicate_t* predicate = &attr.predicate_list[i];
        for (uint8_t j = 0; j < predicate->arg_count; ++j) {
          if (predicate->arg_tags[j] != LOOM_PRED_ARG_VALUE) {
            continue;
          }
          IREE_RETURN_IF_ERROR(predicate_callback(
              (loom_value_id_t)predicate->args[j], user_data));
        }
      }
      return iree_ok_status();

    case LOOM_ATTR_DICT:
      if (depth >= LOOM_ATTR_AGGREGATE_MAX_NESTING_DEPTH) {
        return iree_make_status(
            IREE_STATUS_INVALID_ARGUMENT,
            "aggregate attribute nesting exceeds max depth %u",
            (unsigned)LOOM_ATTR_AGGREGATE_MAX_NESTING_DEPTH);
      }
      for (uint16_t i = 0; i < attr.count; ++i) {
        IREE_RETURN_IF_ERROR(loom_module_walk_attribute_value_refs_impl(
            module, attr.dict_entries[i].value, (uint8_t)(depth + 1), callback,
            predicate_callback, user_data));
      }
      return iree_ok_status();

    case LOOM_ATTR_PARAMETERIZED:
      if (depth >= LOOM_ATTR_AGGREGATE_MAX_NESTING_DEPTH) {
        return iree_make_status(
            IREE_STATUS_INVALID_ARGUMENT,
            "aggregate attribute nesting exceeds max depth %u",
            (unsigned)LOOM_ATTR_AGGREGATE_MAX_NESTING_DEPTH);
      }
      for (uint16_t i = 0; i < attr.count; ++i) {
        IREE_RETURN_IF_ERROR(loom_module_walk_attribute_value_refs_impl(
            module, attr.parameterized_slots[i], (uint8_t)(depth + 1), callback,
            predicate_callback, user_data));
      }
      return iree_ok_status();

    case LOOM_ATTR_PARAMETERIZED_ARRAY:
      if (depth >= LOOM_ATTR_AGGREGATE_MAX_NESTING_DEPTH) {
        return iree_make_status(
            IREE_STATUS_INVALID_ARGUMENT,
            "aggregate attribute nesting exceeds max depth %u",
            (unsigned)LOOM_ATTR_AGGREGATE_MAX_NESTING_DEPTH);
      }
      for (uint16_t i = 0; i < attr.count; ++i) {
        IREE_RETURN_IF_ERROR(loom_module_walk_attribute_value_refs_impl(
            module, attr.parameterized_array[i], (uint8_t)(depth + 1), callback,
            predicate_callback, user_data));
      }
      return iree_ok_status();

    default:
      return iree_ok_status();
  }
}

iree_status_t loom_module_walk_attribute_value_refs(
    const loom_module_t* module, loom_attribute_t attr,
    loom_type_value_ref_callback_t callback, void* user_data) {
  if (!module) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT, "module is NULL");
  }
  if (!callback) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "value reference callback is NULL");
  }
  return loom_module_walk_attribute_value_refs_impl(
      module, attr, /*depth=*/0, callback, callback, user_data);
}

//===----------------------------------------------------------------------===//
// Exact incoming and outgoing use lists
//===----------------------------------------------------------------------===//

static loom_attribute_use_id_t* loom_attribute_use_incoming_head(
    loom_module_t* module, const loom_attribute_use_t* use) {
  loom_value_segment_t* segment =
      loom_value_table_segment_for_id(&module->values, use->value_id);
  loom_value_attribute_use_heads_t* heads =
      &segment->attribute_use_heads[use->value_id & LOOM_VALUE_SEGMENT_MASK];
  return use->is_predicate ? &heads->predicate : &heads->type;
}

static void loom_attribute_use_link(loom_module_t* module,
                                    loom_attribute_use_id_t id) {
  loom_attribute_use_table_t* table = &module->attribute_uses;
  loom_attribute_use_t* use = &table->records[id - 1];
  loom_attribute_use_id_t* head = loom_attribute_use_incoming_head(module, use);
  use->previous_incoming = 0;
  use->next_incoming = *head;
  if (*head) {
    table->records[*head - 1].previous_incoming = id;
  }
  *head = id;
  loom_module_value(module, use->value_id)->flags |=
      LOOM_VALUE_FLAG_ATTRIBUTE_USES;
}

static void loom_attribute_use_unlink(loom_module_t* module,
                                      loom_attribute_use_id_t id) {
  loom_attribute_use_table_t* table = &module->attribute_uses;
  const loom_attribute_use_t* use = &table->records[id - 1];
  if (use->previous_incoming) {
    table->records[use->previous_incoming - 1].next_incoming =
        use->next_incoming;
  } else {
    *loom_attribute_use_incoming_head(module, use) = use->next_incoming;
  }
  if (use->next_incoming) {
    table->records[use->next_incoming - 1].previous_incoming =
        use->previous_incoming;
  }
  if (!loom_module_value_first_attribute_use(module, use->value_id)) {
    loom_module_value(module, use->value_id)->flags &=
        ~LOOM_VALUE_FLAG_ATTRIBUTE_USES;
  }
}

static iree_status_t loom_attribute_use_allocate(
    loom_module_t* module, loom_attribute_use_id_t* out_id) {
  loom_attribute_use_table_t* table = &module->attribute_uses;
  if (table->first_free) {
    *out_id = table->first_free;
    table->first_free = table->records[*out_id - 1].next_outgoing;
    return iree_ok_status();
  }
  if (table->count == table->capacity) {
    if (table->capacity == UINT32_MAX) {
      return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                              "attribute use count exceeds maximum");
    }
    uint32_t capacity = 32;
    if (table->capacity > UINT32_MAX / 2) {
      capacity = UINT32_MAX;
    } else if (table->capacity) {
      capacity = table->capacity * 2;
    }
    loom_attribute_use_t* records = NULL;
    IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
        &module->arena, capacity, sizeof(*records), (void**)&records));
    if (table->count) {
      memcpy(records, table->records, table->count * sizeof(*records));
    }
    table->records = records;
    table->capacity = capacity;
  }
  *out_id = ++table->count;
  return iree_ok_status();
}

static void loom_attribute_use_recycle(loom_attribute_use_table_t* table,
                                       loom_attribute_use_id_t id) {
  loom_attribute_use_t* use = &table->records[id - 1];
  use->op = NULL;
  use->next_outgoing = table->first_free;
  table->first_free = id;
}

void loom_module_drop_attribute_uses(loom_module_t* module, loom_op_t* op,
                                     uint8_t attribute_index) {
  loom_attribute_use_table_t* table = &module->attribute_uses;
  loom_attribute_use_id_t id = loom_op_attribute_use_heads(op)[attribute_index];
  loom_op_attribute_use_heads(op)[attribute_index] = 0;
  while (id) {
    const loom_attribute_use_id_t next = table->records[id - 1].next_outgoing;
    loom_attribute_use_unlink(module, id);
    loom_attribute_use_recycle(table, id);
    id = next;
  }
}

void loom_module_drop_op_attribute_uses(loom_module_t* module, loom_op_t* op) {
  for (uint8_t i = 0; i < op->attribute_count; ++i) {
    loom_module_drop_attribute_uses(module, op, i);
  }
}

void loom_module_reset_attribute_uses(loom_module_t* module) {
  loom_attribute_use_table_t* table = &module->attribute_uses;
  for (uint32_t i = 0; i < table->count; ++i) {
    const loom_attribute_use_t* use = &table->records[i];
    if (!use->op) {
      continue;
    }
    *loom_attribute_use_incoming_head(module, use) = 0;
    loom_op_attribute_use_heads(use->op)[use->attribute_index] = 0;
    loom_module_value(module, use->value_id)->flags &=
        ~LOOM_VALUE_FLAG_ATTRIBUTE_USES;
  }
  table->count = 0;
  table->first_free = 0;
}

// New records are not linked to values until the entire payload walk succeeds.
typedef struct loom_attribute_use_build_t {
  // Module owning the replacement's storage and value identities.
  loom_module_t* module;
  // Stable owning operation.
  loom_op_t* op;
  // New outgoing list, unpublished until construction succeeds.
  loom_attribute_use_id_t first;
  // Ordinal of the attribute being replaced.
  uint8_t attribute_index;
} loom_attribute_use_build_t;

static iree_status_t loom_attribute_use_build_append(
    loom_attribute_use_build_t* build, loom_value_id_t value_id,
    bool is_predicate) {
  if (value_id >= build->module->values.count) {
    return iree_ok_status();
  }
  loom_attribute_use_id_t id = 0;
  IREE_RETURN_IF_ERROR(loom_attribute_use_allocate(build->module, &id));
  build->module->attribute_uses.records[id - 1] = (loom_attribute_use_t){
      .op = build->op,
      .value_id = value_id,
      .next_outgoing = build->first,
      .attribute_index = build->attribute_index,
      .is_predicate = is_predicate,
  };
  build->first = id;
  return iree_ok_status();
}

static iree_status_t loom_attribute_use_build_type(loom_value_id_t value_id,
                                                   void* user_data) {
  return loom_attribute_use_build_append(user_data, value_id, false);
}

static iree_status_t loom_attribute_use_build_predicate(
    loom_value_id_t value_id, void* user_data) {
  return loom_attribute_use_build_append(user_data, value_id, true);
}

iree_status_t loom_module_set_op_attribute(loom_module_t* module, loom_op_t* op,
                                           uint8_t attribute_index,
                                           loom_attribute_t attribute) {
  loom_attribute_use_build_t build = {
      .module = module,
      .op = op,
      .attribute_index = attribute_index,
  };
  iree_status_t status = loom_module_walk_attribute_value_refs_impl(
      module, attribute, 0, loom_attribute_use_build_type,
      loom_attribute_use_build_predicate, &build);
  loom_attribute_use_table_t* table = &module->attribute_uses;
  if (iree_status_is_ok(status)) {
    loom_module_drop_attribute_uses(module, op, attribute_index);
    loom_op_attrs(op)[attribute_index] = attribute;
    loom_op_attribute_use_heads(op)[attribute_index] = build.first;
    for (loom_attribute_use_id_t id = build.first; id;
         id = table->records[id - 1].next_outgoing) {
      loom_attribute_use_link(module, id);
    }
  } else {
    loom_attribute_use_id_t id = build.first;
    while (id) {
      const loom_attribute_use_id_t next = table->records[id - 1].next_outgoing;
      loom_attribute_use_recycle(table, id);
      id = next;
    }
  }
  return status;
}

iree_status_t loom_module_refresh_op_attribute_uses(loom_module_t* module,
                                                    loom_op_t* op) {
  iree_status_t status = iree_ok_status();
  for (uint8_t i = 0; i < op->attribute_count && iree_status_is_ok(status);
       ++i) {
    loom_attribute_t attribute = loom_op_attrs(op)[i];
    switch ((loom_attr_kind_t)attribute.kind) {
      case LOOM_ATTR_TYPE:
      case LOOM_ATTR_PREDICATE_LIST:
      case LOOM_ATTR_DICT:
      case LOOM_ATTR_PARAMETERIZED:
      case LOOM_ATTR_PARAMETERIZED_ARRAY:
        status = loom_module_set_op_attribute(module, op, i, attribute);
        break;
      default:
        // Scalar attributes carry no references. A bulk reader can replace a
        // formerly reference-carrying slot before refreshing its use records.
        if (loom_op_attribute_use_heads(op)[i]) {
          loom_module_drop_attribute_uses(module, op, i);
        }
        break;
    }
  }
  return status;
}

iree_status_t loom_value_replacement_apply_attribute(
    loom_value_replacement_t* replacement, loom_op_t* op,
    uint8_t attribute_index) {
  loom_module_t* module = replacement->module;
  loom_attribute_t attribute = {0};
  bool changed = false;
  IREE_RETURN_IF_ERROR(loom_value_replacement_attribute(
      replacement, loom_op_attrs(op)[attribute_index], &attribute, &changed));
  IREE_ASSERT(changed, "attribute use owner must contain the referenced value");

  // Identity substitution and structural interning preserve reference
  // multiplicity and type/predicate classification. Retain the owner's list;
  // only edges referencing old_id change their incoming list. All fallible
  // payload construction has completed before either representation changes.
  loom_attribute_use_table_t* table = &module->attribute_uses;
  for (loom_attribute_use_id_t id =
           loom_op_attribute_use_heads(op)[attribute_index];
       id; id = table->records[id - 1].next_outgoing) {
    loom_attribute_use_t* use = &table->records[id - 1];
    if (use->value_id != replacement->old_id) {
      continue;
    }
    loom_attribute_use_unlink(module, id);
    use->value_id = replacement->new_id;
    loom_attribute_use_link(module, id);
  }
  loom_op_attrs(op)[attribute_index] = attribute;
  return iree_ok_status();
}

//===----------------------------------------------------------------------===//
// Type-use replacement
//===----------------------------------------------------------------------===//

iree_status_t loom_value_replacement_apply_types(
    loom_value_replacement_t* replacement) {
  loom_module_t* module = replacement->module;
  // Mutation invalidates cursors. Each successful assignment removes this
  // carrier's old membership, so restarting reaches an unprocessed owner.
  while (loom_module_value_has_type_uses(module, replacement->old_id)) {
    loom_type_use_iterator_t type_users;
    loom_module_value_type_users(module, replacement->old_id, &type_users);
    const loom_value_id_t carrier = loom_type_users_next(&type_users);
    loom_type_t result;
    bool changed = false;
    IREE_RETURN_IF_ERROR(loom_value_replacement_type(
        replacement, loom_module_value_type(module, carrier), &result,
        &changed));
    IREE_ASSERT(changed && "retained type dependencies must match the payload");
    IREE_RETURN_IF_ERROR(loom_module_set_value_type(module, carrier, result));
  }
  return iree_ok_status();
}

iree_status_t loom_module_replace_value_type_uses(loom_module_t* module,
                                                  loom_value_id_t old_id,
                                                  loom_value_id_t new_id) {
  if (old_id == new_id) {
    return iree_ok_status();
  }
  if (old_id >= module->values.count || new_id >= module->values.count) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "cannot replace type references from %%%u to %%%u in a module with "
        "%" PRIhsz " values",
        (unsigned)old_id, (unsigned)new_id, module->values.count);
  }
  loom_value_replacement_t replacement;
  loom_value_replacement_initialize(module, old_id, new_id, &replacement);
  iree_status_t status = loom_value_replacement_apply_types(&replacement);
  loom_value_replacement_deinitialize(&replacement);
  return status;
}
