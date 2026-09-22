// Copyright 2026 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "loom/format/bytecode/writer/type_index.h"

#include <string.h>

#include "loom/ir/module.h"
#include "loom/ir/parameterized_type.h"
#include "loom/ir/structural_hash.h"

// Temporary graph construction capacity; the index retains the completed edges.
typedef struct loom_bytecode_type_graph_t {
  // Index receiving exact canonical storage and immediate dependency facts.
  loom_bytecode_type_index_t* index;
  // Scratch arena owning graph and index allocations.
  iree_arena_allocator_t* arena;
  // Ordered immediate dependency node IDs.
  uint32_t* dependencies;
  // Number of populated dependencies.
  iree_host_size_t count;
  // Allocated dependency capacity.
  iree_host_size_t capacity;
} loom_bytecode_type_graph_t;

static uint32_t loom_bytecode_type_storage_hash(loom_type_t type) {
  uint32_t hash = loom_structural_hash_initialize();
  hash = loom_structural_hash_mix_u32(hash, type.header);
  hash = loom_structural_hash_mix_u32(
      hash, type.encoding_id | ((uint32_t)type.encoding_flags << 16));
  hash = loom_structural_hash_mix_u64(hash, type.dims[0]);
  hash = loom_structural_hash_mix_u64(hash, type.dims[1]);
  return loom_structural_hash_finalize(hash);
}

static bool loom_bytecode_type_storage_equal(loom_type_t a, loom_type_t b) {
  return a.header == b.header && a.encoding_id == b.encoding_id &&
         a.encoding_flags == b.encoding_flags && a.dims[0] == b.dims[0] &&
         a.dims[1] == b.dims[1];
}

static uint32_t loom_bytecode_type_storage_lookup(
    const loom_bytecode_type_index_t* index, loom_type_t type, uint32_t hash) {
  if (index->slot_capacity == 0) {
    return UINT32_MAX;
  }
  iree_host_size_t mask = index->slot_capacity - 1;
  for (iree_host_size_t slot = hash & mask; index->slots[slot] != UINT32_MAX;
       slot = (slot + 1) & mask) {
    uint32_t node = index->slots[slot];
    if (index->nodes[node].storage_hash == hash &&
        loom_bytecode_type_storage_equal(index->nodes[node].type, type)) {
      return node;
    }
  }
  return UINT32_MAX;
}

static iree_status_t loom_bytecode_type_storage_insert(
    loom_bytecode_type_graph_t* graph, loom_type_t type, uint32_t* out_node) {
  loom_bytecode_type_index_t* index = graph->index;
  const uint32_t hash = loom_bytecode_type_storage_hash(type);
  uint32_t existing = loom_bytecode_type_storage_lookup(index, type, hash);
  if (existing != UINT32_MAX) {
    *out_node = existing;
    return iree_ok_status();
  }
  if (index->count >= UINT32_MAX) {
    return iree_make_status(
        IREE_STATUS_RESOURCE_EXHAUSTED,
        "bytecode type storage graph exceeds 32-bit indices");
  }
  if (index->count >= index->capacity) {
    IREE_RETURN_IF_ERROR(iree_arena_grow_array(
        graph->arena, index->count, /*minimum_capacity=*/16,
        sizeof(*index->nodes), &index->capacity, (void**)&index->nodes));
  }
  if (index->count + 1 > index->slot_capacity * 3 / 4) {
    iree_host_size_t capacity =
        index->slot_capacity ? index->slot_capacity * 2 : 16;
    uint32_t* slots = NULL;
    IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
        graph->arena, capacity, sizeof(*slots), (void**)&slots));
    memset(slots, 0xFF, capacity * sizeof(*slots));
    for (iree_host_size_t i = 0; i < index->count; ++i) {
      iree_host_size_t slot = index->nodes[i].storage_hash & (capacity - 1);
      while (slots[slot] != UINT32_MAX) {
        slot = (slot + 1) & (capacity - 1);
      }
      slots[slot] = (uint32_t)i;
    }
    index->slots = slots;
    index->slot_capacity = capacity;
  }
  uint32_t node = (uint32_t)index->count++;
  index->nodes[node] = (loom_bytecode_type_node_t){
      .type = type,
      .storage_hash = hash,
      .module_index = LOOM_TYPE_ID_INVALID,
  };
  iree_host_size_t slot = hash & (index->slot_capacity - 1);
  while (index->slots[slot] != UINT32_MAX) {
    slot = (slot + 1) & (index->slot_capacity - 1);
  }
  index->slots[slot] = node;
  *out_node = node;
  return iree_ok_status();
}

static iree_status_t loom_bytecode_type_graph_add_dependency(
    loom_bytecode_type_graph_t* graph, loom_type_t type) {
  uint32_t node = UINT32_MAX;
  IREE_RETURN_IF_ERROR(loom_bytecode_type_storage_insert(graph, type, &node));
  if (graph->count >= graph->capacity) {
    IREE_RETURN_IF_ERROR(iree_arena_grow_array(
        graph->arena, graph->count, /*minimum_capacity=*/16,
        sizeof(*graph->dependencies), &graph->capacity,
        (void**)&graph->dependencies));
  }
  graph->dependencies[graph->count++] = node;
  return iree_ok_status();
}

// Aggregate attributes have bounded nesting. TYPE leaves become graph edges,
// never recursive descent into the referenced type's payload.
static iree_status_t loom_bytecode_type_graph_add_attribute(
    loom_bytecode_type_graph_t* graph, const loom_attribute_t* attr,
    uint8_t depth) {
  switch ((loom_attr_kind_t)attr->kind) {
    case LOOM_ATTR_TYPE:
      if (attr->type_id < graph->index->module->types.count) {
        return loom_bytecode_type_graph_add_dependency(
            graph,
            loom_type_table_get(&graph->index->module->types, attr->type_id));
      }
      return iree_ok_status();
    case LOOM_ATTR_DICT:
    case LOOM_ATTR_PARAMETERIZED:
    case LOOM_ATTR_PARAMETERIZED_ARRAY:
      break;
    default:
      return iree_ok_status();
  }
  if (depth >= LOOM_ATTR_AGGREGATE_MAX_NESTING_DEPTH) {
    return iree_ok_status();
  }
  iree_status_t status = iree_ok_status();
  for (uint16_t i = 0; i < attr->count && iree_status_is_ok(status); ++i) {
    const loom_attribute_t* child = NULL;
    switch ((loom_attr_kind_t)attr->kind) {
      case LOOM_ATTR_DICT:
        child = &attr->dict_entries[i].value;
        break;
      case LOOM_ATTR_PARAMETERIZED:
        child = &attr->parameterized_slots[i];
        break;
      default:
        child = &attr->parameterized_array[i];
        break;
    }
    status = loom_bytecode_type_graph_add_attribute(graph, child, depth + 1);
  }
  return status;
}

static iree_status_t loom_bytecode_type_graph_add_children(
    loom_bytecode_type_graph_t* graph, loom_type_t type) {
  const loom_type_t* children = NULL;
  iree_host_size_t count = 0;
  switch (loom_type_kind(type)) {
    case LOOM_TYPE_FUNCTION: {
      const loom_func_type_data_t* data = loom_type_func_data(type);
      if (data) {
        children = data->types;
        count = (iree_host_size_t)data->arg_count + data->result_count;
      }
      break;
    }
    case LOOM_TYPE_DIALECT:
      children = loom_type_dialect_params(type);
      count = children ? loom_type_dialect_param_count(type) : 0;
      break;
    case LOOM_TYPE_REGISTER:
      children = loom_type_register_value_type(type);
      count = children ? 1 : 0;
      break;
    case LOOM_TYPE_PARAMETERIZED: {
      const loom_attribute_t* parameters =
          loom_type_parameterized_parameters(type);
      if (!parameters) {
        return iree_ok_status();
      }
      iree_status_t status = iree_ok_status();
      for (uint8_t i = 0; i < loom_type_parameterized_parameter_count(type) &&
                          iree_status_is_ok(status);
           ++i) {
        status =
            loom_bytecode_type_graph_add_attribute(graph, &parameters[i], 0);
      }
      return status;
    }
    default:
      break;
  }
  iree_status_t status = iree_ok_status();
  for (iree_host_size_t i = 0; i < count && iree_status_is_ok(status); ++i) {
    status = loom_bytecode_type_graph_add_dependency(graph, children[i]);
  }
  return status;
}

iree_status_t loom_bytecode_type_index_initialize(
    const loom_module_t* module, iree_arena_allocator_t* arena,
    loom_bytecode_type_index_t* out_index) {
  memset(out_index, 0, sizeof(*out_index));
  out_index->module = module;
  if (module->types.count == 0) {
    return iree_ok_status();
  }
  loom_bytecode_type_graph_t graph = {
      .index = out_index,
      .arena = arena,
  };
  // Each canonical module type needs one storage node. Start with the table
  // size instead of abandoning successively doubled arena arrays.
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(arena, module->types.count,
                                                 sizeof(*out_index->nodes),
                                                 (void**)&out_index->nodes));
  out_index->capacity = module->types.count;
  out_index->slot_capacity =
      iree_host_size_next_power_of_two((module->types.count * 4 + 2) / 3);
  if (out_index->slot_capacity < 16) {
    out_index->slot_capacity = 16;
  }
  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(
      arena, out_index->slot_capacity, sizeof(*out_index->slots),
      (void**)&out_index->slots));
  memset(out_index->slots, 0xFF,
         out_index->slot_capacity * sizeof(*out_index->slots));
  for (iree_host_size_t i = 0; i < module->types.count; ++i) {
    uint32_t node = UINT32_MAX;
    IREE_RETURN_IF_ERROR(loom_bytecode_type_storage_insert(
        &graph, loom_type_table_get(&module->types, i), &node));
    if (out_index->nodes[node].module_index == LOOM_TYPE_ID_INVALID) {
      out_index->nodes[node].module_index = (loom_type_id_t)i;
    }
    out_index->nodes[node].has_bindings =
        loom_type_table_dependencies(&module->types, i) != 0;
  }
  for (iree_host_size_t i = 0; i < out_index->count; ++i) {
    const loom_type_t type = out_index->nodes[i].type;
    iree_host_size_t begin = graph.count;
    IREE_RETURN_IF_ERROR(loom_bytecode_type_graph_add_children(&graph, type));
    out_index->nodes[i].dependencies.explicit_count = graph.count - begin;
    if (loom_type_is_shaped(type)) {
      IREE_RETURN_IF_ERROR(loom_bytecode_type_graph_add_dependency(
          &graph, loom_type_scalar(loom_type_element_type(type))));
    }
    out_index->nodes[i].dependencies.begin = begin;
    out_index->nodes[i].dependencies.count = graph.count - begin;
  }
  out_index->dependencies = graph.dependencies;

  IREE_RETURN_IF_ERROR(iree_arena_allocate_array(arena, out_index->count,
                                                 sizeof(*out_index->stack),
                                                 (void**)&out_index->stack));
  return iree_ok_status();
}

const loom_bytecode_type_node_t* loom_bytecode_type_index_lookup_node(
    const loom_bytecode_type_index_t* index, loom_type_t type) {
  uint32_t node = loom_bytecode_type_storage_lookup(
      index, type, loom_bytecode_type_storage_hash(type));
  return node == UINT32_MAX ? NULL : &index->nodes[node];
}

loom_type_id_t loom_bytecode_type_index_lookup(
    const loom_bytecode_type_index_t* index, loom_type_t type) {
  const loom_bytecode_type_node_t* node =
      loom_bytecode_type_index_lookup_node(index, type);
  return node ? node->module_index : LOOM_TYPE_ID_INVALID;
}
