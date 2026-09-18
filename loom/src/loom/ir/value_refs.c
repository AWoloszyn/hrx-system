// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/ir/value_refs.h"

#include <string.h>

#include "loom/ir/context.h"
#include "loom/ir/module.h"

// Consume one retained bitmap at a time. The callback cannot change membership,
// so the current batch remains local across calls instead of round-tripping
// each provider through the address-exposed cursor.
static iree_status_t loom_value_walk_dependencies(
    loom_type_use_iterator_t* dependencies,
    loom_type_value_ref_callback_t callback, void* user_data) {
  while (loom_type_dependencies_advance(dependencies)) {
    const loom_value_id_t base = dependencies->base;
    uint64_t members = dependencies->members;
    while (members) {
      const loom_value_id_t provider =
          base + iree_math_count_trailing_zeros_u64(members);
      members &= members - 1;
      IREE_RETURN_IF_ERROR(callback(provider, user_data));
    }
  }
  return iree_ok_status();
}

static iree_status_t loom_value_walk_outgoing_type_refs(
    const loom_module_t* module, loom_value_id_t value_id,
    loom_type_value_ref_callback_t callback, void* user_data) {
  loom_type_use_iterator_t dependencies;
  loom_module_value_type_dependencies(module, value_id, &dependencies);
  return loom_value_walk_dependencies(&dependencies, callback, user_data);
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

  const uint32_t* attribute_owners = loom_op_attribute_owners(op);
  for (uint8_t i = 0; i < op->attribute_count; ++i) {
    if (!attribute_owners[i]) {
      continue;
    }
    loom_type_use_iterator_t dependencies;
    loom_attribute_dependencies_begin(&module->type_uses, op, i, &dependencies);
    IREE_RETURN_IF_ERROR(
        loom_value_walk_dependencies(&dependencies, callback, user_data));
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

// The structural walker owns aggregate validation. TYPE consumers choose either
// occurrence traversal or retained membership without duplicating that
// boundary.
typedef struct loom_attribute_reference_visitor_t {
  // Consumes one canonical TYPE field.
  iree_status_t (*type)(loom_type_id_t type_id, void* user_data);
  // Consumes one immediate predicate value.
  loom_type_value_ref_callback_t value;
  // Borrowed callback state for this walk.
  void* user_data;
} loom_attribute_reference_visitor_t;

static iree_status_t loom_module_walk_attribute_value_refs_impl(
    const loom_module_t* module, loom_attribute_t attr, uint8_t depth,
    const loom_attribute_reference_visitor_t* visitor) {
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
      return visitor->type(attr.type_id, visitor->user_data);

    case LOOM_ATTR_PREDICATE_LIST:
      for (uint16_t i = 0; i < attr.count; ++i) {
        const loom_predicate_t* predicate = &attr.predicate_list[i];
        for (uint8_t j = 0; j < predicate->arg_count; ++j) {
          if (predicate->arg_tags[j] == LOOM_PRED_ARG_VALUE) {
            IREE_RETURN_IF_ERROR(visitor->value(
                (loom_value_id_t)predicate->args[j], visitor->user_data));
          }
        }
      }
      return iree_ok_status();

    case LOOM_ATTR_DICT:
    case LOOM_ATTR_PARAMETERIZED:
    case LOOM_ATTR_PARAMETERIZED_ARRAY:
      if (depth >= LOOM_ATTR_AGGREGATE_MAX_NESTING_DEPTH) {
        return iree_make_status(
            IREE_STATUS_INVALID_ARGUMENT,
            "aggregate attribute nesting exceeds max depth %u",
            (unsigned)LOOM_ATTR_AGGREGATE_MAX_NESTING_DEPTH);
      }
      for (uint16_t i = 0; i < attr.count; ++i) {
        const loom_attribute_t* child = NULL;
        switch ((loom_attr_kind_t)attr.kind) {
          case LOOM_ATTR_DICT:
            child = &attr.dict_entries[i].value;
            break;
          case LOOM_ATTR_PARAMETERIZED:
            child = &attr.parameterized_slots[i];
            break;
          default:
            child = &attr.parameterized_array[i];
            break;
        }
        IREE_RETURN_IF_ERROR(loom_module_walk_attribute_value_refs_impl(
            module, *child, (uint8_t)(depth + 1), visitor));
      }
      return iree_ok_status();

    default:
      return iree_ok_status();
  }
}

typedef struct loom_attribute_occurrence_walk_t {
  // Module resolving canonical TYPE identities.
  const loom_module_t* module;
  // Receives ordered occurrences, including duplicates.
  loom_type_value_ref_callback_t callback;
  // Borrowed caller state.
  void* user_data;
} loom_attribute_occurrence_walk_t;

static iree_status_t loom_attribute_walk_type_occurrences(
    loom_type_id_t type_id, void* user_data) {
  const loom_attribute_occurrence_walk_t* walk = user_data;
  return loom_type_walk_value_refs(walk->module,
                                   walk->module->types.entries[type_id],
                                   walk->callback, walk->user_data);
}

static iree_status_t loom_attribute_walk_value_occurrence(
    loom_value_id_t value_id, void* user_data) {
  const loom_attribute_occurrence_walk_t* walk = user_data;
  return walk->callback(value_id, walk->user_data);
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
  loom_attribute_occurrence_walk_t walk = {module, callback, user_data};
  const loom_attribute_reference_visitor_t visitor = {
      .type = loom_attribute_walk_type_occurrences,
      .value = loom_attribute_walk_value_occurrence,
      .user_data = &walk,
  };
  return loom_module_walk_attribute_value_refs_impl(module, attr, 0, &visitor);
}

//===----------------------------------------------------------------------===//
// Retained attribute dependency ownership
//===----------------------------------------------------------------------===//

typedef struct loom_attribute_dependency_build_t {
  // Module owning canonical membership.
  loom_module_t* module;
  // Union of retained TYPE sets and immediate predicate references.
  loom_type_dependency_id_t root;
} loom_attribute_dependency_build_t;

static iree_status_t loom_attribute_collect_type_dependencies(
    loom_type_id_t type_id, void* user_data) {
  loom_attribute_dependency_build_t* build = user_data;
  return loom_type_dependencies_union(
      &build->module->type_uses, build->root,
      build->module->types.dependencies[type_id], &build->root);
}

static iree_status_t loom_attribute_collect_value_dependency(
    loom_value_id_t value_id, void* user_data) {
  loom_attribute_dependency_build_t* build = user_data;
  return loom_type_dependencies_add(&build->module->type_uses, build->root,
                                    value_id, &build->root);
}

void loom_module_drop_attribute_uses(loom_module_t* module, loom_op_t* op,
                                     uint8_t attribute_index) {
  loom_attribute_dependencies_drop(&module->type_uses, op, attribute_index);
}

void loom_module_drop_op_attribute_uses(loom_module_t* module, loom_op_t* op) {
  for (uint8_t i = 0; i < op->attribute_count; ++i) {
    loom_module_drop_attribute_uses(module, op, i);
  }
}

void loom_module_reset_attribute_uses(loom_module_t* module) {
  loom_attribute_dependencies_reset(&module->type_uses);
}

iree_status_t loom_module_set_op_attribute(loom_module_t* module, loom_op_t* op,
                                           uint8_t attribute_index,
                                           loom_attribute_t attribute) {
  loom_attribute_dependency_build_t build = {.module = module};
  const loom_attribute_reference_visitor_t visitor = {
      .type = loom_attribute_collect_type_dependencies,
      .value = loom_attribute_collect_value_dependency,
      .user_data = &build,
  };
  IREE_RETURN_IF_ERROR(loom_module_walk_attribute_value_refs_impl(
      module, attribute, 0, &visitor));
  loom_type_dependency_assignment_t assignment;
  IREE_RETURN_IF_ERROR(loom_attribute_dependencies_prepare(
      &module->type_uses, op, attribute_index, build.root, &assignment));
  loom_op_attrs(op)[attribute_index] = attribute;
  loom_attribute_dependencies_commit(&module->type_uses, op, attribute_index,
                                     &assignment);
  return iree_ok_status();
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
        // formerly reference-carrying slot before refreshing its ownership.
        if (loom_op_attribute_owners(op)[i]) {
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

  return loom_module_set_op_attribute(module, op, attribute_index, attribute);
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
