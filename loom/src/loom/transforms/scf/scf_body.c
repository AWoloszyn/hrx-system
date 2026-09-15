// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/transforms/scf/scf_body.h"

#include "loom/ir/attribute.h"
#include "loom/ir/context.h"
#include "loom/ir/encoding.h"
#include "loom/ir/module.h"
#include "loom/ir/types.h"
#include "loom/ops/op_defs.h"

typedef struct loom_scf_body_builder_t {
  // Module providing maintained type uses and interned attribute payloads.
  const loom_module_t* module;
  // Block defining the source iteration's local values.
  const loom_block_t* block;
  // Current operation, whose result-type self references need no dependency.
  const loom_op_t* op;
  // Destination plan populated in authored order.
  loom_scf_body_t* body;
  // Arena owning all plan storage.
  iree_arena_allocator_t* arena;
  // Allocated reference slots in the packed dependency array.
  iree_host_size_t reference_capacity;
} loom_scf_body_builder_t;

static iree_status_t loom_scf_body_append_reference(loom_value_id_t value_id,
                                                    void* user_data) {
  loom_scf_body_builder_t* builder = (loom_scf_body_builder_t*)user_data;
  const loom_value_t* value = loom_module_value(builder->module, value_id);
  bool allow_identity_mapping = false;
  if (loom_value_is_block_arg(value)) {
    if (loom_value_def_block(value) != builder->block) return iree_ok_status();
  } else {
    const loom_op_t* definition = loom_value_def_op(value);
    if (!definition || definition == builder->op) return iree_ok_status();
    if (!definition->parent_block) {
      allow_identity_mapping = true;
    } else if (definition->parent_block != builder->block) {
      return iree_ok_status();
    }
  }
  loom_scf_body_t* body = builder->body;
  if (body->reference_count == builder->reference_capacity) {
    IREE_RETURN_IF_ERROR(iree_arena_grow_array(
        builder->arena, body->reference_count, body->reference_count + 1,
        sizeof(*body->references), &builder->reference_capacity,
        (void**)&body->references));
  }
  body->references[body->reference_count++] = (loom_scf_body_reference_t){
      .value_id = value_id,
      .allow_identity_mapping = allow_identity_mapping,
  };
  return iree_ok_status();
}

static iree_status_t loom_scf_body_append_attribute(
    loom_scf_body_builder_t* builder, const loom_attribute_t* attribute) {
  switch ((loom_attr_kind_t)attribute->kind) {
    case LOOM_ATTR_TYPE:
      return loom_type_walk_value_refs(
          builder->module, builder->module->types.entries[attribute->type_id],
          loom_scf_body_append_reference, builder);
    case LOOM_ATTR_PREDICATE_LIST:
      for (uint16_t i = 0; i < attribute->count; ++i) {
        const loom_predicate_t* predicate = &attribute->predicate_list[i];
        for (uint8_t j = 0; j < predicate->arg_count; ++j) {
          if (predicate->arg_tags[j] != LOOM_PRED_ARG_VALUE) continue;
          IREE_RETURN_IF_ERROR(loom_scf_body_append_reference(
              (loom_value_id_t)predicate->args[j], builder));
        }
      }
      break;
    case LOOM_ATTR_DICT:
      for (uint16_t i = 0; i < attribute->count; ++i) {
        IREE_RETURN_IF_ERROR(loom_scf_body_append_attribute(
            builder, &attribute->dict_entries[i].value));
      }
      break;
    case LOOM_ATTR_PARAMETERIZED:
      for (uint16_t i = 0; i < attribute->count; ++i) {
        IREE_RETURN_IF_ERROR(loom_scf_body_append_attribute(
            builder, &attribute->parameterized_slots[i]));
      }
      break;
    case LOOM_ATTR_PARAMETERIZED_ARRAY:
      for (uint16_t i = 0; i < attribute->count; ++i) {
        IREE_RETURN_IF_ERROR(loom_scf_body_append_attribute(
            builder, &attribute->parameterized_array[i]));
      }
      break;
    case LOOM_ATTR_ENCODING: {
      const loom_encoding_t* encoding = loom_module_encoding(
          builder->module, (uint16_t)attribute->encoding_id);
      for (uint8_t i = 0; i < encoding->attribute_count; ++i) {
        IREE_RETURN_IF_ERROR(loom_scf_body_append_attribute(
            builder, &encoding->attributes[i].value));
      }
      break;
    }
    default:
      break;
  }
  return iree_ok_status();
}

static loom_scf_body_effect_flags_t loom_scf_body_operation_effects(
    const loom_module_t* module, const loom_op_t* op) {
  loom_trait_flags_t traits = loom_op_effective_traits(module, op);
  loom_scf_body_effect_flags_t flags = 0;
  if (loom_traits_may_read(traits)) flags |= LOOM_SCF_BODY_EFFECT_READ;
  if (loom_traits_may_write(traits)) flags |= LOOM_SCF_BODY_EFFECT_WRITE;
  if (iree_any_bit_set(
          traits, LOOM_TRAIT_NON_DETERMINISTIC | LOOM_TRAIT_UNKNOWN_EFFECTS |
                      LOOM_TRAIT_HINT | LOOM_TRAIT_POISON_BOUNDARY |
                      LOOM_TRAIT_CONVERGENT)) {
    flags |= LOOM_SCF_BODY_EFFECT_ORDERED;
  }
  if (flags == 0 && !iree_any_bit_set(traits, LOOM_TRAIT_PURE)) {
    flags |= LOOM_SCF_BODY_EFFECT_ORDERED;
  }
  return flags;
}

static iree_status_t loom_scf_body_capture_operation(
    loom_scf_body_builder_t* builder, const loom_op_t* op,
    loom_scf_body_operation_t* out_operation) {
  builder->op = op;
  *out_operation = (loom_scf_body_operation_t){
      .op = op,
      .reference_begin = builder->body->reference_count,
      .effects = loom_scf_body_operation_effects(builder->module, op),
  };
  const loom_value_id_t* operands = loom_op_const_operands(op);
  for (uint16_t i = 0; i < op->operand_count; ++i) {
    IREE_RETURN_IF_ERROR(loom_scf_body_append_reference(operands[i], builder));
  }
  const loom_value_id_t* results = loom_op_const_results(op);
  for (uint16_t i = 0; i < op->result_count; ++i) {
    for (loom_type_use_id_t use_id = loom_module_value_first_outgoing_type_use(
             builder->module, results[i]);
         use_id != LOOM_TYPE_USE_ID_INVALID;) {
      const loom_type_use_t* use = &builder->module->type_uses.records[use_id];
      IREE_RETURN_IF_ERROR(
          loom_scf_body_append_reference(use->referenced_value_id, builder));
      use_id = use->next_outgoing_use_id;
    }
  }
  const loom_attribute_t* attributes = loom_op_const_attrs(op);
  for (uint8_t i = 0; i < op->attribute_count; ++i) {
    IREE_RETURN_IF_ERROR(
        loom_scf_body_append_attribute(builder, &attributes[i]));
  }
  out_operation->reference_count =
      builder->body->reference_count - out_operation->reference_begin;
  return iree_ok_status();
}

iree_status_t loom_scf_body_build(const loom_module_t* module,
                                  const loom_block_t* block,
                                  iree_arena_allocator_t* arena,
                                  loom_scf_body_t* out_body,
                                  const loom_op_t** out_unstructured_op) {
  *out_body = (loom_scf_body_t){0};
  *out_unstructured_op = NULL;
  loom_scf_body_builder_t builder = {
      .module = module,
      .block = block,
      .body = out_body,
      .arena = arena,
  };
  iree_host_size_t operation_capacity = 0;
  for (const loom_op_t* op = block->first_op; op != block->last_op;
       op = op->next_op) {
    if (iree_any_bit_set(op->flags, LOOM_OP_FLAG_DEAD)) continue;
    const loom_op_vtable_t* vtable = loom_op_vtable(module, op);
    if (op->region_count != 0 || op->successor_count != 0 ||
        (vtable && vtable->region_count != 0)) {
      *out_unstructured_op = op;
      return iree_ok_status();
    }
    if (out_body->count == UINT32_MAX) {
      return iree_make_status(IREE_STATUS_RESOURCE_EXHAUSTED,
                              "SCF body operation count exceeds uint32");
    }
    if (out_body->count == operation_capacity) {
      IREE_RETURN_IF_ERROR(iree_arena_grow_array(
          arena, out_body->count, (iree_host_size_t)out_body->count + 1,
          sizeof(*out_body->operations), &operation_capacity,
          (void**)&out_body->operations));
    }
    IREE_RETURN_IF_ERROR(loom_scf_body_capture_operation(
        &builder, op, &out_body->operations[out_body->count]));
    ++out_body->count;
  }
  return loom_scf_body_capture_operation(&builder, block->last_op,
                                         &out_body->terminator);
}
