// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/ir/value_replacement.h"

#include <string.h>

#include "iree/base/internal/math.h"
#include "loom/ir/module.h"
#include "loom/ir/type_identity.h"

//===----------------------------------------------------------------------===//
// Completed canonical substitutions
//===----------------------------------------------------------------------===//

static loom_value_replacement_memo_t* loom_replacement_memo(uintptr_t edge) {
  return (loom_value_replacement_memo_t*)(edge & ~(uintptr_t)1);
}

static loom_type_id_t loom_replacement_find(uintptr_t edge,
                                            loom_type_id_t source) {
  if (!edge) {
    return LOOM_TYPE_ID_INVALID;
  }
  while (!(edge & 1)) {
    const loom_value_replacement_memo_t* branch = loom_replacement_memo(edge);
    edge = branch->edges[(source >> branch->bit) & 1];
  }
  const loom_value_replacement_memo_t* leaf = loom_replacement_memo(edge);
  return leaf->source == source ? leaf->result : LOOM_TYPE_ID_INVALID;
}

static iree_status_t loom_replacement_insert(
    loom_value_replacement_t* replacement, loom_type_id_t source,
    loom_type_id_t result) {
  loom_value_replacement_memo_t* entry = &replacement->first_result;
  if (replacement->memo) {
    IREE_RETURN_IF_ERROR(iree_arena_allocate(&replacement->scratch,
                                             sizeof(*entry), (void**)&entry));
  }
  *entry = (loom_value_replacement_memo_t){.source = source, .result = result};
  const uintptr_t leaf = (uintptr_t)entry | 1;
  if (!replacement->memo) {
    replacement->memo = leaf;
    return iree_ok_status();
  }
  uintptr_t existing = replacement->memo;
  while (!(existing & 1)) {
    const loom_value_replacement_memo_t* branch =
        loom_replacement_memo(existing);
    existing = branch->edges[(source >> branch->bit) & 1];
  }
  const uint32_t difference = source ^ loom_replacement_memo(existing)->source;
  // Acyclic canonical inputs complete before any later occurrence can request
  // the same source. Every insertion therefore introduces a distinct identity.
  IREE_ASSERT(difference != 0);
  entry->bit = 31 - iree_math_count_leading_zeros_u32(difference);
  uintptr_t* edge = &replacement->memo;
  while (!(*edge & 1)) {
    loom_value_replacement_memo_t* branch = loom_replacement_memo(*edge);
    if (branch->bit < entry->bit) {
      break;
    }
    edge = &branch->edges[(source >> branch->bit) & 1];
  }
  const uint32_t side = (source >> entry->bit) & 1;
  entry->edges[side] = leaf;
  entry->edges[side ^ 1] = *edge;
  *edge = (uintptr_t)entry;
  return iree_ok_status();
}

//===----------------------------------------------------------------------===//
// Immediate payloads
//===----------------------------------------------------------------------===//

static iree_status_t loom_replacement_leaf_type(
    loom_value_replacement_t* replacement, loom_type_t type,
    loom_type_t* out_type, bool* out_changed) {
  loom_type_t result = type;
  bool changed = false;
  if (loom_type_has_ssa_encoding(type) &&
      loom_type_encoding_value_id(type) == replacement->old_id) {
    if (replacement->new_id > UINT16_MAX) {
      return iree_make_status(
          IREE_STATUS_OUT_OF_RANGE,
          "cannot store value %%%u in a 16-bit SSA encoding reference",
          (unsigned)replacement->new_id);
    }
    result.encoding_id = (uint16_t)replacement->new_id;
    changed = true;
  }
  if (loom_type_is_shaped(type) || loom_type_is_pool(type)) {
    const uint8_t rank = loom_type_rank(type);
    if (loom_type_has_inline_dims(type)) {
      for (uint8_t i = 0; i < rank; ++i) {
        if (loom_dim_is_dynamic(result.dims[i]) &&
            loom_dim_value_id(result.dims[i]) == replacement->old_id) {
          result.dims[i] = loom_dim_pack_dynamic(replacement->new_id);
          changed = true;
        }
      }
    } else {
      const loom_overflow_dim_t* original =
          (const loom_overflow_dim_t*)(uintptr_t)type.dims[0];
      loom_overflow_dim_t* dimensions = NULL;
      for (uint8_t i = 0; i < rank; ++i) {
        if (!loom_dim_is_dynamic(original[i]) ||
            loom_dim_value_id(original[i]) != replacement->old_id) {
          continue;
        }
        if (!dimensions) {
          IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
              &replacement->scratch, rank, sizeof(*dimensions),
              (void**)&dimensions));
          memcpy(dimensions, original, rank * sizeof(*dimensions));
        }
        dimensions[i] = loom_dim_pack_dynamic(replacement->new_id);
      }
      if (dimensions) {
        result.dims[0] = (uint64_t)(uintptr_t)dimensions;
        result.dims[1] = 0;
        changed = true;
      }
    }
  }
  if (changed) {
    IREE_RETURN_IF_ERROR(
        loom_module_intern_type(replacement->module, result, &result));
  }
  *out_type = result;
  *out_changed = changed;
  return iree_ok_status();
}

static iree_status_t loom_replacement_predicates(
    loom_value_replacement_t* replacement, loom_attribute_t attribute,
    iree_arena_allocator_t* arena, loom_attribute_t* out_attribute,
    bool* out_changed) {
  loom_predicate_t* predicates = NULL;
  for (uint16_t i = 0; i < attribute.count; ++i) {
    const loom_predicate_t* original = &attribute.predicate_list[i];
    for (uint8_t j = 0; j < original->arg_count; ++j) {
      if (original->arg_tags[j] != LOOM_PRED_ARG_VALUE ||
          (loom_value_id_t)original->args[j] != replacement->old_id) {
        continue;
      }
      if (!predicates) {
        IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
            arena, attribute.count, sizeof(*predicates), (void**)&predicates));
        memcpy(predicates, attribute.predicate_list,
               attribute.count * sizeof(*predicates));
      }
      predicates[i].args[j] = replacement->new_id;
    }
  }
  *out_attribute = attribute;
  *out_changed = predicates != NULL;
  if (predicates) {
    out_attribute->predicate_list = predicates;
  }
  return iree_ok_status();
}

//===----------------------------------------------------------------------===//
// Explicit type and attribute continuations
//===----------------------------------------------------------------------===//

enum loom_replacement_frame_flag_bits_e {
  // The continuation owns a canonical type rather than an attribute.
  LOOM_REPLACEMENT_FRAME_FLAG_TYPE = 1u << 0,
  // At least one immediate child or field differs from the input.
  LOOM_REPLACEMENT_FRAME_FLAG_CHANGED = 1u << 1,
};
typedef uint8_t loom_replacement_frame_flags_t;

// Aggregate attributes alone have bounded nesting, but TYPE-valued attributes
// can lead into another parameterized type indefinitely. Both transitions use
// this stack; the C call stack never follows the canonical type graph.
typedef struct loom_value_replacement_frame_t {
  // Unfinished owner, or next reusable frame while not active.
  struct loom_value_replacement_frame_t* parent;
  // Stable output slot in a parent reconstruction or the invoking call.
  void* destination;
  // Continuation kind and accumulated child-change state.
  loom_replacement_frame_flags_t flags;
  // Number of immediate child slots.
  uint32_t count;
  // Next child to visit; a pending child's destination remains stable.
  uint32_t next;
  // Source identity and immutable payload for type continuations.
  struct {
    // Module-local canonical identity.
    loom_type_id_t id;
    // By-value copy unaffected by growth of the module's type table.
    loom_type_t value;
    // Borrowed structural children for functions, dialects and registers.
    const loom_type_t* children;
  } type;
  // Attribute input and ownership of changed aggregate payloads.
  struct {
    // Immutable source; TYPE continuations update its type ID on completion.
    loom_attribute_t value;
    // Scratch for type parameters, module storage for standalone attributes.
    iree_arena_allocator_t* arena;
  } attribute;
  // Reconstructed immediate children, indexed by the continuation kind.
  union {
    // Canonical structural child identities.
    loom_type_id_t* types;
    // Parameter slots or aggregate attribute values.
    loom_attribute_t* attributes;
  } children;
} loom_value_replacement_frame_t;

typedef struct loom_value_replacement_walk_t {
  // Fixed substitution and completed results shared by all invocations.
  loom_value_replacement_t* replacement;
  // Most recently suspended continuation, or NULL when complete.
  loom_value_replacement_frame_t* top;
  // Root continuation whose lifetime is the current invoking call.
  loom_value_replacement_frame_t root;
  // Inline structural children for ordinary short root signatures.
  loom_type_id_t root_children[16];
} loom_value_replacement_walk_t;

static iree_status_t loom_replacement_push(
    loom_value_replacement_walk_t* walk,
    loom_value_replacement_frame_t** out_frame) {
  loom_value_replacement_frame_t* frame = &walk->root;
  if (walk->top) {
    frame = walk->replacement->free_frames;
    if (frame) {
      walk->replacement->free_frames = frame->parent;
    } else {
      IREE_RETURN_IF_ERROR(iree_arena_allocate(&walk->replacement->scratch,
                                               sizeof(*frame), (void**)&frame));
    }
  }
  *frame = (loom_value_replacement_frame_t){.parent = walk->top};
  walk->top = frame;
  *out_frame = frame;
  return iree_ok_status();
}

static void loom_replacement_pop(loom_value_replacement_walk_t* walk) {
  loom_value_replacement_frame_t* frame = walk->top;
  walk->top = frame->parent;
  if (frame != &walk->root) {
    frame->parent = walk->replacement->free_frames;
    walk->replacement->free_frames = frame;
  }
}

static iree_status_t loom_replacement_request_type(
    loom_value_replacement_walk_t* walk, loom_type_id_t source,
    loom_type_id_t* destination) {
  loom_value_replacement_t* replacement = walk->replacement;
  loom_module_t* module = replacement->module;
  *destination = source;
  if (!loom_type_dependencies_contains(&module->type_uses,
                                       module->types.dependencies[source],
                                       replacement->old_id)) {
    return iree_ok_status();
  }
  const loom_type_id_t found = loom_replacement_find(replacement->memo, source);
  if (found != LOOM_TYPE_ID_INVALID) {
    *destination = found;
    if (walk->top) {
      walk->top->flags |= LOOM_REPLACEMENT_FRAME_FLAG_CHANGED;
    }
    return iree_ok_status();
  }
  loom_value_replacement_frame_t* frame = NULL;
  IREE_RETURN_IF_ERROR(loom_replacement_push(walk, &frame));
  frame->flags = LOOM_REPLACEMENT_FRAME_FLAG_TYPE;
  frame->type.id = source;
  frame->type.value = module->types.entries[source];
  frame->destination = destination;
  switch (loom_type_kind(frame->type.value)) {
    case LOOM_TYPE_FUNCTION: {
      const loom_func_type_data_t* data =
          loom_type_func_data(frame->type.value);
      frame->type.children = data->types;
      frame->count = (uint32_t)data->arg_count + data->result_count;
      break;
    }
    case LOOM_TYPE_DIALECT:
      frame->type.children = loom_type_dialect_params(frame->type.value);
      frame->count = loom_type_dialect_param_count(frame->type.value);
      break;
    case LOOM_TYPE_REGISTER:
      frame->type.children = loom_type_register_value_type(frame->type.value);
      frame->count = frame->type.children ? 1 : 0;
      break;
    case LOOM_TYPE_PARAMETERIZED:
      frame->count = loom_type_parameterized_parameter_count(frame->type.value);
      return iree_arena_allocate_array(&replacement->scratch, frame->count,
                                       sizeof(loom_attribute_t),
                                       (void**)&frame->children.attributes);
    default:
      break;
  }
  if (frame->count) {
    if (frame == &walk->root &&
        frame->count <= IREE_ARRAYSIZE(walk->root_children)) {
      frame->children.types = walk->root_children;
    } else {
      return iree_arena_allocate_array(&replacement->scratch, frame->count,
                                       sizeof(loom_type_id_t),
                                       (void**)&frame->children.types);
    }
  }
  return iree_ok_status();
}

static iree_status_t loom_replacement_request_attribute(
    loom_value_replacement_walk_t* walk, loom_attribute_t attribute,
    iree_arena_allocator_t* arena, loom_attribute_t* destination) {
  *destination = attribute;
  switch ((loom_attr_kind_t)attribute.kind) {
    case LOOM_ATTR_TYPE:
    case LOOM_ATTR_DICT:
    case LOOM_ATTR_PARAMETERIZED:
    case LOOM_ATTR_PARAMETERIZED_ARRAY:
      break;
    case LOOM_ATTR_PREDICATE_LIST: {
      bool changed = false;
      IREE_RETURN_IF_ERROR(loom_replacement_predicates(
          walk->replacement, attribute, arena, destination, &changed));
      if (walk->top && changed) {
        walk->top->flags |= LOOM_REPLACEMENT_FRAME_FLAG_CHANGED;
      }
      return iree_ok_status();
    }
    case LOOM_ATTR_ABSENT:
    case LOOM_ATTR_I64:
    case LOOM_ATTR_F64:
    case LOOM_ATTR_STRING:
    case LOOM_ATTR_BOOL:
    case LOOM_ATTR_ENUM:
    case LOOM_ATTR_SCOPED_ENUM:
    case LOOM_ATTR_SYMBOL:
    case LOOM_ATTR_SYMBOL_ARRAY:
    case LOOM_ATTR_SYMBOL_SET:
    case LOOM_ATTR_I64_ARRAY:
    case LOOM_ATTR_ENUM_ARRAY:
    case LOOM_ATTR_SIGNED_ENUM_SET:
    case LOOM_ATTR_ENCODING:
    case LOOM_ATTR_BYTES:
      return iree_ok_status();
    default:
      IREE_ASSERT_UNREACHABLE("attribute construction establishes its kind");
  }
  loom_value_replacement_frame_t* frame = NULL;
  IREE_RETURN_IF_ERROR(loom_replacement_push(walk, &frame));
  frame->destination = destination;
  frame->attribute.value = attribute;
  frame->attribute.arena = arena;
  frame->count = attribute.kind == LOOM_ATTR_TYPE ? 1 : attribute.count;
  if (attribute.kind != LOOM_ATTR_TYPE && attribute.count) {
    return iree_arena_allocate_array(&walk->replacement->scratch,
                                     attribute.count, sizeof(loom_attribute_t),
                                     (void**)&frame->children.attributes);
  }
  return iree_ok_status();
}

static iree_status_t loom_replacement_finish_type(
    loom_value_replacement_walk_t* walk) {
  loom_value_replacement_frame_t* frame = walk->top;
  loom_module_t* module = walk->replacement->module;
  loom_type_id_t result = frame->type.id;
  const loom_type_t type = frame->type.value;
  if (iree_any_bit_set(frame->flags, LOOM_REPLACEMENT_FRAME_FLAG_CHANGED)) {
    if (loom_type_kind(type) == LOOM_TYPE_PARAMETERIZED) {
      loom_type_t result_type;
      IREE_RETURN_IF_ERROR(loom_module_make_parameterized_type(
          module, loom_type_parameterized_descriptor(type),
          frame->children.attributes, frame->count, &result_type, &result));
    } else {
      IREE_RETURN_IF_ERROR(loom_module_intern_topological_type_id(
          module, type, frame->children.types, frame->count, &result));
    }
  } else if (!frame->count) {
    loom_type_t result_type;
    bool changed = false;
    IREE_RETURN_IF_ERROR(loom_replacement_leaf_type(walk->replacement, type,
                                                    &result_type, &changed));
    if (changed) {
      IREE_RETURN_IF_ERROR(
          loom_module_intern_type_id(module, result_type, &result));
    }
  }
  IREE_RETURN_IF_ERROR(
      loom_replacement_insert(walk->replacement, frame->type.id, result));
  *(loom_type_id_t*)frame->destination = result;
  if (frame->parent && result != frame->type.id) {
    frame->parent->flags |= LOOM_REPLACEMENT_FRAME_FLAG_CHANGED;
  }
  loom_replacement_pop(walk);
  return iree_ok_status();
}

static iree_status_t loom_replacement_finish_attribute(
    loom_value_replacement_walk_t* walk) {
  loom_value_replacement_frame_t* frame = walk->top;
  loom_attribute_t result = frame->attribute.value;
  if (iree_any_bit_set(frame->flags, LOOM_REPLACEMENT_FRAME_FLAG_CHANGED) &&
      result.kind != LOOM_ATTR_TYPE) {
    if (result.kind == LOOM_ATTR_DICT) {
      loom_named_attr_t* entries = NULL;
      IREE_RETURN_IF_ERROR(
          iree_arena_allocate_array(frame->attribute.arena, result.count,
                                    sizeof(*entries), (void**)&entries));
      for (uint16_t i = 0; i < result.count; ++i) {
        entries[i] = result.dict_entries[i];
        entries[i].value = frame->children.attributes[i];
      }
      result.dict_entries = entries;
    } else {
      loom_attribute_t* entries = frame->children.attributes;
      if (frame->attribute.arena != &walk->replacement->scratch) {
        IREE_RETURN_IF_ERROR(
            iree_arena_allocate_array(frame->attribute.arena, result.count,
                                      sizeof(*entries), (void**)&entries));
        memcpy(entries, frame->children.attributes,
               result.count * sizeof(*entries));
      }
      if (result.kind == LOOM_ATTR_PARAMETERIZED) {
        result.parameterized_slots = entries;
      } else {
        result.parameterized_array = entries;
      }
    }
  }
  *(loom_attribute_t*)frame->destination = result;
  if (frame->parent) {
    frame->parent->flags |= frame->flags & LOOM_REPLACEMENT_FRAME_FLAG_CHANGED;
  }
  loom_replacement_pop(walk);
  return iree_ok_status();
}

static iree_status_t loom_replacement_run(loom_value_replacement_walk_t* walk) {
  iree_status_t status = iree_ok_status();
  while (walk->top && iree_status_is_ok(status)) {
    loom_value_replacement_frame_t* frame = walk->top;
    if (frame->next == frame->count) {
      status = iree_any_bit_set(frame->flags, LOOM_REPLACEMENT_FRAME_FLAG_TYPE)
                   ? loom_replacement_finish_type(walk)
                   : loom_replacement_finish_attribute(walk);
      continue;
    }
    const uint32_t index = frame->next++;
    if (iree_any_bit_set(frame->flags, LOOM_REPLACEMENT_FRAME_FLAG_TYPE)) {
      if (loom_type_kind(frame->type.value) == LOOM_TYPE_PARAMETERIZED) {
        status = loom_replacement_request_attribute(
            walk, loom_type_parameterized_parameters(frame->type.value)[index],
            &walk->replacement->scratch, &frame->children.attributes[index]);
      } else {
        loom_type_id_t child;
        status = loom_module_intern_type_id(
            walk->replacement->module, frame->type.children[index], &child);
        if (iree_status_is_ok(status)) {
          status = loom_replacement_request_type(walk, child,
                                                 &frame->children.types[index]);
        }
      }
    } else if (frame->attribute.value.kind == LOOM_ATTR_TYPE) {
      status =
          loom_replacement_request_type(walk, frame->attribute.value.type_id,
                                        &frame->attribute.value.type_id);
    } else {
      const loom_attribute_t attribute = frame->attribute.value;
      loom_attribute_t child;
      switch ((loom_attr_kind_t)attribute.kind) {
        case LOOM_ATTR_DICT:
          child = attribute.dict_entries[index].value;
          break;
        case LOOM_ATTR_PARAMETERIZED:
          child = attribute.parameterized_slots[index];
          break;
        default:
          child = attribute.parameterized_array[index];
          break;
      }
      status = loom_replacement_request_attribute(
          walk, child, frame->attribute.arena,
          &frame->children.attributes[index]);
    }
  }
  return status;
}

//===----------------------------------------------------------------------===//
// Shared and standalone substitution lifetimes
//===----------------------------------------------------------------------===//

void loom_value_replacement_initialize(
    loom_module_t* module, loom_value_id_t old_id, loom_value_id_t new_id,
    loom_value_replacement_t* out_replacement) {
  *out_replacement = (loom_value_replacement_t){
      .module = module, .old_id = old_id, .new_id = new_id};
  iree_arena_initialize(module->arena.block_pool, &out_replacement->scratch);
}

void loom_value_replacement_deinitialize(
    loom_value_replacement_t* replacement) {
  iree_arena_deinitialize(&replacement->scratch);
}

iree_status_t loom_value_replacement_type(loom_value_replacement_t* replacement,
                                          loom_type_t type,
                                          loom_type_t* out_type,
                                          bool* out_changed) {
  *out_type = type;
  *out_changed = false;
  if (!loom_type_may_reference_values(type)) {
    return iree_ok_status();
  }
  if (!loom_type_identity_key(type)) {
    return loom_replacement_leaf_type(replacement, type, out_type, out_changed);
  }
  loom_type_id_t source;
  IREE_RETURN_IF_ERROR(
      loom_module_intern_type_id(replacement->module, type, &source));
  loom_type_id_t result;
  loom_value_replacement_walk_t walk = {.replacement = replacement};
  IREE_RETURN_IF_ERROR(loom_replacement_request_type(&walk, source, &result));
  IREE_RETURN_IF_ERROR(loom_replacement_run(&walk));
  if (result != source) {
    *out_type = replacement->module->types.entries[result];
    *out_changed = true;
  }
  return iree_ok_status();
}

iree_status_t loom_value_replacement_attribute(
    loom_value_replacement_t* replacement, loom_attribute_t attribute,
    loom_attribute_t* out_attribute, bool* out_changed) {
  *out_attribute = attribute;
  *out_changed = false;
  loom_attribute_t result;
  loom_value_replacement_walk_t walk = {.replacement = replacement};
  IREE_RETURN_IF_ERROR(loom_replacement_request_attribute(
      &walk, attribute, &replacement->module->arena, &result));
  IREE_RETURN_IF_ERROR(loom_replacement_run(&walk));
  *out_attribute = result;
  *out_changed = memcmp(&attribute, &result, sizeof(result)) != 0;
  return iree_ok_status();
}

static iree_status_t loom_replacement_check_values(const loom_module_t* module,
                                                   loom_value_id_t old_id,
                                                   loom_value_id_t new_id) {
  if (old_id >= module->values.count || new_id >= module->values.count) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "cannot replace references from %%%u to %%%u in a module with %" PRIhsz
        " values",
        (unsigned)old_id, (unsigned)new_id, module->values.count);
  }
  return iree_ok_status();
}

iree_status_t loom_module_replace_type_value_references(
    loom_module_t* module, loom_type_t type, loom_value_id_t old_id,
    loom_value_id_t new_id, loom_type_t* out_type, bool* out_changed) {
  *out_type = type;
  *out_changed = false;
  if (old_id == new_id) {
    return iree_ok_status();
  }
  IREE_RETURN_IF_ERROR(loom_replacement_check_values(module, old_id, new_id));
  loom_value_replacement_t replacement;
  loom_value_replacement_initialize(module, old_id, new_id, &replacement);
  iree_status_t status =
      loom_value_replacement_type(&replacement, type, out_type, out_changed);
  loom_value_replacement_deinitialize(&replacement);
  return status;
}

iree_status_t loom_module_replace_attribute_value_references(
    loom_module_t* module, loom_attribute_t attribute, loom_value_id_t old_id,
    loom_value_id_t new_id, loom_attribute_t* out_attribute,
    bool* out_changed) {
  *out_attribute = attribute;
  *out_changed = false;
  if (old_id == new_id) {
    return iree_ok_status();
  }
  IREE_RETURN_IF_ERROR(loom_replacement_check_values(module, old_id, new_id));
  loom_value_replacement_t replacement;
  loom_value_replacement_initialize(module, old_id, new_id, &replacement);
  iree_status_t status = loom_value_replacement_attribute(
      &replacement, attribute, out_attribute, out_changed);
  loom_value_replacement_deinitialize(&replacement);
  return status;
}
